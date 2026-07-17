// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/standalone_sender/streamer_plus/capture.h"

#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <sstream>

#include <gst/app/gstappsink.h>
#include <gst/audio/audio.h>
#include <gst/gst.h>
#include <gst/video/video.h>

namespace openscreen::cast::streamer_plus {
namespace {

bool InitializeGstreamer(std::string* error) {
  static std::once_flag once;
  static bool initialized = false;
  static std::string* const initialization_error = new std::string;
  std::call_once(once, [] {
    GError* gst_error = nullptr;
    initialized = gst_init_check(nullptr, nullptr, &gst_error);
    if (!initialized) {
      *initialization_error = gst_error == nullptr ? "gst_init_check failed"
                                                   : gst_error->message;
    }
    if (gst_error != nullptr) {
      g_error_free(gst_error);
    }
  });
  if (!initialized) {
    *error = *initialization_error;
  }
  return initialized;
}

std::string GstreamerError(const char* operation, GError* error) {
  const std::string message = error == nullptr ? "unknown GStreamer error"
                                                : error->message;
  if (error != nullptr) {
    g_error_free(error);
  }
  return std::string(operation) + ": " + message;
}

}  // namespace

class GstreamerCapture::Impl {
 public:
  explicit Impl(size_t queue_capacity)
      : video_queue(queue_capacity), audio_queue(queue_capacity) {}

  ~Impl() { Stop(); }

  bool Start(const std::string& description) {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (pipeline != nullptr) {
      SetError("capture is already running");
      return false;
    }
    if (!InitializeGstreamer(&error)) {
      return false;
    }

    GError* gst_error = nullptr;
    pipeline = gst_parse_launch(description.c_str(), &gst_error);
    if (pipeline == nullptr) {
      SetError(GstreamerError("failed to parse capture pipeline", gst_error));
      return false;
    }
    if (gst_error != nullptr) {
      SetError(GstreamerError("failed to parse capture pipeline", gst_error));
      gst_object_unref(pipeline);
      pipeline = nullptr;
      return false;
    }

    return ConfigureAndStart(5000);
  }

