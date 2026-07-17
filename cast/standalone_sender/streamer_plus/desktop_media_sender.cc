// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/standalone_sender/streamer_plus/desktop_media_sender.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

#include "cast/streaming/public/constants.h"
#include "cast/streaming/public/environment.h"
#include "platform/api/time.h"
#include "util/osp_logging.h"

namespace openscreen::cast::streamer_plus {

namespace {

using std::chrono::milliseconds;
using std::chrono::nanoseconds;

// Poll interval for capture drain.
constexpr Clock::duration kPollInterval = milliseconds(5);

// Congestion control interval.
constexpr Clock::duration kCongestionInterval = milliseconds(500);

// Telemetry interval.
constexpr Clock::duration kTelemetryInterval = milliseconds(1000);

// Watchdog deadlines.
constexpr Clock::duration kInitialPairDeadline = milliseconds(2000);
constexpr Clock::duration kAudioWatchdogDeadline = milliseconds(1000);
constexpr Clock::duration kVideoWatchdogDeadline = milliseconds(2000);

// VPX frame duration clamp bounds.
constexpr Clock::duration kMinFrameDuration = milliseconds(1);
constexpr Clock::duration kMaxFrameDuration = milliseconds(125);

// Audio gap threshold: more than 1 Cast frame (10 ms) of difference.
constexpr Clock::duration kAudioGapThreshold = milliseconds(10);

// VPX encoder implementation floor.
constexpr int kMinVideoBitrate = 1024;

// 80% of estimated bandwidth is usable (leave headroom for overhead).
constexpr double kUsableBandwidthFraction = 0.8;

// Max bitrate growth per congestion interval.
constexpr double kMaxBitrateGrowthFactor = 0.10;

}  // namespace

DesktopMediaSender::DesktopMediaSender(
    Environment& environment,
    const ConnectionSettings& settings,
    const SenderSession* session,
    SenderSession::ConfiguredSenders senders,
    capture_recommendations::Recommendations recommendations,
    GstreamerCapture* capture,
    ShutdownCallback shutdown_callback)
    : env_(environment),
      settings_(settings),
      session_(session),
      capture_(capture),
      shutdown_callback_(std::move(shutdown_callback)),
      recommendations_(recommendations),
      poll_alarm_(&Clock::now, env_.task_runner()),
      congestion_alarm_(&Clock::now, env_.task_runner()),
      telemetry_alarm_(&Clock::now, env_.task_runner()),
      cli_video_cap_bps_(settings.video_max_bitrate > 0
                             ? settings.video_max_bitrate
                             : (settings.max_bitrate > 0
                                    ? settings.max_bitrate
                                    : 8000000)),
      cli_audio_cap_bps_(settings.audio_bitrate > 0 ? settings.audio_bitrate
                                                    : 128000) {
  OSP_DCHECK(capture_);
  OSP_DCHECK(shutdown_callback_);

  // Create the VPX encoder if we have a video sender.
  if (senders.video_sender) {
    StreamingVpxEncoder::Parameters params;
    params.codec = VideoCodec::kVp8;
    video_encoder_ = std::make_unique<StreamingVpxEncoder>(
        params, env_.task_runner(), std::move(senders.video_sender));
    video_encoder_->SetTargetBitrate(cli_video_cap_bps_);
    counters_.video_target_bps = cli_video_cap_bps_;
  }

  // Create the Opus encoder if we have an audio sender.
  if (senders.audio_sender) {
    // Negotiated channels come from the configured audio config.
    const int channels =
        senders.audio_config.channels > 0
            ? senders.audio_config.channels
            : kDefaultAudioChannels;
    audio_encoder_ = std::make_unique<StreamingOpusEncoder>(
        channels,
        StreamingOpusEncoder::kDefaultCastAudioFramesPerSecond,
        std::move(senders.audio_sender));
    // Set initial Opus bitrate from CLI, clamped to receiver recommendation.
    int target_opus = cli_audio_cap_bps_;
    if (recommendations_.audio.bit_rate_limits.maximum > 0) {
      target_opus =
          std::min(target_opus, recommendations_.audio.bit_rate_limits.maximum);
    }
    if (recommendations_.audio.bit_rate_limits.minimum > 0) {
      target_opus = std::max(
          target_opus,
          std::min(cli_audio_cap_bps_,
                   recommendations_.audio.bit_rate_limits.minimum));
    }
    audio_encoder_->SetBitrate(target_opus);
    counters_.opus_bitrate = audio_encoder_->GetBitrate();
  }

  streaming_start_ = env_.now();

  // Start recurring alarms.
  poll_alarm_.ScheduleFromNow([this] { PollCapture(); }, kPollInterval);
  congestion_alarm_.ScheduleFromNow(
      [this] { ControlForNetworkCongestion(); }, kCongestionInterval);
  telemetry_alarm_.ScheduleFromNow([this] { EmitTelemetry(); },
                                   kTelemetryInterval);
}

DesktopMediaSender::~DesktopMediaSender() {
  // Alarms are automatically canceled on destruction.
  EmitFinalTelemetry();
}

void DesktopMediaSender::SetPlaybackRate(double rate) {
  // Desktop capture does not support playback rate changes.
}

void DesktopMediaSender::OnInputMessage(InputMessage message) {
  // Desktop mirroring could forward input to the desktop, but that's out of
  // scope for this slice. Input messages are intentionally ignored.
}

void DesktopMediaSender::OnStatisticsUpdated(const SenderStats& stats) {
  counters_.stats_updates++;
}

void DesktopMediaSender::PollCapture() {
  const auto now = env_.now();

  // Check capture bus errors.
  if (capture_->PollRuntimeError()) {
    last_error_ = capture_->last_error();
    watchdog_failed_ = true;
    shutdown_callback_();
    return;
  }

  if (anchor_state_ == AnchorState::kWaitingForFirstPair) {
    // Do not submit either track until their timestamps establish an A/V
    // anchor. Keep only the latest frame from each capture queue.
    if (auto audio = capture_->TakeLatestAudio()) {
      pending_audio_ = std::move(*audio);
    }
    if (auto video = capture_->TakeLatestVideo()) {
      pending_video_ = std::move(*video);
    }

    if (pending_video_ && pending_audio_) {
      anchor_pts_ns_ =
          std::min(pending_video_->timestamp_ns, pending_audio_->timestamp_ns);
      anchor_clock_ = now;
      anchor_state_ = AnchorState::kAnchored;
      // Submit the initial pair.
      VideoFrame initial_video = std::move(*pending_video_);
      pending_video_.reset();
      AudioFrame initial_audio = std::move(*pending_audio_);
      pending_audio_.reset();
      TrySubmitVideo(initial_video);
      SubmitAudio(initial_audio);
    }
  } else {
    // Drain the latest frame from each track once timestamps are anchored.
    if (auto audio = capture_->TakeLatestAudio()) {
      SubmitAudio(*audio);
    }
    if (auto video = capture_->TakeLatestVideo()) {
      TrySubmitVideo(*video);
    }
  }

  CheckWatchdog();

  poll_alarm_.ScheduleFromNow([this] { PollCapture(); }, kPollInterval);
}

void DesktopMediaSender::ControlForNetworkCongestion() {
  if (!video_encoder_) {
    return;
  }

  const int estimated = session_->GetEstimatedNetworkBandwidth();
  counters_.network_estimate_bps = estimated;

  if (estimated <= 0) {
    congestion_alarm_.ScheduleFromNow(
        [this] { ControlForNetworkCongestion(); }, kCongestionInterval);
    return;
  }

  // Compute usable bandwidth.
  const int usable_total = static_cast<int>(estimated * kUsableBandwidthFraction);
  const int actual_opus = counters_.opus_bitrate > 0 ? counters_.opus_bitrate : 0;
  const int available_video =
      std::max(kMinVideoBitrate, usable_total - actual_opus);

  // Receiver recommendation hard cap.
  int receiver_video_max = cli_video_cap_bps_;
  if (recommendations_.video.bit_rate_limits.maximum > 0) {
    receiver_video_max =
        std::min(cli_video_cap_bps_,
                 recommendations_.video.bit_rate_limits.maximum);
  }
  const int hard_video_cap = std::min(cli_video_cap_bps_, receiver_video_max);

  int new_target;
  const int current_target = counters_.video_target_bps > 0
                                 ? counters_.video_target_bps
                                 : video_encoder_->GetTargetBitrate();
  if (available_video < current_target) {
    // Decrease: apply immediately.
    new_target = std::min(available_video, hard_video_cap);
  } else {
    // Increase: grow by at most 10% per interval, capped.
    const int max_growth = static_cast<int>(current_target *
                                            (1.0 + kMaxBitrateGrowthFactor));
    new_target = std::min({available_video, hard_video_cap, max_growth});
  }

  // Apply recommendation floor if reachable.
  if (recommendations_.video.bit_rate_limits.minimum > 0) {
    const int floor = std::min(recommendations_.video.bit_rate_limits.minimum,
                               hard_video_cap);
    new_target = std::max(new_target,
                          std::min(floor, available_video));
  }

  new_target = std::max(kMinVideoBitrate, new_target);
  video_encoder_->SetTargetBitrate(new_target);
  counters_.video_target_bps = new_target;

  congestion_alarm_.ScheduleFromNow(
      [this] { ControlForNetworkCongestion(); }, kCongestionInterval);
}

void DesktopMediaSender::EmitTelemetry() {
  const auto tel = capture_->telemetry();
  const auto now = env_.now();
  const auto uptime_ms = std::chrono::duration_cast<milliseconds>(
      now - streaming_start_).count();

  std::printf(
      "{\"event\":\"streamer_plus_telemetry\","
      "\"state\":\"%s\","
      "\"uptime_ms\":%lld,"
      "\"capture\":{\"video_frames\":%llu,\"audio_frames\":%llu,"
      "\"video_dropped\":%llu,\"audio_dropped\":%llu,"
      "\"format_error\":%s,\"timestamp_error\":%s},"
      "\"submit\":{\"video\":%llu,\"audio_buffers\":%llu,"
      "\"audio_samples\":%llu,\"video_pending_dropped\":%llu,"
      "\"video_preencode_rejected\":%llu,"
      "\"video_timestamp_dropped\":%llu,"
      "\"audio_timestamp_dropped\":%llu,\"audio_gaps\":%llu},"
      "\"encode\":{\"video_completed\":%llu,\"video_bytes\":%llu,"
      "\"last_video_encode_us\":%lld,\"last_video_quantizer\":%d,"
      "\"opus_bitrate\":%d},"
      "\"network\":{\"estimate_bps\":%d,\"usable_bps\":%d,"
      "\"video_target_bps\":%d,\"video_cli_cap_bps\":%d,"
      "\"video_receiver_cap_bps\":%d,\"stats_updates\":%llu},"
      "\"runtime\":{\"portal_closed\":false,\"capture_error\":%s,"
      "\"watchdog_error\":%s,\"timestamp_error\":%s,"
      "\"last_error\":\"%s\"}}\n",
      watchdog_failed_ ? "failed" : "streaming",
      static_cast<long long>(uptime_ms),
      static_cast<unsigned long long>(tel.video_frames),
      static_cast<unsigned long long>(tel.audio_frames),
      static_cast<unsigned long long>(tel.video_dropped),
      static_cast<unsigned long long>(tel.audio_dropped),
      tel.format_error ? "true" : "false",
      tel.timestamp_error ? "true" : "false",
      static_cast<unsigned long long>(counters_.video_submitted),
      static_cast<unsigned long long>(counters_.audio_submitted),
      static_cast<unsigned long long>(counters_.audio_samples),
      static_cast<unsigned long long>(counters_.video_pending_dropped),
      static_cast<unsigned long long>(counters_.video_preencode_rejected),
      static_cast<unsigned long long>(counters_.video_timestamp_dropped),
      static_cast<unsigned long long>(counters_.audio_timestamp_dropped),
      static_cast<unsigned long long>(counters_.audio_gaps),
      static_cast<unsigned long long>(counters_.video_encoded),
      static_cast<unsigned long long>(counters_.video_bytes),
      static_cast<long long>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              counters_.last_video_encode_wall).count()),
      counters_.last_video_quantizer,
      counters_.opus_bitrate,
      counters_.network_estimate_bps,
      counters_.network_estimate_bps > 0
          ? static_cast<int>(counters_.network_estimate_bps *
                             kUsableBandwidthFraction)
          : 0,
      counters_.video_target_bps,
      cli_video_cap_bps_,
      recommendations_.video.bit_rate_limits.maximum,
      static_cast<unsigned long long>(counters_.stats_updates),
      watchdog_failed_ ? "true" : "false",
      watchdog_failed_ ? "true" : "false",
      tel.timestamp_error ? "true" : "false",
      last_error_.c_str());
  std::fflush(stdout);

  if (!watchdog_failed_) {
    telemetry_alarm_.ScheduleFromNow([this] { EmitTelemetry(); },
                                     kTelemetryInterval);
  }
}

