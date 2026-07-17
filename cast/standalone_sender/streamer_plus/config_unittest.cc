// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/standalone_sender/streamer_plus/config.h"

#include <string_view>

#include "gtest/gtest.h"

namespace openscreen::cast::streamer_plus {
namespace {

ErrorOr<ParsedCommandLine> Parse(std::initializer_list<const char*> arguments) {
  return ParseCommandLine(static_cast<int>(arguments.size()), arguments.begin());
}

TEST(StreamerPlusConfigTest, DefaultsWithIpv4Receiver) {
  const auto result = Parse({"streamer_plus", "--receiver=192.168.1.20"});
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().command, Command::kRun);
  EXPECT_EQ(result.value().config.receiver.ToString(), "192.168.1.20:8009");
  EXPECT_EQ(result.value().config.width, 1920u);
  EXPECT_EQ(result.value().config.height, 1080u);
  EXPECT_EQ(result.value().config.frames_per_second, 30u);
  EXPECT_EQ(result.value().config.video_bitrate, 8000000u);
  EXPECT_EQ(result.value().config.audio_bitrate, 128000u);
  EXPECT_EQ(result.value().config.latency_target_ms, 500u);
  EXPECT_EQ(result.value().config.audio_source, "@DEFAULT_MONITOR@");
  EXPECT_EQ(result.value().config.portal_timeout_ms, 120000u);
}

TEST(StreamerPlusConfigTest, ParsesAndValidatesPortalCaptureOptions) {
  const auto explicit_source = Parse({"streamer_plus", "--receiver=192.168.1.20",
                                      "--audio-source=alsa_output.foo.monitor",
                                      "--portal-timeout-ms=1000"});
  ASSERT_TRUE(explicit_source);
  EXPECT_EQ(explicit_source.value().config.audio_source,
            "alsa_output.foo.monitor");
  EXPECT_EQ(explicit_source.value().config.portal_timeout_ms, 1000u);
  EXPECT_FALSE(Parse({"streamer_plus", "--receiver=192.168.1.20",
                      "--audio-source="}));
  EXPECT_FALSE(Parse({"streamer_plus", "--receiver=192.168.1.20",
                      "--audio-source=microphone"}));
  EXPECT_FALSE(Parse({"streamer_plus", "--receiver=192.168.1.20",
                      "--audio-source=bad\n.monitor"}));
  EXPECT_FALSE(Parse({"streamer_plus", "--receiver=192.168.1.20",
                      "--portal-timeout-ms=999"}));
  EXPECT_FALSE(Parse({"streamer_plus", "--receiver=192.168.1.20",
                      "--portal-timeout-ms=300001"}));
  EXPECT_FALSE(Parse({"streamer_plus", "--receiver=192.168.1.20",
                      "--audio-source=@DEFAULT_MONITOR@",
                      "--audio-source=@DEFAULT_MONITOR@"}));
}

TEST(StreamerPlusConfigTest, ParsesExplicitValuesAndIpv4Port) {
  const auto result = Parse({"streamer_plus", "--receiver=192.168.1.20:9000",
                             "--width=1280", "--height=720", "--fps=60",
                             "--video-bitrate=4000000", "--audio-bitrate=96000",
                             "--latency-target-ms=450", "--dry-run"});
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.value().config.dry_run);
  EXPECT_EQ(result.value().config.receiver.ToString(), "192.168.1.20:9000");
  EXPECT_EQ(result.value().config.width, 1280u);
  EXPECT_EQ(result.value().config.height, 720u);
  EXPECT_EQ(result.value().config.frames_per_second, 60u);
  EXPECT_EQ(result.value().config.video_bitrate, 4000000u);
  EXPECT_EQ(result.value().config.audio_bitrate, 96000u);
  EXPECT_EQ(result.value().config.latency_target_ms, 450u);
}

TEST(StreamerPlusConfigTest, ParsesBracketedIpv6) {
  const auto result = Parse({"streamer_plus", "--receiver=[fd00::20]:8009"});
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().config.receiver.ToString(),
            "[fd00:0000:0000:0000:0000:0000:0000:0020]:8009");
}

TEST(StreamerPlusConfigTest, RejectsInvalidReceiverInputs) {
  EXPECT_FALSE(Parse({"streamer_plus"}));
  EXPECT_FALSE(Parse({"streamer_plus", "--receiver=not-an-address"}));
  EXPECT_FALSE(Parse({"streamer_plus", "--receiver=192.168.1.20:0"}));
}

TEST(StreamerPlusConfigTest, RejectsMalformedOverflowAndOutOfRangeNumbers) {
  EXPECT_FALSE(Parse({"streamer_plus", "--receiver=192.168.1.20", "--width=x"}));
  EXPECT_FALSE(Parse({"streamer_plus", "--receiver=192.168.1.20",
                      "--width=999999999999999999999"}));
  EXPECT_FALSE(Parse({"streamer_plus", "--receiver=192.168.1.20", "--fps=0"}));
  EXPECT_FALSE(Parse({"streamer_plus", "--receiver=192.168.1.20",
                      "--latency-target-ms=10001"}));
}

TEST(StreamerPlusConfigTest, RejectsOddI420Dimensions) {
  EXPECT_FALSE(Parse(
      {"streamer_plus", "--receiver=192.168.1.20", "--width=65"}));
  EXPECT_FALSE(Parse(
      {"streamer_plus", "--receiver=192.168.1.20", "--height=65"}));
}

TEST(StreamerPlusConfigTest, RejectsDuplicateUnknownAndPositionalArguments) {
  EXPECT_FALSE(Parse({"streamer_plus", "--receiver=192.168.1.20",
                      "--width=1280", "--width=1920"}));
  EXPECT_FALSE(Parse({"streamer_plus", "--receiver=192.168.1.20", "--wat"}));
  EXPECT_FALSE(Parse(
      {"streamer_plus", "--receiver=192.168.1.20", "unexpected"}));
}

TEST(StreamerPlusConfigTest, DistinguishesHelpAndDryRun) {
  const auto help = Parse({"streamer_plus", "--help"});
  ASSERT_TRUE(help);
  EXPECT_EQ(help.value().command, Command::kHelp);

  const auto dry_run =
      Parse({"streamer_plus", "--dry-run", "--receiver=192.168.1.20"});
  ASSERT_TRUE(dry_run);
  EXPECT_EQ(dry_run.value().command, Command::kRun);
  EXPECT_TRUE(dry_run.value().config.dry_run);
}

TEST(StreamerPlusConfigTest, CaptureSelfTestDoesNotRequireReceiver) {
  const auto result = Parse({"streamer_plus", "--capture-self-test"});
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.value().config.capture_self_test);
}

TEST(StreamerPlusConfigTest, RejectsCaptureSelfTestWithDryRun) {
  EXPECT_FALSE(Parse({"streamer_plus", "--capture-self-test", "--dry-run"}));
}

}  // namespace
}  // namespace openscreen::cast::streamer_plus