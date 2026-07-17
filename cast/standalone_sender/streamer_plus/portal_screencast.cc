// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/standalone_sender/streamer_plus/portal_screencast.h"

#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

#include <libportal/portal.h>
#include <libportal/portal-helpers.h>
#include <libportal/remote.h>

namespace openscreen::cast::streamer_plus {
namespace {

XdpPortal* Portal(void* portal) {
  return static_cast<XdpPortal*>(portal);
}
XdpSession* Session(void* session) {
  return static_cast<XdpSession*>(session);
}
GCancellable* Cancellable(void* cancellable) {
  return static_cast<GCancellable*>(cancellable);
}

}  // namespace

bool ParsePortalStreamTarget(GVariant* streams,
                             PortalStreamTarget* target,
                             std::string* error) {
  if (streams == nullptr || !g_variant_is_of_type(streams, G_VARIANT_TYPE("a(ua{sv})"))) {
    *error = "portal returned malformed streams (expected a(ua{sv}))";
    return false;
  }
  if (g_variant_n_children(streams) != 1) {
    *error = "portal must return exactly one selected monitor stream";
    return false;
  }
  GVariant* tuple = g_variant_get_child_value(streams, 0);
  guint32 node_id = 0;
  GVariant* properties = nullptr;
  g_variant_get(tuple, "(u@a{sv})", &node_id, &properties);
  g_variant_unref(tuple);
  GVariant* serial = g_variant_lookup_value(properties, "pipewire-serial", nullptr);
  if (serial != nullptr) {
    if (!g_variant_is_of_type(serial, G_VARIANT_TYPE_UINT64)) {
      g_variant_unref(serial);
      g_variant_unref(properties);
      *error = "portal returned pipewire-serial with invalid type";
      return false;
    }
    const guint64 value = g_variant_get_uint64(serial);
    g_variant_unref(serial);
    g_variant_unref(properties);
    if (value == 0) {
      *error = "portal returned zero pipewire-serial";
      return false;
    }
    *target = {.target_object = std::to_string(value),
               .kind = PortalTargetKind::kPipeWireSerial,
               .node_id = node_id,
               .serial = value,
               .used_node_id_fallback = false};
    return true;
  }
  g_variant_unref(properties);
  if (node_id == 0) {
    *error = "portal omitted pipewire-serial and returned zero node ID";
    return false;
  }
  *target = {.target_object = std::to_string(node_id),
             .kind = PortalTargetKind::kNodeIdCompatibilityFallback,
             .node_id = node_id,
             .serial = 0,
             .used_node_id_fallback = true};
  return true;
}

PortalScreencast::PortalScreencast() = default;

PortalScreencast::~PortalScreencast() {
  Close();
  if (cancellable_ != nullptr) {
    g_object_unref(Cancellable(cancellable_));
  }
  if (portal_ != nullptr) {
    g_object_unref(Portal(portal_));
  }
  if (loop_ != nullptr) {
    g_main_loop_unref(loop_);
  }
}

void PortalScreencast::SetError(std::string error) {
  if (last_error_.empty()) {
    last_error_ = std::move(error);
  }
}

void PortalScreencast::FinishAcquisition() {
  if (loop_ != nullptr) {
    g_main_loop_quit(loop_);
  }
}

gboolean PortalScreencast::OnTimeout(gpointer data) {
  auto* self = static_cast<PortalScreencast*>(data);
  self->timeout_source_ = 0;
  self->deadline_ = true;
  self->SetError("portal screencast request timed out");
  g_cancellable_cancel(Cancellable(self->cancellable_));
  return G_SOURCE_REMOVE;
}

void PortalScreencast::OnClosed(gpointer, gpointer data) {
  auto* self = static_cast<PortalScreencast*>(data);
  self->session_closed_ = true;
  self->SetError("portal screencast session closed; desktop permission may have been revoked");
  if (self->awaiting_create_ || self->awaiting_start_) {
    g_cancellable_cancel(Cancellable(self->cancellable_));
    return;
  }
  self->FinishAcquisition();
}

void PortalScreencast::OnCreate(GObject*, GAsyncResult* result, gpointer data) {
  auto* self = static_cast<PortalScreencast*>(data);
  self->awaiting_create_ = false;
  GError* error = nullptr;
  XdpSession* session =
      xdp_portal_create_screencast_session_finish(Portal(self->portal_), result, &error);
  if (session == nullptr) {
    if (self->deadline_) {
      self->SetError("portal screencast request timed out");
    } else if (error != nullptr &&
               g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      self->SetError("portal screencast request was cancelled by user");
    } else {
      self->SetError("portal screencast create failed: " +
                     std::string(error ? error->message : "unknown error"));
    }
    if (error != nullptr) g_error_free(error);
    self->FinishAcquisition();
    return;
  }
  if (error != nullptr) g_error_free(error);
  self->session_ = session;
  self->closed_handler_ = g_signal_connect(session, "closed", G_CALLBACK(OnClosed), self);
  self->awaiting_start_ = true;
  xdp_session_start(session, nullptr, Cancellable(self->cancellable_), OnStart, self);
}

void PortalScreencast::OnStart(GObject*, GAsyncResult* result, gpointer data) {
  auto* self = static_cast<PortalScreencast*>(data);
  self->awaiting_start_ = false;
  GError* error = nullptr;
  if (!xdp_session_start_finish(Session(self->session_), result, &error)) {
    if (self->deadline_) {
      self->SetError("portal screencast request timed out");
    } else if (error != nullptr &&
               g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      self->SetError("portal screencast request was cancelled by user");
    } else {
      self->SetError("portal screencast start failed: " +
                     std::string(error ? error->message : "unknown error"));
    }
    if (error != nullptr) g_error_free(error);
    self->FinishAcquisition();
    return;
  }
  if (error != nullptr) g_error_free(error);
  GVariant* streams = xdp_session_get_streams(Session(self->session_));
  if (!ParsePortalStreamTarget(streams, &self->target_, &self->last_error_)) {
    self->FinishAcquisition();
    return;
  }
  if (self->target_.used_node_id_fallback) {
    std::fprintf(stderr,
                 "streamer_plus: portal did not return pipewire-serial; using "
                 "transient node ID compatibility fallback\n");
  }
  self->pipewire_fd_ = xdp_session_open_pipewire_remote(Session(self->session_));
  if (self->pipewire_fd_ < 0 || fcntl(self->pipewire_fd_, F_GETFD) == -1) {
    if (self->pipewire_fd_ >= 0) close(self->pipewire_fd_);
    self->pipewire_fd_ = -1;
    self->SetError("portal failed to open a valid restricted PipeWire remote");
    self->FinishAcquisition();
    return;
  }
  const int flags = fcntl(self->pipewire_fd_, F_GETFD);
  if (fcntl(self->pipewire_fd_, F_SETFD, flags | FD_CLOEXEC) == -1) {
    close(self->pipewire_fd_);
    self->pipewire_fd_ = -1;
    self->SetError("failed to mark restricted PipeWire FD close-on-exec");
  }
  self->FinishAcquisition();
}

bool PortalScreencast::AcquireMonitor(uint32_t timeout_ms) {
  if (portal_ != nullptr || timeout_ms == 0) {
    SetError("portal acquisition is already initialized or has invalid timeout");
    return false;
  }
  GError* portal_error = nullptr;
  portal_ = xdp_portal_initable_new(&portal_error);
  if (portal_ == nullptr) {
    SetError("failed to connect to xdg-desktop-portal: " +
             std::string(portal_error ? portal_error->message : "unknown error"));
    if (portal_error != nullptr) {
      g_error_free(portal_error);
    }
    return false;
  }
  if (portal_error != nullptr) {
    g_error_free(portal_error);
  }
  cancellable_ = g_cancellable_new();
  loop_ = g_main_loop_new(nullptr, FALSE);
  if (portal_ == nullptr || cancellable_ == nullptr || loop_ == nullptr) {
    SetError("failed to initialize libportal request state");
    return false;
  }
  timeout_source_ = g_timeout_add(timeout_ms, OnTimeout, this);
  awaiting_create_ = true;
  xdp_portal_create_screencast_session(
      Portal(portal_), XDP_OUTPUT_MONITOR, XDP_SCREENCAST_FLAG_NONE,
      XDP_CURSOR_MODE_EMBEDDED, XDP_PERSIST_MODE_NONE, nullptr,
      Cancellable(cancellable_), OnCreate, this);
  g_main_loop_run(loop_);
  if (timeout_source_ != 0) {
    g_source_remove(timeout_source_);
    timeout_source_ = 0;
  }
  return pipewire_fd_ >= 0 && !session_closed_ && last_error_.empty();
}

void PortalScreencast::Close() {
  if (timeout_source_ != 0) {
    g_source_remove(timeout_source_);
    timeout_source_ = 0;
  }
  if (pipewire_fd_ >= 0) {
    close(pipewire_fd_);
    pipewire_fd_ = -1;
  }
  if (session_ != nullptr) {
    if (closed_handler_ != 0) {
      g_signal_handler_disconnect(Session(session_), closed_handler_);
      closed_handler_ = 0;
    }
    if (!session_closed_) xdp_session_close(Session(session_));
    g_object_unref(Session(session_));
    session_ = nullptr;
  }
}

}  // namespace openscreen::cast::streamer_plus