void DesktopMediaSender::EmitFinalTelemetry() {
  const auto tel = capture_->telemetry();
  const auto now = env_.now();
  const auto uptime_ms = std::chrono::duration_cast<milliseconds>(
      now - streaming_start_).count();

  std::printf(
      "{\"event\":\"streamer_plus_telemetry\","
      "\"state\":\"stopping\","
      "\"uptime_ms\":%lld,"
      "\"capture\":{\"video_frames\":%llu,\"audio_frames\":%llu,"
      "\"video_dropped\":%llu,\"audio_dropped\":%llu,"
      "\"format_error\":%s,\"timestamp_error\":%s},"
      "\"submit\":{\"video\":%llu,\"audio_buffers\":%llu,"
      "\"audio_samples\":%llu,\"video_pending_dropped\":%llu,"
      "\"video_preencode_rejected\":%llu,"
      "\"video_timestamp_dropped\":%llu,"
      "\"audio_timestamp_dropped\":%llu,\"audio_gaps\":%llu},"
      "\"encode\":{\"video_completed\":%llu,\"video_bytes\":%llu,"
      "\"last_video_encode_us\":%lld,\"last_video_quantizer\":%d,"
      "\"opus_bitrate\":%d},"
      "\"network\":{\"estimate_bps\":%d,\"usable_bps\":%d,"
      "\"video_target_bps\":%d,\"video_cli_cap_bps\":%d,"
      "\"video_receiver_cap_bps\":%d,\"stats_updates\":%llu},"
      "\"runtime\":{\"portal_closed\":false,\"capture_error\":%s,"
      "\"watchdog_error\":%s,\"timestamp_error\":%s,"
      "\"last_error\":\"%s\"}}\n",
      static_cast<long long>(uptime_ms),
      static_cast<unsigned long long>(tel.video_frames),
      static_cast<unsigned long long>(tel.audio_frames),
      static_cast<unsigned long long>(tel.video_dropped),
      static_cast<unsigned long long>(tel.audio_dropped),
      tel.format_error ? "true" : "false",
      tel.timestamp_error ? "true" : "false",
      static_cast<unsigned long long>(counters_.video_submitted),
      static_cast<unsigned long long>(counters_.audio_submitted),
      static_cast<unsigned long long>(counters_.audio_samples),
      static_cast<unsigned long long>(counters_.video_pending_dropped),
      static_cast<unsigned long long>(counters_.video_preencode_rejected),
      static_cast<unsigned long long>(counters_.video_timestamp_dropped),
      static_cast<unsigned long long>(counters_.audio_timestamp_dropped),
      static_cast<unsigned long long>(counters_.audio_gaps),
      static_cast<unsigned long long>(counters_.video_encoded),
      static_cast<unsigned long long>(counters_.video_bytes),
      static_cast<long long>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              counters_.last_video_encode_wall).count()),
      counters_.last_video_quantizer,
      counters_.opus_bitrate,
      counters_.network_estimate_bps,
      0,
      counters_.video_target_bps,
      cli_video_cap_bps_,
      recommendations_.video.bit_rate_limits.maximum,
      static_cast<unsigned long long>(counters_.stats_updates),
      watchdog_failed_ ? "true" : "false",
      watchdog_failed_ ? "true" : "false",
      tel.timestamp_error ? "true" : "false",
      last_error_.c_str());
  std::fflush(stdout);
}

