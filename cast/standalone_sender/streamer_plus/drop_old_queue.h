// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STANDALONE_SENDER_STREAMER_PLUS_DROP_OLD_QUEUE_H_
#define CAST_STANDALONE_SENDER_STREAMER_PLUS_DROP_OLD_QUEUE_H_

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

#include "util/osp_logging.h"

namespace openscreen::cast::streamer_plus {

// A small queue for real-time producers. Consumers should normally use
// TakeLatest() so they never add latency by processing stale media.
template <typename T>
class DropOldQueue {
 public:
  explicit DropOldQueue(size_t capacity) : capacity_(capacity) {
    OSP_CHECK_GT(capacity_, 0u);
  }

  DropOldQueue(const DropOldQueue&) = delete;
  DropOldQueue& operator=(const DropOldQueue&) = delete;

  void Push(T value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.size() == capacity_) {
      entries_.pop_front();
      ++dropped_;
    }
    entries_.push_back(std::move(value));
  }

  std::optional<T> TakeLatest() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.empty()) {
      return std::nullopt;
    }
    T latest = std::move(entries_.back());
    dropped_ += entries_.size() - 1;
    entries_.clear();
    return latest;
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
  }

  size_t dropped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
  }

 private:
  const size_t capacity_;
  mutable std::mutex mutex_;
  std::deque<T> entries_;
  size_t dropped_ = 0;
};

}  // namespace openscreen::cast::streamer_plus

#endif  // CAST_STANDALONE_SENDER_STREAMER_PLUS_DROP_OLD_QUEUE_H_
