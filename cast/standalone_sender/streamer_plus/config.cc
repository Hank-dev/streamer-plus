// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/standalone_sender/streamer_plus/config.h"

#include <charconv>
#include <cctype>
#include <system_error>

#include "cast/streaming/public/constants.h"

namespace openscreen::cast::streamer_plus {
namespace {

// Stock Chromecast Cast V2 control traffic is served on TCP/TLS port 8009.
// Open Screen's kDefaultCastPort (8010) is for development receivers, not
// production hardware.
constexpr uint16_t kStockCastV2ControlPort = 8009;

Error InvalidArgument(std::string message) {
  return Error(Error::Code::kParameterInvalid, std::move(message));
}

ErrorOr<uint32_t> ParseScalar(std::string_view flag,
                              std::string_view value,
                              uint32_t minimum,
                              uint32_t maximum) {
  uint32_t parsed = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(),
                                      parsed);
  if (value.empty() || result.ec != std::errc() ||
      result.ptr != value.data() + value.size()) {
    return InvalidArgument("--" + std::string(flag) +
                           " must be an integer value");
  }
  if (parsed < minimum || parsed > maximum) {
    return InvalidArgument("--" + std::string(flag) + " must be between " +
                           std::to_string(minimum) + " and " +
                           std::to_string(maximum));
  }
  return parsed;
}

ErrorOr<IPEndpoint> ParseReceiver(std::string_view value) {
  ErrorOr<IPEndpoint> endpoint = IPEndpoint::Parse(value);
  if (endpoint.is_value()) {
    if (endpoint.value().port == 0) {
      return InvalidArgument("--receiver port must not be zero");
    }
    return endpoint.value();
  }

  ErrorOr<IPAddress> address = IPAddress::Parse(value);
  if (address.is_value()) {
    return IPEndpoint{address.value(), kStockCastV2ControlPort};
  }

  return InvalidArgument("--receiver must be an IP address or addr[:port]");
}

ErrorOr<std::string> ParseAudioSource(std::string_view value) {
  if (value.empty() || value.size() > 255) {
    return InvalidArgument("--audio-source must contain 1 through 255 bytes");
  }
  for (unsigned char character : value) {
    if (std::iscntrl(character)) {
      return InvalidArgument("--audio-source must not contain ASCII control characters");
    }
  }
  if (value != "@DEFAULT_MONITOR@" && !value.ends_with(".monitor")) {
    return InvalidArgument("--audio-source must be @DEFAULT_MONITOR@ or end in .monitor");
  }
  return std::string(value);
}

}  // namespace

