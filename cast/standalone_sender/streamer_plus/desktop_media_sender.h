// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STANDALONE_SENDER_STREAMER_PLUS_DESKTOP_MEDIA_SENDER_H_
#define CAST_STANDALONE_SENDER_STREAMER_PLUS_DESKTOP_MEDIA_SENDER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "cast/standalone_sender/media_sender.h"
#include "cast/standalone_sender/streamer_plus/capture.h"
#include "cast/standalone_sender/streaming_opus_encoder.h"
#include "cast/standalone_sender/streaming_vpx_encoder.h"
#include "cast/streaming/public/capture_recommendations.h"
#include "cast/streaming/public/environment.h"
#include "cast/streaming/public/sender_session.h"
#include "platform/api/time.h"
#include "util/alarm.h"

namespace openscreen::cast::streamer_plus {

// Maps a capture PTS to a Cast clock time using a previously established A/V
// anchor. Returns empty if the PTS precedes the anchor or the result overflows.
std::optional<Clock::time_point> MapCaptureTimestamp(
    uint64_t anchor_pts_ns,
    Clock::time_point anchor_clock,
    uint64_t pts_ns);

// Returns whether |byte_count| is the tightly packed I420 size for an even
// dimension frame. I420 requires even width and height in this sender.
bool IsValidI420FrameSize(uint32_t width,
                          uint32_t height,
                          size_t byte_count);

// Bridges GstreamerCapture output into the negotiated Cast streaming encoders.
// All public methods are called on the Open Screen task-runner sequence.
// GstreamerCapture is borrowed and must outlive this sender.
class DesktopMediaSender final : public MediaSender {
 public:
  DesktopMediaSender(Environment& environment,
                     const ConnectionSettings& settings,
                     const SenderSession* session,
                     SenderSession::ConfiguredSenders senders,
                     capture_recommendations::Recommendations recommendations,
                     GstreamerCapture* capture,
                     ShutdownCallback shutdown_callback);
  ~DesktopMediaSender() final;

  // MediaSender overrides.
  void SetPlaybackRate(double rate) final;
  void OnInputMessage(InputMessage message) final;
  void OnStatisticsUpdated(const SenderStats& stats) final;

 private:
  // Periodic capture poll: drains the latest A/V frames from the capture
  // queues and submits them to the encoders. Runs every ~5 ms.
  void PollCapture();

  // Congestion control: adjusts encoder bitrates based on estimated
  // bandwidth. Runs every ~500 ms.
  void ControlForNetworkCongestion();

  // Telemetry JSON output. Runs every ~1 s.
  void EmitTelemetry();

  // Maps a capture PTS (in nanoseconds) to a Clock::time_point using the
  // established anchor. Returns empty on failure (PTS before anchor, overflow).
  std::optional<Clock::time_point> MapTimestamp(uint64_t pts_ns);

  // Submits one video frame to the VPX encoder if the encoder is not busy.
  void TrySubmitVideo(const VideoFrame& frame);

  // VPX encode completion callback.
  void OnVideoEncoded(StreamingVpxEncoder::Stats stats);

  // Submits one audio frame to the Opus encoder.
  void SubmitAudio(const AudioFrame& frame);

  // Checks the watchdog conditions and triggers shutdown on failure.
  void CheckWatchdog();

  // Emit final telemetry at shutdown.
  void EmitFinalTelemetry();

  // --- Owned objects ---
  Environment& env_;
  const ConnectionSettings settings_;
  const raw_ptr<const SenderSession> session_;
  const raw_ptr<GstreamerCapture> capture_;
  const ShutdownCallback shutdown_callback_;

  std::unique_ptr<StreamingVpxEncoder> video_encoder_;
  std::unique_ptr<StreamingOpusEncoder> audio_encoder_;

  // Receiver recommendations for congestion clamping.
  const capture_recommendations::Recommendations recommendations_;

  // --- Timestamp anchoring ---
  enum class AnchorState {
    kWaitingForFirstPair,
    kAnchored,
  };
  AnchorState anchor_state_ = AnchorState::kWaitingForFirstPair;
  uint64_t anchor_pts_ns_ = 0;
  Clock::time_point anchor_clock_{};

  // Per-track last accepted PTS for monotonicity checks.
  uint64_t last_video_pts_ns_ = 0;
  uint64_t last_audio_pts_ns_ = 0;
  bool have_last_video_pts_ = false;
  bool have_last_audio_pts_ = false;

  // --- One-video-in-flight ---
  enum class VideoState { kIdle, kEncoding };
  VideoState video_state_ = VideoState::kIdle;
  std::optional<VideoFrame> pending_video_;
  std::optional<AudioFrame> pending_audio_;

  // --- Telemetry counters ---
  struct Counters {
    uint64_t video_submitted = 0;
    uint64_t audio_submitted = 0;
    uint64_t audio_samples = 0;
    uint64_t video_pending_dropped = 0;
    uint64_t video_preencode_rejected = 0;
    uint64_t video_timestamp_dropped = 0;
    uint64_t audio_timestamp_dropped = 0;
    uint64_t audio_gaps = 0;
    uint64_t video_encoded = 0;
    uint64_t video_bytes = 0;
    uint64_t stats_updates = 0;
    Clock::duration last_video_encode_wall{};
    int last_video_quantizer = 0;
    int opus_bitrate = 0;
    int network_estimate_bps = 0;
    int video_target_bps = 0;
  } counters_;

  // Watchdog tracking
  Clock::time_point streaming_start_{};
  Clock::time_point last_audio_submit_{};
  Clock::time_point last_video_submit_{};
  bool watchdog_failed_ = false;
  std::string last_error_;

  // --- Alarms ---
  Alarm poll_alarm_;
  Alarm congestion_alarm_;
  Alarm telemetry_alarm_;

  // Configured caps
  const int cli_video_cap_bps_;
  const int cli_audio_cap_bps_;
};

}  // namespace openscreen::cast::streamer_plus

#endif  // CAST_STANDALONE_SENDER_STREAMER_PLUS_DESKTOP_MEDIA_SENDER_H_