std::optional<Clock::time_point> MapCaptureTimestamp(
    uint64_t anchor_pts_ns,
    Clock::time_point anchor_clock,
    uint64_t pts_ns) {
  if (pts_ns < anchor_pts_ns) {
    return std::nullopt;
  }
  const uint64_t delta_ns = pts_ns - anchor_pts_ns;
  if (delta_ns > static_cast<uint64_t>(nanoseconds::max().count())) {
    return std::nullopt;
  }
  // Convert to Clock::duration (which is microseconds on this platform).
  const auto delta = std::chrono::duration_cast<Clock::duration>(
      nanoseconds(delta_ns));
  // Check for time_point overflow.
  if (anchor_clock > Clock::time_point::max() - delta) {
    return std::nullopt;  // Would overflow
  }
  return anchor_clock + delta;
}

bool IsValidI420FrameSize(uint32_t width,
                           uint32_t height,
                           size_t byte_count) {
  if (width % 2 != 0 || height % 2 != 0) {
    return false;
  }
  const size_t expected_size =
      static_cast<size_t>(width) * height * 3 / 2;
  return byte_count == expected_size;
}

std::optional<Clock::time_point> DesktopMediaSender::MapTimestamp(
    uint64_t pts_ns) {
  return MapCaptureTimestamp(anchor_pts_ns_, anchor_clock_, pts_ns);
}

