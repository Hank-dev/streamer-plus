// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <signal.h>
#include <thread>
#include <utility>
#include <vector>

#include <glib-unix.h>

#include "cast/common/public/trust_store.h"
#include "cast/standalone_sender/looping_file_cast_agent.h"
#include "cast/standalone_sender/streamer_plus/capture.h"
#include "cast/standalone_sender/streamer_plus/config.h"
#include "cast/standalone_sender/streamer_plus/desktop_media_sender.h"
#include "cast/standalone_sender/streamer_plus/portal_screencast.h"
#include "platform/api/time.h"
#include "platform/impl/platform_client_posix.h"
#include "platform/impl/task_runner.h"

namespace {

struct ActiveRun {
  GMainLoop* loop = nullptr;
  openscreen::cast::streamer_plus::GstreamerCapture* capture = nullptr;
  openscreen::cast::streamer_plus::PortalScreencast* portal = nullptr;
  bool interrupted = false;
  bool failed = false;
  bool cast_terminated = false;
  std::atomic_bool accepting_cast_termination = true;
};

gboolean OnSignal(gpointer data) {
  auto* run = static_cast<ActiveRun*>(data);
  run->interrupted = true;
  g_main_loop_quit(run->loop);
  return G_SOURCE_REMOVE;
}

gboolean OnCastTerminated(gpointer data) {
  auto* run = static_cast<ActiveRun*>(data);
  run->failed = true;
  run->cast_terminated = true;
  g_main_loop_quit(run->loop);
  return G_SOURCE_REMOVE;
}

gboolean PollCapture(gpointer data) {
  auto* run = static_cast<ActiveRun*>(data);
  if (run->portal->session_closed() || run->capture->PollRuntimeError()) {
    run->failed = true;
    g_main_loop_quit(run->loop);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

void RemoveSourceIfPresent(guint source) {
  if (g_main_context_find_source_by_id(nullptr, source) != nullptr) {
    g_source_remove(source);
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  signal(SIGPIPE, SIG_IGN);
  std::vector<const char*> arguments(argv, argv + argc);
  const auto parsed = openscreen::cast::streamer_plus::ParseCommandLine(
      argc, arguments.data());
  if (parsed.is_error()) {
    std::cerr << "streamer_plus: " << parsed.error().message() << "\n"
              << openscreen::cast::streamer_plus::Usage(argv[0]);
    return 1;
  }
  if (parsed.value().command ==
      openscreen::cast::streamer_plus::Command::kHelp) {
    std::cout << openscreen::cast::streamer_plus::Usage(argv[0]);
    return 0;
  }
  if (parsed.value().config.dry_run) {
    std::cout << openscreen::cast::streamer_plus::FormatDryRun(
        parsed.value().config);
    return 0;
  }
  if (parsed.value().config.capture_self_test) {
    openscreen::cast::streamer_plus::GstreamerCapture capture;
    if (!capture.Start(
            openscreen::cast::streamer_plus::GstreamerCapture::SyntheticPipeline())) {
      std::cerr << "streamer_plus: capture self-test initialization failed: "
                << capture.last_error() << "\n";
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
    capture.Stop();
    const auto telemetry = capture.telemetry();
    const auto video = capture.TakeLatestVideo();
    const auto audio = capture.TakeLatestAudio();
    if (telemetry.video_frames < 3 || telemetry.audio_frames < 3 ||
        telemetry.format_error || telemetry.timestamp_error || !video || !audio ||
        video->width != 320 || video->height != 240 ||
        video->i420_bytes.size() != 320u * 240u * 3u / 2u ||
        audio->channels != 2 || audio->sample_rate != 48000 ||
        audio->interleaved_samples.empty() ||
        audio->interleaved_samples.size() % 2 != 0) {
      std::cerr << "streamer_plus: capture self-test validation failed"
                << " (video=" << telemetry.video_frames
                << ", audio=" << telemetry.audio_frames << ")\n";
      return 1;
    }
    std::cout << "CAPTURE_SELF_TEST_OK video_frames=" << telemetry.video_frames
              << " audio_frames=" << telemetry.audio_frames
              << " video_dropped=" << telemetry.video_dropped
              << " audio_dropped=" << telemetry.audio_dropped << "\n";
    return 0;
  }

  const auto& config = parsed.value().config;
  openscreen::cast::streamer_plus::PortalScreencast portal;
  if (!portal.AcquireMonitor(config.portal_timeout_ms)) {
    std::cerr << "streamer_plus: " << portal.last_error() << "\n";
    return 1;
  }
  const auto& target = portal.target();
  std::cout << "streamer_plus: acquired portal monitor target="
            << target.target_object << " type="
            << (target.kind == openscreen::cast::streamer_plus::PortalTargetKind::kPipeWireSerial
                    ? "pipewire-serial"
                    : "node-id-fallback")
            << "\n";
  openscreen::cast::streamer_plus::GstreamerCapture capture;
  if (!capture.StartDesktop({.pipewire_fd = portal.pipewire_fd(),
                             .target_object = target.target_object,
                             .width = config.width,
                             .height = config.height,
                             .frames_per_second = config.frames_per_second,
                             .audio_source = config.audio_source,
                             .startup_timeout_ms = 5000})) {
    std::cerr << "streamer_plus: desktop capture initialization failed: "
              << capture.last_error() << "\n";
    return 1;
  }

  auto* const task_runner =
      new openscreen::TaskRunnerImpl(&openscreen::Clock::now);
  openscreen::PlatformClientPosix::Create(
      std::chrono::milliseconds(50),
      std::unique_ptr<openscreen::TaskRunnerImpl>(task_runner));
  std::thread task_runner_thread(
      [&] { task_runner->RunUntilStopped(); });

  GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
  ActiveRun run{.loop = loop, .capture = &capture, .portal = &portal};
  const guint sigint_source = g_unix_signal_add(SIGINT, OnSignal, &run);
  const guint sigterm_source = g_unix_signal_add(SIGTERM, OnSignal, &run);
  const guint poll_source = g_timeout_add(100, PollCapture, &run);

  // `cast_agent` is constructed and destroyed on the Open Screen task runner.
  std::unique_ptr<openscreen::cast::LoopingFileCastAgent> cast_agent;
  task_runner->PostTask([&] {
    cast_agent = std::make_unique<openscreen::cast::LoopingFileCastAgent>(
        *task_runner, openscreen::cast::CastTrustStore::Create(),
        [&run] {
          if (run.accepting_cast_termination.exchange(false)) {
            g_main_context_invoke(nullptr, OnCastTerminated, &run);
          }
        },
        [&capture](
            openscreen::cast::Environment& environment,
            const openscreen::cast::ConnectionSettings& settings,
            const openscreen::cast::SenderSession* session,
            openscreen::cast::SenderSession::ConfiguredSenders senders,
            openscreen::cast::capture_recommendations::Recommendations
                recommendations,
            openscreen::cast::MediaSender::ShutdownCallback shutdown_callback) {
          return std::make_unique<
              openscreen::cast::streamer_plus::DesktopMediaSender>(
              environment, settings, session, std::move(senders),
              recommendations, &capture, std::move(shutdown_callback));
        });

    cast_agent->Connect({
        .receiver_endpoint = config.receiver,
        .video_max_bitrate = static_cast<int>(config.video_bitrate),
        .audio_bitrate = static_cast<int>(config.audio_bitrate),
        .video_width = static_cast<int>(config.width),
        .video_height = static_cast<int>(config.height),
        .video_frames_per_second = static_cast<int>(config.frames_per_second),
        .target_playout_delay =
            std::chrono::milliseconds(config.latency_target_ms),
        .should_include_video = true,
        .should_include_audio = true,
        .use_remoting = false,
        .codec = openscreen::cast::VideoCodec::kVp8,
    });
  });

  g_main_loop_run(loop);
  RemoveSourceIfPresent(sigint_source);
  RemoveSourceIfPresent(sigterm_source);
  RemoveSourceIfPresent(poll_source);
  run.accepting_cast_termination.store(false);

  task_runner->PostTask([&] {
    cast_agent.reset();
    capture.Stop();  // Must precede closure of PortalScreencast's borrowed FD.
    portal.Close();
    task_runner->RequestStopSoon();
  });
  task_runner_thread.join();
  openscreen::PlatformClientPosix::ShutDown();
  g_main_loop_unref(loop);
  const auto telemetry = capture.telemetry();
  std::cout << "streamer_plus: capture stopped video_frames=" << telemetry.video_frames
            << " audio_frames=" << telemetry.audio_frames
            << " video_dropped=" << telemetry.video_dropped
            << " audio_dropped=" << telemetry.audio_dropped << "\n";
  if (run.failed) {
    std::cerr << "streamer_plus: "
              << (run.cast_terminated
                      ? "Cast session terminated"
                      : (portal.session_closed() ? portal.last_error()
                                                 : capture.last_error()))
              << "\n";
    return 1;
  }
  return 0;
}