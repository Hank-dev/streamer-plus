// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STANDALONE_SENDER_STREAMER_PLUS_CAPTURE_H_
#define CAST_STANDALONE_SENDER_STREAMER_PLUS_CAPTURE_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cast/standalone_sender/streamer_plus/drop_old_queue.h"

namespace openscreen::cast::streamer_plus {

struct VideoFrame {
  uint32_t width = 0;
  uint32_t height = 0;
  // Tightly packed I420: Y, then U, then V. Plane strides are width,
  // ceil(width / 2), and ceil(width / 2), respectively.
  std::vector<uint8_t> i420_bytes;
  uint64_t timestamp_ns = 0;
};

struct AudioFrame {
  uint32_t channels = 0;
  uint32_t sample_rate = 0;
  std::vector<float> interleaved_samples;
  uint64_t timestamp_ns = 0;
};

struct CaptureTelemetry {
  uint64_t video_frames = 0;
  uint64_t audio_frames = 0;
  uint64_t video_dropped = 0;
  uint64_t audio_dropped = 0;
  bool format_error = false;
  bool timestamp_error = false;
};

struct DesktopCaptureOptions {
  int pipewire_fd = -1;  // Borrowed; PortalScreencast keeps it open.
  std::string target_object;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t frames_per_second = 0;
  std::string audio_source;
  uint32_t startup_timeout_ms = 5000;
};

// Pulls the two named appsinks from a supplied pipeline into owned frames.
class GstreamerCapture {
 public:
  explicit GstreamerCapture(size_t queue_capacity = 4);
  ~GstreamerCapture();

  GstreamerCapture(const GstreamerCapture&) = delete;
  GstreamerCapture& operator=(const GstreamerCapture&) = delete;

  bool Start(const std::string& pipeline_description);
  bool StartDesktop(const DesktopCaptureOptions& options);
  void Stop();

  // Returns true when the bus has reported an error or unexpected EOS.
  bool PollRuntimeError();

  std::optional<VideoFrame> TakeLatestVideo();
  std::optional<AudioFrame> TakeLatestAudio();
  CaptureTelemetry telemetry() const;
  std::string last_error() const;

  static std::string SyntheticPipeline();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace openscreen::cast::streamer_plus

#endif  // CAST_STANDALONE_SENDER_STREAMER_PLUS_CAPTURE_H_