void DesktopMediaSender::TrySubmitVideo(const VideoFrame& frame) {
  if (!video_encoder_) {
    return;
  }

  // Monotonicity check.
  if (have_last_video_pts_ && frame.timestamp_ns <= last_video_pts_ns_) {
    counters_.video_timestamp_dropped++;
    return;
  }

  const auto ref_time = MapTimestamp(frame.timestamp_ns);
  if (!ref_time) {
    counters_.video_timestamp_dropped++;
    return;
  }

  if (video_state_ == VideoState::kEncoding) {
    // Replace pending frame; drop the old one.
    pending_video_ = frame;
    counters_.video_pending_dropped++;
    return;
  }

  // Build the encoder frame.
  StreamingVideoEncoder::VideoFrame enc_frame;
  enc_frame.width = static_cast<int>(frame.width);
  enc_frame.height = static_cast<int>(frame.height);

  // I420 tightly packed: Y plane, then U, then V.
  if (!IsValidI420FrameSize(frame.width, frame.height,
                            frame.i420_bytes.size())) {
    const size_t expected_size =
        static_cast<size_t>(frame.width) * frame.height * 3 / 2;
    OSP_LOG_WARN << "VIDEO frame size mismatch: got "
                 << frame.i420_bytes.size() << " expected " << expected_size;
    counters_.video_preencode_rejected++;
    return;
  }
  const int y_size = enc_frame.width * enc_frame.height;
  const int uv_size = y_size / 4;
  enc_frame.yuv_planes[0] = frame.i420_bytes.data();
  enc_frame.yuv_planes[1] = frame.i420_bytes.data() + y_size;
  enc_frame.yuv_planes[2] = frame.i420_bytes.data() + y_size + uv_size;
  enc_frame.yuv_strides[0] = enc_frame.width;
  enc_frame.yuv_strides[1] = enc_frame.width / 2;
  enc_frame.yuv_strides[2] = enc_frame.width / 2;

  // Compute frame duration.
  Clock::duration frame_duration = milliseconds(33);  // ~30fps default
  if (have_last_video_pts_) {
    const int64_t pts_delta =
        static_cast<int64_t>(frame.timestamp_ns) -
        static_cast<int64_t>(last_video_pts_ns_);
    if (pts_delta > 0) {
      frame_duration = std::chrono::duration_cast<Clock::duration>(
          nanoseconds(pts_delta));
    }
  } else {
    // First video frame: use nominal from configured FPS.
    if (settings_.video_frames_per_second > 0) {
      frame_duration = milliseconds(1000 / settings_.video_frames_per_second);
    }
  }
  frame_duration =
      std::max(std::min(frame_duration, kMaxFrameDuration), kMinFrameDuration);
  enc_frame.duration = frame_duration;

  // Capture metadata.
  enc_frame.capture_begin_time = *ref_time;
  enc_frame.capture_end_time = *ref_time + frame_duration;

  last_video_pts_ns_ = frame.timestamp_ns;
  have_last_video_pts_ = true;
  const bool accepted = video_encoder_->TryEncodeAndSend(
      enc_frame, *ref_time,
      [this](StreamingVpxEncoder::Stats stats) { OnVideoEncoded(stats); });

  if (accepted) {
    video_state_ = VideoState::kEncoding;
    last_video_submit_ = env_.now();
    counters_.video_submitted++;
  } else {
    counters_.video_preencode_rejected++;
    // Gate is still idle — try pending immediately.
    if (pending_video_) {
      TrySubmitVideo(*pending_video_);
      pending_video_.reset();
    }
  }
}