  bool StartDesktop(const DesktopCaptureOptions& options) {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (pipeline != nullptr) {
      SetError("capture is already running");
      return false;
    }
    if (!InitializeGstreamer(&error)) {
      return false;
    }
    if (options.pipewire_fd < 0 || fcntl(options.pipewire_fd, F_GETFD) == -1 ||
        options.target_object.empty() || options.width == 0 || options.height == 0 ||
        options.frames_per_second == 0 || options.audio_source.empty() ||
        options.startup_timeout_ms == 0) {
      SetError("invalid desktop capture options");
      return false;
    }
    pipeline = GST_ELEMENT(gst_pipeline_new("streamer_plus_desktop_capture"));
    if (pipeline == nullptr) {
      SetError("failed to create GStreamer desktop capture pipeline");
      return false;
    }
    auto make = [this](const char* factory, const char* name) -> GstElement* {
      GstElement* element = gst_element_factory_make(factory, name);
      if (element == nullptr) {
        SetError(std::string("missing GStreamer element '") + factory + "'" +
                 (std::string(factory) == "pipewiresrc"
                      ? "; install/enable Fedora pipewire-gstreamer"
                      : std::string(factory) == "pulsesrc"
                            ? "; install/enable Fedora gstreamer1-plugins-good"
                            : ""));
        return nullptr;
      }
      if (!gst_bin_add(GST_BIN(pipeline), element)) {
        SetError(std::string("failed to add GStreamer element '") + factory +
                 "' to desktop capture pipeline");
        gst_object_unref(element);
        return nullptr;
      }
      return element;
    };
    GstElement *source = make("pipewiresrc", "portal_video_source"),
               *video_queue_element = make("queue", "video_queue"),
               *convert = make("videoconvert", "video_convert"),
               *scale = make("videoscale", "video_scale"),
               *rate = make("videorate", "video_rate"),
               *video_capsfilter = make("capsfilter", "video_caps"),
               *video_appsink = make("appsink", "video_sink"),
               *pulse = make("pulsesrc", "system_audio_source"),
               *audio_queue_element = make("queue", "audio_queue"),
               *audio_convert = make("audioconvert", "audio_convert"),
               *resample = make("audioresample", "audio_resample"),
               *audio_capsfilter = make("capsfilter", "audio_caps"),
               *audio_appsink = make("appsink", "audio_sink");
    if (!source || !video_queue_element || !convert || !scale || !rate ||
        !video_capsfilter || !video_appsink || !pulse || !audio_queue_element ||
        !audio_convert || !resample || !audio_capsfilter || !audio_appsink) {
      ReleasePipeline();
      return false;
    }
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(source), "fd") == nullptr ||
        g_object_class_find_property(G_OBJECT_GET_CLASS(source), "target-object") == nullptr) {
      SetError("installed pipewiresrc lacks required fd or target-object property");
      ReleasePipeline();
      return false;
    }
    GstCaps* video_caps = gst_caps_new_simple(
        "video/x-raw", "format", G_TYPE_STRING, "I420", "width", G_TYPE_INT,
        static_cast<int>(options.width), "height", G_TYPE_INT,
        static_cast<int>(options.height), "framerate", GST_TYPE_FRACTION,
        static_cast<int>(options.frames_per_second), 1, nullptr);
    GstCaps* audio_caps = gst_caps_new_simple(
        "audio/x-raw", "format", G_TYPE_STRING, "F32LE", "layout", G_TYPE_STRING,
        "interleaved", "channels", G_TYPE_INT, 2, "rate", G_TYPE_INT, 48000, nullptr);
    if (!video_caps || !audio_caps) {
      SetError("failed to create GStreamer pipeline caps");
      if (video_caps) gst_caps_unref(video_caps);
      if (audio_caps) gst_caps_unref(audio_caps);
      ReleasePipeline();
      return false;
    }
    g_object_set(source, "fd", options.pipewire_fd, "target-object",
                 options.target_object.c_str(), nullptr);
    g_object_set(video_queue_element, "leaky", 2, "max-size-buffers", 2u,
                 "max-size-bytes", 0u, "max-size-time", guint64{0}, nullptr);
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(scale), "add-borders")) {
      g_object_set(scale, "add-borders", TRUE, nullptr);
    }
    g_object_set(video_capsfilter, "caps", video_caps, nullptr);
    g_object_set(pulse, "device", options.audio_source.c_str(), nullptr);
    g_object_set(audio_queue_element, "leaky", 2, "max-size-buffers", 4u,
                 "max-size-bytes", 0u, "max-size-time", guint64{0}, nullptr);
    g_object_set(audio_capsfilter, "caps", audio_caps, nullptr);
    gst_caps_unref(video_caps);
    gst_caps_unref(audio_caps);
    if (!gst_element_link_many(source, video_queue_element, convert, scale, rate,
                               video_capsfilter, video_appsink, nullptr) ||
        !gst_element_link_many(pulse, audio_queue_element, audio_convert, resample,
                               audio_capsfilter, audio_appsink, nullptr)) {
      SetError("failed to link desktop capture pipeline");
      ReleasePipeline();
      return false;
    }
    return ConfigureAndStart(options.startup_timeout_ms);
  }

  bool ConfigureAndStart(uint32_t startup_timeout_ms) {
    if (!GST_IS_BIN(pipeline)) {
      SetError("capture pipeline must be a GStreamer bin");
      ReleasePipeline();
      return false;
    }
    GstElement* video_element =
        gst_bin_get_by_name(GST_BIN(pipeline), "video_sink");
    GstElement* audio_element =
        gst_bin_get_by_name(GST_BIN(pipeline), "audio_sink");
    if (video_element == nullptr || audio_element == nullptr ||
        !GST_IS_APP_SINK(video_element) || !GST_IS_APP_SINK(audio_element)) {
      if (video_element != nullptr) {
        gst_object_unref(video_element);
      }
      if (audio_element != nullptr) {
        gst_object_unref(audio_element);
      }
      SetError("capture pipeline must contain appsinks named video_sink and audio_sink");
      ReleasePipeline();
      return false;
    }
    video_sink = GST_APP_SINK(video_element);
    audio_sink = GST_APP_SINK(audio_element);
    g_object_set(video_sink, "max-buffers", 4u, "drop", TRUE, "sync", FALSE,
                 nullptr);
    g_object_set(audio_sink, "max-buffers", 4u, "drop", TRUE, "sync", FALSE,
                 nullptr);

    const GstStateChangeReturn transition =
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (transition == GST_STATE_CHANGE_FAILURE) {
      SetError("failed to start capture pipeline");
      gst_element_set_state(pipeline, GST_STATE_NULL);
      ReleasePipeline();
      return false;
    }
    GstState state = GST_STATE_NULL;
    const GstStateChangeReturn startup = gst_element_get_state(
        pipeline, &state, nullptr, GST_MSECOND * startup_timeout_ms);
    const bool runtime_error = PollRuntimeError();
    if (startup == GST_STATE_CHANGE_FAILURE || state != GST_STATE_PLAYING ||
        runtime_error) {
      if (Error().empty()) SetError("capture pipeline startup timed out or failed");
      gst_element_set_state(pipeline, GST_STATE_NULL);
      ReleasePipeline();
      return false;
    }
    stop_requested.store(false);
    const int video_worker_error =
        pthread_create(&video_thread, nullptr, PullVideoWorker, this);
    if (video_worker_error != 0) {
      SetError("failed to create video capture worker: " +
               std::string(std::strerror(video_worker_error)));
      gst_element_set_state(pipeline, GST_STATE_NULL);
      ReleasePipeline();
      return false;
    }
    video_thread_started = true;
    const int audio_worker_error =
        pthread_create(&audio_thread, nullptr, PullAudioWorker, this);
    if (audio_worker_error != 0) {
      SetError("failed to create audio capture worker: " +
               std::string(std::strerror(audio_worker_error)));
      stop_requested.store(true);
      gst_element_set_state(pipeline, GST_STATE_NULL);
      pthread_join(video_thread, nullptr);
      video_thread_started = false;
      ReleasePipeline();
      return false;
    }
    audio_thread_started = true;
    return true;
  }

  bool PollRuntimeError() {
    if (!Error().empty()) return true;
    if (pipeline == nullptr) return false;
    GstBus* bus = gst_element_get_bus(pipeline);
    if (bus == nullptr) return false;
    GstMessage* message = gst_bus_pop_filtered(
        bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    gst_object_unref(bus);
    if (message == nullptr) return false;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
      SetError("capture pipeline reached unexpected EOS");
    } else {
      GError* gst_error = nullptr;
      gchar* debug = nullptr;
      gst_message_parse_error(message, &gst_error, &debug);
      const gchar* name = GST_OBJECT_NAME(GST_MESSAGE_SRC(message));
      SetError("capture pipeline error from " + std::string(name ? name : "unknown") +
               ": " + (gst_error ? gst_error->message : "unknown") +
               (debug ? "; " + std::string(debug) : ""));
      if (gst_error) g_error_free(gst_error);
      g_free(debug);
    }
    gst_message_unref(message);
    return true;
  }

  void Stop() {
    GstElement* pipeline_to_stop = nullptr;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      if (pipeline == nullptr) {
        return;
      }
      stop_requested.store(true);
      pipeline_to_stop = pipeline;
    }
    gst_element_set_state(pipeline_to_stop, GST_STATE_NULL);
    if (video_thread_started) {
      pthread_join(video_thread, nullptr);
      video_thread_started = false;
    }
    if (audio_thread_started) {
      pthread_join(audio_thread, nullptr);
      audio_thread_started = false;
    }
    std::lock_guard<std::mutex> lock(state_mutex);
    ReleasePipeline();
  }

  static void* PullVideoWorker(void* data) {
    static_cast<Impl*>(data)->PullVideo();
    return nullptr;
  }

  static void* PullAudioWorker(void* data) {
    static_cast<Impl*>(data)->PullAudio();
    return nullptr;
  }

  void PullVideo() {
    while (!stop_requested.load()) {
      GstSample* sample = gst_app_sink_try_pull_sample(
          video_sink, 100 * GST_MSECOND);
      if (sample == nullptr) {
        continue;
      }
      CopyVideo(sample);
      gst_sample_unref(sample);
    }
  }

  void PullAudio() {
    while (!stop_requested.load()) {
      GstSample* sample = gst_app_sink_try_pull_sample(
          audio_sink, 100 * GST_MSECOND);
      if (sample == nullptr) {
        continue;
      }
      CopyAudio(sample);
      gst_sample_unref(sample);
    }
  }

  void CopyVideo(GstSample* sample) {
    GstCaps* caps = gst_sample_get_caps(sample);
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstVideoInfo info;
    if (caps == nullptr || buffer == nullptr || !gst_video_info_from_caps(&info, caps) ||
        GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_I420) {
      format_error.store(true);
      SetError("video capture format error: expected an I420 sample");
      return;
    }
    const uint32_t width = GST_VIDEO_INFO_WIDTH(&info);
    const uint32_t height = GST_VIDEO_INFO_HEIGHT(&info);
    GstVideoFrame mapped;
    if (!gst_video_frame_map(&mapped, &info, buffer, GST_MAP_READ)) {
      format_error.store(true);
      SetError("video capture format error: failed to map I420 buffer");
      return;
    }
    VideoFrame frame;
    frame.width = width;
    frame.height = height;
    frame.timestamp_ns = GST_BUFFER_PTS(buffer);
    if (frame.timestamp_ns == GST_CLOCK_TIME_NONE) {
      gst_video_frame_unmap(&mapped);
      timestamp_error.store(true);
      SetError("video capture timestamp error: sample has no presentation timestamp");
      return;
    }
    const uint32_t chroma_width = (width + 1) / 2;
    const uint32_t chroma_height = (height + 1) / 2;
    frame.i420_bytes.resize(width * height + 2 * chroma_width * chroma_height);
    uint8_t* destination = frame.i420_bytes.data();
    const uint32_t plane_widths[] = {width, chroma_width, chroma_width};
    const uint32_t plane_heights[] = {height, chroma_height, chroma_height};
    for (int plane = 0; plane < 3; ++plane) {
      const uint8_t* source = reinterpret_cast<const uint8_t*>(
          GST_VIDEO_FRAME_PLANE_DATA(&mapped, plane));
      const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&mapped, plane);
      if (source == nullptr || stride <= 0 ||
          static_cast<uint32_t>(stride) < plane_widths[plane]) {
        gst_video_frame_unmap(&mapped);
        format_error.store(true);
        SetError("video capture format error: invalid I420 plane layout");
        return;
      }
      for (uint32_t row = 0; row < plane_heights[plane]; ++row) {
        std::memcpy(destination, source + row * stride, plane_widths[plane]);
        destination += plane_widths[plane];
      }
    }
    gst_video_frame_unmap(&mapped);
    const uint64_t previous = last_video_timestamp.exchange(frame.timestamp_ns);
    if (have_video_timestamp.exchange(true) && frame.timestamp_ns <= previous) {
      timestamp_error.store(true);
      SetError("video capture timestamp error: presentation timestamps are not monotonic");
      return;
    }
    video_queue.Push(std::move(frame));
    ++video_frames;
  }

  void CopyAudio(GstSample* sample) {
    GstCaps* caps = gst_sample_get_caps(sample);
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstAudioInfo info;
    if (caps == nullptr || buffer == nullptr || !gst_audio_info_from_caps(&info, caps) ||
        GST_AUDIO_INFO_FORMAT(&info) != GST_AUDIO_FORMAT_F32LE ||
        GST_AUDIO_INFO_LAYOUT(&info) != GST_AUDIO_LAYOUT_INTERLEAVED ||
        GST_AUDIO_INFO_CHANNELS(&info) != 2 || GST_AUDIO_INFO_RATE(&info) != 48000) {
      format_error.store(true);
      SetError("audio capture format error: expected interleaved F32LE stereo at 48000 Hz");
      return;
    }
    GstMapInfo mapped;
    if (!gst_buffer_map(buffer, &mapped, GST_MAP_READ)) {
      format_error.store(true);
      SetError("audio capture format error: failed to map F32LE buffer");
      return;
    }
    if (mapped.size == 0 || mapped.size % (sizeof(float) * 2) != 0) {
      gst_buffer_unmap(buffer, &mapped);
      format_error.store(true);
      SetError("audio capture format error: malformed interleaved stereo buffer size");
      return;
    }
    AudioFrame frame;
    frame.channels = 2;
    frame.sample_rate = 48000;
    frame.timestamp_ns = GST_BUFFER_PTS(buffer);
    if (frame.timestamp_ns == GST_CLOCK_TIME_NONE) {
      gst_buffer_unmap(buffer, &mapped);
      timestamp_error.store(true);
      SetError("audio capture timestamp error: sample has no presentation timestamp");
      return;
    }
    frame.interleaved_samples.resize(mapped.size / sizeof(float));
    std::memcpy(frame.interleaved_samples.data(), mapped.data, mapped.size);
    gst_buffer_unmap(buffer, &mapped);
    const uint64_t previous = last_audio_timestamp.exchange(frame.timestamp_ns);
    if (have_audio_timestamp.exchange(true) && frame.timestamp_ns <= previous) {
      timestamp_error.store(true);
      SetError("audio capture timestamp error: presentation timestamps are not monotonic");
      return;
    }
    audio_queue.Push(std::move(frame));
    ++audio_frames;
  }

  CaptureTelemetry Telemetry() const {
    return {.video_frames = video_frames.load(),
            .audio_frames = audio_frames.load(),
            .video_dropped = video_queue.dropped(),
            .audio_dropped = audio_queue.dropped(),
            .format_error = format_error.load(),
            .timestamp_error = timestamp_error.load()};
  }

  void SetError(std::string value) {
    std::lock_guard<std::mutex> lock(error_mutex);
    if (error.empty()) {
      error = std::move(value);
    }
  }

  std::string Error() const {
    std::lock_guard<std::mutex> lock(error_mutex);
    return error;
  }

 private:
  void ReleasePipeline() {
    if (video_sink != nullptr) {
      gst_object_unref(video_sink);
      video_sink = nullptr;
    }
    if (audio_sink != nullptr) {
      gst_object_unref(audio_sink);
      audio_sink = nullptr;
    }
    if (pipeline != nullptr) {
      gst_object_unref(pipeline);
      pipeline = nullptr;
    }
  }

 public:
  DropOldQueue<VideoFrame> video_queue;
  DropOldQueue<AudioFrame> audio_queue;

 private:
  mutable std::mutex state_mutex;
  mutable std::mutex error_mutex;
  std::string error;
  GstElement* pipeline = nullptr;
  GstAppSink* video_sink = nullptr;
  GstAppSink* audio_sink = nullptr;
  pthread_t video_thread{};
  pthread_t audio_thread{};
  bool video_thread_started = false;
  bool audio_thread_started = false;
  std::atomic<bool> stop_requested = false;
  std::atomic<uint64_t> video_frames = 0;
  std::atomic<uint64_t> audio_frames = 0;
  std::atomic<uint64_t> last_video_timestamp = 0;
  std::atomic<uint64_t> last_audio_timestamp = 0;
  std::atomic<bool> have_video_timestamp = false;
  std::atomic<bool> have_audio_timestamp = false;
  std::atomic<bool> format_error = false;
  std::atomic<bool> timestamp_error = false;
};

