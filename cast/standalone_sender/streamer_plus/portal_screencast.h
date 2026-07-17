// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STANDALONE_SENDER_STREAMER_PLUS_PORTAL_SCREENCAST_H_
#define CAST_STANDALONE_SENDER_STREAMER_PLUS_PORTAL_SCREENCAST_H_

#include <cstdint>
#include <string>

#include <gio/gio.h>

namespace openscreen::cast::streamer_plus {

enum class PortalTargetKind { kPipeWireSerial, kNodeIdCompatibilityFallback };

struct PortalStreamTarget {
  std::string target_object;
  PortalTargetKind kind = PortalTargetKind::kPipeWireSerial;
  uint32_t node_id = 0;
  uint64_t serial = 0;
  bool used_node_id_fallback = false;
};

// This parser is deliberately independent of a live portal request so malformed
// portal replies can be covered in headless tests.
bool ParsePortalStreamTarget(GVariant* streams,
                             PortalStreamTarget* target,
                             std::string* error);

// Owns the portal session and its restricted PipeWire remote FD.  The FD is
// borrowed by GstreamerCapture and remains valid until this object is closed.
class PortalScreencast {
 public:
  PortalScreencast();
  ~PortalScreencast();
  PortalScreencast(const PortalScreencast&) = delete;
  PortalScreencast& operator=(const PortalScreencast&) = delete;

  bool AcquireMonitor(uint32_t timeout_ms);
  void Close();

  int pipewire_fd() const { return pipewire_fd_; }
  const PortalStreamTarget& target() const { return target_; }
  bool session_closed() const { return session_closed_; }
  const std::string& last_error() const { return last_error_; }

 private:
  static void OnCreate(GObject*, GAsyncResult*, gpointer);
  static void OnStart(GObject*, GAsyncResult*, gpointer);
  static gboolean OnTimeout(gpointer);
  static void OnClosed(gpointer, gpointer);
  void FinishAcquisition();
  void SetError(std::string error);

  void* portal_ = nullptr;
  void* session_ = nullptr;
  void* cancellable_ = nullptr;
  GMainLoop* loop_ = nullptr;
  guint timeout_source_ = 0;
  gulong closed_handler_ = 0;
  int pipewire_fd_ = -1;
  PortalStreamTarget target_;
  std::string last_error_;
  bool awaiting_create_ = false;
  bool awaiting_start_ = false;
  bool deadline_ = false;
  bool session_closed_ = false;
};

}  // namespace openscreen::cast::streamer_plus

#endif  // CAST_STANDALONE_SENDER_STREAMER_PLUS_PORTAL_SCREENCAST_H_