void DesktopMediaSender::OnVideoEncoded(StreamingVpxEncoder::Stats stats) {
  video_state_ = VideoState::kIdle;
  counters_.video_encoded++;
  counters_.video_bytes += static_cast<uint64_t>(stats.encoded_size);
  counters_.last_video_encode_wall = stats.encode_wall_time;
  counters_.last_video_quantizer = stats.quantizer;

  // Submit the pending frame if one exists.
  if (pending_video_) {
    TrySubmitVideo(*pending_video_);
    pending_video_.reset();
  }
}

void DesktopMediaSender::SubmitAudio(const AudioFrame& frame) {
  if (!audio_encoder_) {
    return;
  }

  // Monotonicity check.
  if (have_last_audio_pts_ && frame.timestamp_ns <= last_audio_pts_ns_) {
    counters_.audio_timestamp_dropped++;
    return;
  }

  const auto ref_time = MapTimestamp(frame.timestamp_ns);
  if (!ref_time) {
    counters_.audio_timestamp_dropped++;
    return;
  }

  // Detect audio gaps.
  if (have_last_audio_pts_) {
    const int64_t pts_delta =
        static_cast<int64_t>(frame.timestamp_ns) -
        static_cast<int64_t>(last_audio_pts_ns_);
    if (pts_delta > 0) {
      const auto gap = std::chrono::duration_cast<Clock::duration>(
          nanoseconds(pts_delta));
      if (gap > kAudioGapThreshold) {
        counters_.audio_gaps++;
      }
    }
  }

  last_audio_pts_ns_ = frame.timestamp_ns;
  have_last_audio_pts_ = true;
  last_audio_submit_ = env_.now();

  // num_samples is tuples, not total floats.
  const int num_samples =
      static_cast<int>(frame.interleaved_samples.size() / frame.channels);
  if (num_samples <= 0) {
    return;
  }

  audio_encoder_->EncodeAndSend(
      frame.interleaved_samples.data(), num_samples,
      *ref_time, *ref_time, *ref_time);
  counters_.audio_submitted++;
  counters_.audio_samples += static_cast<uint64_t>(
      frame.interleaved_samples.size());
}

void DesktopMediaSender::CheckWatchdog() {
  if (watchdog_failed_) {
    return;  // Already shutting down.
  }

  const auto now = env_.now();
  const auto since_start = now - streaming_start_;

  // Initial pair deadline.
  if (anchor_state_ != AnchorState::kAnchored) {
    if (since_start > kInitialPairDeadline) {
      last_error_ = "watchdog: initial A/V pair not received within deadline";
      watchdog_failed_ = true;
      shutdown_callback_();
    }
    return;
  }

  // Audio watchdog: no submitted audio for 1 second.
  if (audio_encoder_ && last_audio_submit_ != Clock::time_point{}) {
    if (now - last_audio_submit_ > kAudioWatchdogDeadline) {
      last_error_ = "watchdog: no audio submitted within deadline";
      watchdog_failed_ = true;
      shutdown_callback_();
    }
  }

  // Video watchdog: no submitted video for 2 seconds.
  if (video_encoder_ && last_video_submit_ != Clock::time_point{}) {
    if (now - last_video_submit_ > kVideoWatchdogDeadline) {
      last_error_ = "watchdog: no video submitted within deadline";
      watchdog_failed_ = true;
      shutdown_callback_();
    }
  }
}

}  // namespace openscreen::cast::streamer_plus