ErrorOr<ParsedCommandLine> ParseCommandLine(int argc,
                                            const char* const argv[]) {
  ParsedCommandLine parsed;
  bool receiver_seen = false;
  bool width_seen = false;
  bool height_seen = false;
  bool fps_seen = false;
  bool video_bitrate_seen = false;
  bool audio_bitrate_seen = false;
  bool latency_seen = false;
  bool audio_source_seen = false;
  bool portal_timeout_seen = false;
  bool dry_run_seen = false;
  bool capture_self_test_seen = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view argument(argv[i]);
    if (argument == "--help") {
      if (argc != 2) {
        return InvalidArgument("--help cannot be combined with other arguments");
      }
      parsed.command = Command::kHelp;
      return parsed;
    }
    if (argument == "--dry-run") {
      if (dry_run_seen) {
        return InvalidArgument("duplicate --dry-run flag");
      }
      dry_run_seen = true;
      parsed.config.dry_run = true;
      continue;
    }
    if (argument == "--capture-self-test") {
      if (capture_self_test_seen) {
        return InvalidArgument("duplicate --capture-self-test flag");
      }
      capture_self_test_seen = true;
      parsed.config.capture_self_test = true;
      continue;
    }
    if (!argument.starts_with("--")) {
      return InvalidArgument("positional arguments are not supported: " +
                             std::string(argument));
    }

    const size_t equals = argument.find('=');
    if (equals == std::string_view::npos) {
      return InvalidArgument("flag requires a value: " + std::string(argument));
    }
    const std::string_view name = argument.substr(2, equals - 2);
    const std::string_view value = argument.substr(equals + 1);

    if (name == "receiver") {
      if (receiver_seen) {
        return InvalidArgument("duplicate --receiver flag");
      }
      receiver_seen = true;
      ErrorOr<IPEndpoint> receiver = ParseReceiver(value);
      if (receiver.is_error()) {
        return std::move(receiver.error());
      }
      parsed.config.receiver = receiver.value();
    } else if (name == "width") {
      if (width_seen) {
        return InvalidArgument("duplicate --width flag");
      }
      width_seen = true;
      ErrorOr<uint32_t> width = ParseScalar(name, value, kMinWidth, kMaxWidth);
      if (width.is_error()) {
        return std::move(width.error());
      }
      parsed.config.width = width.value();
    } else if (name == "height") {
      if (height_seen) {
        return InvalidArgument("duplicate --height flag");
      }
      height_seen = true;
      ErrorOr<uint32_t> height =
          ParseScalar(name, value, kMinHeight, kMaxHeight);
      if (height.is_error()) {
        return std::move(height.error());
      }
      parsed.config.height = height.value();
    } else if (name == "fps") {
      if (fps_seen) {
        return InvalidArgument("duplicate --fps flag");
      }
      fps_seen = true;
      ErrorOr<uint32_t> fps =
          ParseScalar(name, value, kMinFramesPerSecond, kMaxFramesPerSecond);
      if (fps.is_error()) {
        return std::move(fps.error());
      }
      parsed.config.frames_per_second = fps.value();
    } else if (name == "video-bitrate") {
      if (video_bitrate_seen) {
        return InvalidArgument("duplicate --video-bitrate flag");
      }
      video_bitrate_seen = true;
      ErrorOr<uint32_t> bitrate =
          ParseScalar(name, value, kMinVideoBitrate, kMaxVideoBitrate);
      if (bitrate.is_error()) {
        return std::move(bitrate.error());
      }
      parsed.config.video_bitrate = bitrate.value();
    } else if (name == "audio-bitrate") {
      if (audio_bitrate_seen) {
        return InvalidArgument("duplicate --audio-bitrate flag");
      }
      audio_bitrate_seen = true;
      ErrorOr<uint32_t> bitrate =
          ParseScalar(name, value, kMinAudioBitrate, kMaxAudioBitrate);
      if (bitrate.is_error()) {
        return std::move(bitrate.error());
      }
      parsed.config.audio_bitrate = bitrate.value();
    } else if (name == "latency-target-ms") {
      if (latency_seen) {
        return InvalidArgument("duplicate --latency-target-ms flag");
      }
      latency_seen = true;
      ErrorOr<uint32_t> latency = ParseScalar(
          name, value, kMinLatencyTargetMs, kMaxLatencyTargetMs);
      if (latency.is_error()) {
        return std::move(latency.error());
      }
      parsed.config.latency_target_ms = latency.value();
    } else if (name == "audio-source") {
      if (audio_source_seen) {
        return InvalidArgument("duplicate --audio-source flag");
      }
      audio_source_seen = true;
      ErrorOr<std::string> source = ParseAudioSource(value);
      if (source.is_error()) {
        return std::move(source.error());
      }
      parsed.config.audio_source = std::move(source.value());
    } else if (name == "portal-timeout-ms") {
      if (portal_timeout_seen) {
        return InvalidArgument("duplicate --portal-timeout-ms flag");
      }
      portal_timeout_seen = true;
      ErrorOr<uint32_t> timeout =
          ParseScalar(name, value, kMinPortalTimeoutMs, kMaxPortalTimeoutMs);
      if (timeout.is_error()) {
        return std::move(timeout.error());
      }
      parsed.config.portal_timeout_ms = timeout.value();
    } else {
      return InvalidArgument("unknown flag: --" + std::string(name));
    }
  }

  if (parsed.config.dry_run && parsed.config.capture_self_test) {
    return InvalidArgument("--capture-self-test cannot be combined with --dry-run");
  }
  if (parsed.config.width % 2 != 0 || parsed.config.height % 2 != 0) {
    return InvalidArgument("--width and --height must be even for I420 video");
  }
  if (!receiver_seen && !parsed.config.capture_self_test) {
    return InvalidArgument("--receiver=addr[:port] is required");
  }
  return parsed;
}

std::string Usage(const char* program_name) {
  return "Usage: " + std::string(program_name) +
         " [--capture-self-test] | --receiver=addr[:port] [--dry-run] [--width=N] [--height=N] "
         "[--fps=N] [--video-bitrate=N] [--audio-bitrate=N] "
         "[--latency-target-ms=N] [--audio-source=NAME] [--portal-timeout-ms=N]\n";
}

std::string FormatDryRun(const Config& config) {
  return "receiver=" + config.receiver.ToString() + "\n" +
         "width=" + std::to_string(config.width) + "\n" +
         "height=" + std::to_string(config.height) + "\n" +
         "fps=" + std::to_string(config.frames_per_second) + "\n" +
         "video_bitrate=" + std::to_string(config.video_bitrate) + "\n" +
         "audio_bitrate=" + std::to_string(config.audio_bitrate) + "\n" +
         "latency_target_ms=" + std::to_string(config.latency_target_ms) + "\n" +
         "audio_source=" + config.audio_source + "\n" +
         "portal_timeout_ms=" + std::to_string(config.portal_timeout_ms) + "\n";
}

}  // namespace openscreen::cast::streamer_plus