GstreamerCapture::GstreamerCapture(size_t queue_capacity)
    : impl_(std::make_unique<Impl>(queue_capacity)) {}

GstreamerCapture::~GstreamerCapture() = default;

bool GstreamerCapture::Start(const std::string& pipeline_description) {
  return impl_->Start(pipeline_description);
}

bool GstreamerCapture::StartDesktop(const DesktopCaptureOptions& options) {
  return impl_->StartDesktop(options);
}

void GstreamerCapture::Stop() {
  impl_->Stop();
}

bool GstreamerCapture::PollRuntimeError() {
  return impl_->PollRuntimeError();
}

std::optional<VideoFrame> GstreamerCapture::TakeLatestVideo() {
  return impl_->video_queue.TakeLatest();
}

std::optional<AudioFrame> GstreamerCapture::TakeLatestAudio() {
  return impl_->audio_queue.TakeLatest();
}

CaptureTelemetry GstreamerCapture::telemetry() const {
  return impl_->Telemetry();
}

std::string GstreamerCapture::last_error() const {
  return impl_->Error();
}

std::string GstreamerCapture::SyntheticPipeline() {
  return "videotestsrc is-live=true ! videoconvert ! "
         "video/x-raw,format=I420,width=320,height=240,framerate=30/1 ! "
         "appsink name=video_sink audiotestsrc is-live=true ! audioconvert ! "
         "audioresample ! audio/x-raw,format=F32LE,layout=interleaved,channels=2,rate=48000 ! "
         "appsink name=audio_sink";
}

}  // namespace openscreen::cast::streamer_plus
