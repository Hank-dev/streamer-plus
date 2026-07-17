# streamer-plus

**Fedora → Chromecast full-screen HD+audio low-latency desktop streamer.**

Captures the desktop (PipeWire) and system audio (PulseAudio/PipeWire) via
[xdg-desktop-portal](https://flatpak.github.io/xdg-desktop-portal/), encodes to
VP8 + Opus, and streams to a stock Chromecast using the native
[Open Screen](https://github.com/chromium/openscreen) Cast V2 mirroring protocol
over TLS on port 8009.

No receiver-side app installation. No HTTP/HLS/WebRTC/ffmpeg transport.
Best-effort ~1–2 s latency on LAN.

## Build

### Prerequisites (Fedora 44)

```bash
sudo dnf install -y \
  clang clang-tools-extra lld ninja-build git python3 \
  gstreamer1 gstreamer1-plugins-base gstreamer1-plugins-good \
  gstreamer1-plugins-bad-free pipewire-gstreamer \
  libportal libportal-devel \
  opus opus-devel libvpx libvpx-devel \
  ffmpeg ffmpeg-devel pkgconf-pkg-config
```

### Compile

This project is built as part of the Open Screen tree. Clone and generate:

```bash
git clone https://github.com/Hank-dev/streamer-plus.git
cd streamer-plus

gn gen out/Release --args='
  is_debug=false
  use_sysroot=false
  use_custom_libcxx=true
  clang_base_path="/usr"
  clang_use_chrome_plugins=false
  use_lld=true
  have_ffmpeg=true
  have_libopus=true
  have_libvpx=true
'

ninja -C out/Release \
  cast/standalone_sender:streamer_plus_app \
  cast/standalone_sender:streamer_plus_tests
```

The binary is at `out/Release/streamer_plus`.

## Usage

```bash
# Verify capture pipeline (no Chromecast needed)
./streamer_plus --capture-self-test

# Dry-run: print resolved config without connecting
./streamer_plus --dry-run --receiver=192.168.1.50

# Stream to Chromecast
./streamer_plus --receiver=192.168.1.50
```

A screen-sharing dialog will appear via xdg-desktop-portal. After approval,
desktop video and system audio are streamed to the Chromecast.

### Options

| Flag | Default | Description |
|------|---------|-------------|
| `--receiver=ADDR[:PORT]` | required | Chromecast IP; bare IP defaults to port 8009 |
| `--width=N` | 1920 | Capture width (must be even) |
| `--height=N` | 1080 | Capture height (must be even) |
| `--fps=N` | 30 | Frames per second |
| `--video-bitrate=N` | 8000000 | Video bitrate cap (bps) |
| `--audio-bitrate=N` | 128000 | Audio bitrate (bps) |
| `--latency-target-ms=N` | 500 | Target playout delay (1–5000 ms) |
| `--audio-source=NAME` | `@DEFAULT_MONITOR@` | PulseAudio sink monitor source |
| `--portal-timeout-ms=N` | 120000 | Portal response timeout |
| `--dry-run` | | Print config and exit |
| `--capture-self-test` | | Test GStreamer capture pipeline |
| `--help` | | Show usage |

### Telemetry

During streaming, one JSON object per second is printed to stdout:

```json
{"event":"streamer_plus_telemetry","state":"streaming","uptime_ms":12345,
 "capture":{"video_frames":300,"audio_frames":450,...},
 "encode":{"video_completed":298,"video_bytes":1234567,...},
 "network":{"estimate_bps":8000000,"video_target_bps":7500000,...}}
```

## Architecture

```
┌──────────────────┐    ┌─────────────────────┐    ┌──────────────────┐
│ xdg-desktop-     │───▶│ GstreamerCapture     │───▶│ DesktopMediaSender│
│ portal (PipeWire)│    │ (pipewiresrc +       │    │ (VP8 + Opus       │
│                  │    │  pulsesrc → appsink) │    │  encoders)        │
└──────────────────┘    └─────────────────────┘    └────────┬─────────┘
                                                            │
                                                   ┌────────▼─────────┐
                                                   │ LoopingFileCast   │
                                                   │ Agent (Cast V2    │
                                                   │ TLS → SenderSession│
                                                   │ → RTP/RTCP)       │
                                                   └────────┬──────────┘
                                                            │
                                                   ┌────────▼──────────┐
                                                   │ Chromecast (8009)  │
                                                   └───────────────────┘
```

### Key components

| File | Role |
|------|------|
| `streamer_plus/main.cc` | CLI entry, GLib+OpenScreen dual-loop, agent wiring |
| `streamer_plus/config.{h,cc}` | Argument parsing, validation, dry-run |
| `streamer_plus/portal_screencast.{h,cc}` | xdg-desktop-portal screen selection |
| `streamer_plus/capture.{h,cc}` | GStreamer PipeWire+PulseAudio → bounded queues |
| `streamer_plus/desktop_media_sender.{h,cc}` | Capture→encoder bridge, congestion, watchdog, telemetry |
| `media_sender.h` | Injectable media-producer interface |
| `looping_file_cast_agent.{h,cc}` | Cast V2 control channel (reused, factory-injected) |

## Testing

```bash
# Unit tests
./streamer_plus_unittests

# Capture self-test
./streamer_plus --capture-self-test
```

## Limitations

- **No sub-500 ms latency guarantee.** Best-effort ~1–2 s on a healthy LAN.
- **Hardware-dependent.** Requires a graphical PipeWire session and a
  Chromecast on the same network.
- **VP8 video + Opus audio only.** No HEVC/AV1 or AAC.
- **No transcoding or resampling.** Capture must produce I420 video and
  F32LE 48 kHz audio matching the negotiated config.

## License

Built on [Open Screen](https://github.com/chromium/openscreen) (BSD-style
license). See LICENSE file in the repository root.
