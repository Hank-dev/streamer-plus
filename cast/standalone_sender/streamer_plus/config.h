// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STANDALONE_SENDER_STREAMER_PLUS_CONFIG_H_
#define CAST_STANDALONE_SENDER_STREAMER_PLUS_CONFIG_H_

#include <cstdint>
#include <string>

#include "platform/base/error.h"
#include "platform/base/ip_address.h"

namespace openscreen::cast::streamer_plus {

// These limits keep later capture and encoder implementations within practical
// desktop-streaming limits while rejecting accidental or unsafe allocations.
inline constexpr uint32_t kMinWidth = 64;
inline constexpr uint32_t kMaxWidth = 7680;
inline constexpr uint32_t kMinHeight = 64;
inline constexpr uint32_t kMaxHeight = 4320;
inline constexpr uint32_t kMinFramesPerSecond = 1;
inline constexpr uint32_t kMaxFramesPerSecond = 240;
inline constexpr uint32_t kMinVideoBitrate = 100000;
inline constexpr uint32_t kMaxVideoBitrate = 100000000;
inline constexpr uint32_t kMinAudioBitrate = 8000;
inline constexpr uint32_t kMaxAudioBitrate = 1000000;
inline constexpr uint32_t kMinLatencyTargetMs = 1;
inline constexpr uint32_t kMaxLatencyTargetMs = 5000;
inline constexpr uint32_t kMinPortalTimeoutMs = 1000;
inline constexpr uint32_t kMaxPortalTimeoutMs = 300000;

struct Config {
  IPEndpoint receiver;
  uint32_t width = 1920;
  uint32_t height = 1080;
  uint32_t frames_per_second = 30;
  uint32_t video_bitrate = 8000000;
  uint32_t audio_bitrate = 128000;
  uint32_t latency_target_ms = 500;
  std::string audio_source = "@DEFAULT_MONITOR@";
  uint32_t portal_timeout_ms = 120000;
  bool dry_run = false;
  bool capture_self_test = false;
};

enum class Command {
  kHelp,
  kRun,
};

struct ParsedCommandLine {
  Command command = Command::kRun;
  Config config;
};

ErrorOr<ParsedCommandLine> ParseCommandLine(int argc,
                                            const char* const argv[]);
std::string Usage(const char* program_name);
std::string FormatDryRun(const Config& config);

}  // namespace openscreen::cast::streamer_plus

#endif  // CAST_STANDALONE_SENDER_STREAMER_PLUS_CONFIG_H_
