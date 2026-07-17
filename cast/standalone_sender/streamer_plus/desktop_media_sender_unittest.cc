// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/standalone_sender/streamer_plus/desktop_media_sender.h"

#include <chrono>

#include "gtest/gtest.h"

namespace openscreen::cast::streamer_plus {
namespace {

TEST(DesktopMediaSenderTimestampTest, MapsTimestampFromAnchor) {
  const Clock::time_point anchor_clock(Clock::duration(100));
  const auto mapped = MapCaptureTimestamp(1000, anchor_clock, 3000);

  ASSERT_TRUE(mapped);
  EXPECT_EQ(*mapped, anchor_clock + std::chrono::duration_cast<Clock::duration>(
                                        std::chrono::nanoseconds(2000)));
}

TEST(DesktopMediaSenderTimestampTest, RejectsTimestampBeforeAnchor) {
  EXPECT_FALSE(MapCaptureTimestamp(1000, Clock::time_point{}, 999));
}

TEST(DesktopMediaSenderTimestampTest, RejectsTimePointOverflow) {
  const Clock::time_point anchor_clock =
      Clock::time_point::max() - Clock::duration(1);

  EXPECT_FALSE(MapCaptureTimestamp(1000, anchor_clock, 3000));
}

TEST(DesktopMediaSenderI420Test, ValidatesEvenDimensionsAndPackedSize) {
  EXPECT_TRUE(IsValidI420FrameSize(64, 64, 64u * 64u * 3u / 2u));
  EXPECT_FALSE(IsValidI420FrameSize(64, 64, 64u * 64u * 3u / 2u - 1u));
  EXPECT_FALSE(IsValidI420FrameSize(65, 64, 65u * 64u * 3u / 2u));
  EXPECT_FALSE(IsValidI420FrameSize(64, 65, 64u * 65u * 3u / 2u));
}

}  // namespace
}  // namespace openscreen::cast::streamer_plus
