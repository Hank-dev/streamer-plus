// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/standalone_sender/streamer_plus/capture.h"

#include <chrono>
#include <thread>

#include "gtest/gtest.h"

namespace openscreen::cast::streamer_plus {
namespace {

TEST(DropOldQueueTest, OverflowDropsOldestEntry) {
  DropOldQueue<int> queue(2);
  queue.Push(1);
  queue.Push(2);
  queue.Push(3);

  EXPECT_EQ(queue.size(), 2u);
  EXPECT_EQ(queue.dropped(), 1u);
  const auto latest = queue.TakeLatest();
  ASSERT_TRUE(latest);
  EXPECT_EQ(*latest, 3);
  EXPECT_EQ(queue.size(), 0u);
  EXPECT_EQ(queue.dropped(), 2u);
}

TEST(DropOldQueueTest, TakeLatestDoesNotBlockAndDropsOlderEntries) {
  DropOldQueue<int> queue(3);
  EXPECT_FALSE(queue.TakeLatest());
  queue.Push(4);
  queue.Push(5);
  queue.Push(6);

  const auto latest = queue.TakeLatest();
  ASSERT_TRUE(latest);
  EXPECT_EQ(*latest, 6);
  EXPECT_EQ(queue.dropped(), 2u);
}

TEST(GstreamerCaptureTest, MalformedPipelineFailsCleanly) {
  GstreamerCapture capture;
  EXPECT_FALSE(capture.Start("this is not a gstreamer pipeline"));
  EXPECT_FALSE(capture.last_error().empty());
}

TEST(GstreamerCaptureTest, InvalidDesktopOptionsFailClosed) {
  GstreamerCapture capture;
  DesktopCaptureOptions options;
  options.pipewire_fd = -1;
  options.target_object = "42";
  options.width = 1920;
  options.height = 1080;
  options.frames_per_second = 30;
  options.audio_source = "@DEFAULT_MONITOR@";
  EXPECT_FALSE(capture.StartDesktop(options));
  EXPECT_NE(capture.last_error().find("invalid desktop capture options"),
            std::string::npos);
}

TEST(GstreamerCaptureTest, ReportsWorkerFormatFailureAtRuntime) {
  GstreamerCapture capture;
  ASSERT_TRUE(capture.Start(
      "videotestsrc is-live=true ! videoconvert ! "
      "video/x-raw,format=I420,width=64,height=48,framerate=30/1 ! "
      "appsink name=video_sink audiotestsrc is-live=true ! audioconvert ! "
      "audio/x-raw,format=S16LE,layout=interleaved,channels=2,rate=48000 ! "
      "appsink name=audio_sink"))
      << capture.last_error();

  bool reported = false;
  for (int attempt = 0; attempt < 50 && !reported; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    reported = capture.PollRuntimeError();
  }
  capture.Stop();

  EXPECT_TRUE(reported);
  EXPECT_TRUE(capture.telemetry().format_error);
  EXPECT_NE(capture.last_error().find("audio capture format error"),
            std::string::npos);
}

TEST(GstreamerCaptureTest, SyntheticPipelineProducesExpectedFrames) {
  GstreamerCapture capture;
  ASSERT_TRUE(capture.Start(GstreamerCapture::SyntheticPipeline()))
      << capture.last_error();

  std::this_thread::sleep_for(std::chrono::milliseconds(750));
  capture.Stop();

  const CaptureTelemetry telemetry = capture.telemetry();
  EXPECT_GE(telemetry.video_frames, 3u);
  EXPECT_GE(telemetry.audio_frames, 3u);
  EXPECT_FALSE(telemetry.format_error);
  EXPECT_FALSE(telemetry.timestamp_error);

  const auto video = capture.TakeLatestVideo();
  ASSERT_TRUE(video);
  EXPECT_EQ(video->width, 320u);
  EXPECT_EQ(video->height, 240u);
  EXPECT_EQ(video->i420_bytes.size(), 320u * 240u * 3u / 2u);

  const auto audio = capture.TakeLatestAudio();
  ASSERT_TRUE(audio);
  EXPECT_EQ(audio->channels, 2u);
  EXPECT_EQ(audio->sample_rate, 48000u);
  EXPECT_FALSE(audio->interleaved_samples.empty());
  EXPECT_EQ(audio->interleaved_samples.size() % 2u, 0u);
}

}  // namespace
}  // namespace openscreen::cast::streamer_plus
