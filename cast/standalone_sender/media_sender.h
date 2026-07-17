// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STANDALONE_SENDER_MEDIA_SENDER_H_
#define CAST_STANDALONE_SENDER_MEDIA_SENDER_H_

#include <functional>
#include <memory>

#include "cast/standalone_sender/connection_settings.h"
#include "cast/streaming/public/capture_recommendations.h"
#include "cast/streaming/public/sender_session.h"

namespace openscreen::cast {

// The producer of media for a negotiated Cast streaming session. Implementors
// are owned by LoopingFileCastAgent and are called on its task-runner sequence.
class MediaSender {
 public:
  using ShutdownCallback = std::function<void()>;

  virtual ~MediaSender() = default;
  virtual void SetPlaybackRate(double) {}
  virtual void OnInputMessage(InputMessage) {}
  virtual void OnStatisticsUpdated(const SenderStats&) {}
};

using MediaSenderFactory = std::function<std::unique_ptr<MediaSender>(
    Environment&,
    const ConnectionSettings&,
    const SenderSession*,
    SenderSession::ConfiguredSenders,
    capture_recommendations::Recommendations,
    MediaSender::ShutdownCallback)>;

}  // namespace openscreen::cast

#endif  // CAST_STANDALONE_SENDER_MEDIA_SENDER_H_
