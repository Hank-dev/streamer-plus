# streamer-plus

**Fedora → Chromecast full-screen HD+audio low-latency desktop streamer.**

Captures the desktop (PipeWire) and system audio (PulseAudio/PipeWire) via
[xdg-desktop-portal](https://flatpak.github.io/xdg-desktop-portal/), encodes to
VP8 + Opus, and streams to a stock Chromecast using the native
[Open Screen](https://github.com/chromium/openscreen) Cast V2 mirroring protocol
over TLS on port 8009.

No receiver-side app installation. No HTTP/HLS/WebRTC/ffmpeg transport.
Best-effort ~1–2 s latency on LAN.

## Download prebuilt binary

A prebuilt Fedora 44 x86-64 release is available at
https://github.com/Hank-dev/streamer-plus/releases/tag/v0.1.0.

Install its runtime dependencies:

```bash
sudo dnf install -y \
  gstreamer1 gstreamer1-plugins-base gstreamer1-plugins-good \
  pipewire-gstreamer libportal opus libvpx
```

Download `streamer-plus-v0.1.0-fedora44-x86_64.tar.gz` from the release, then
extract and run it:

```bash
tar -xzf streamer-plus-v0.1.0-fedora44-x86_64.tar.gz
./streamer_plus --capture-self-test
./streamer_plus --receiver=<CHROMECAST_IP>
```

## Build from source

### Prerequisites (Fedora 44)

```bash
sudo dnf install -y \
  clang clang-tools-extra lld gn ninja-build git python3 \
  gstreamer1 gstreamer1-plugins-base gstreamer1-plugins-good \
  gstreamer1-plugins-bad-free pipewire-gstreamer \
  libportal libportal-devel \
  opus opus-devel libvpx libvpx-devel \
  ffmpeg ffmpeg-devel pkgconf-pkg-config
```

### Checkout and compile

Use depot_tools and `gclient` to create the checkout. Open Screen tracks
DEPS-managed dependency roots, including `build/`, as gitlinks. A plain
`git clone` contains the streamer source and `.gn`, but does not populate the
DEPS-managed build, toolchain, and third-party directories.

```bash
git clone --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git "$HOME/depot_tools"
export PATH="$HOME/depot_tools:$PATH"
mkdir streamer-plus-checkout
cd streamer-plus-checkout
gclient config https://github.com/Hank-dev/streamer-plus.git
gclient sync --no-history
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
  ffmpeg_include_dirs=["/usr/include/ffmpeg"]
  libopus_include_dirs=["/usr/include/opus"]
  libvpx_include_dirs=["/usr/include"]
  external_lib_dirs=["/usr/lib64"]
  ffmpeg_lib_dirs=["/usr/lib64"]
  libopus_lib_dirs=["/usr/lib64"]
  libvpx_lib_dirs=["/usr/lib64"]
  ffmpeg_libs=["avformat","avcodec","swresample","avutil"]
  libopus_libs=["opus"]
  libvpx_libs=["vpx"]
'

ninja -C out/Release \
  cast/standalone_sender:streamer_plus_app \
  cast/standalone_sender:streamer_plus_tests
```

The binary is at `out/Release/streamer_plus`.

### Troubleshooting

- If `.gn` is missing, you are in the wrong directory or are using an obsolete
  or incomplete clone. From the source-build checkout, run `cd streamer-plus`
  and verify it with `test -f .gn`.
- If `.gn` is present but `build/config/BUILDCONFIG.gn` is missing,
  `gclient sync --no-history` was not run or did not complete. Return to the
  `streamer-plus-checkout` directory and run it successfully before `gn gen`.

## Usage

```bash
# Verify capture pipeline (no Chromecast needed)
./out/Release/streamer_plus --capture-self-test

# Dry-run: print resolved config without connecting
./out/Release/streamer_plus --dry-run --receiver=192.168.1.50

# Stream to Chromecast
./out/Release/streamer_plus --receiver=192.168.1.50
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
| `cast/standalone_sender/streamer_plus/main.cc` | CLI entry, GLib+OpenScreen dual-loop, agent wiring |
| `cast/standalone_sender/streamer_plus/config.{h,cc}` | Argument parsing, validation, dry-run |
| `cast/standalone_sender/streamer_plus/portal_screencast.{h,cc}` | xdg-desktop-portal screen selection |
| `cast/standalone_sender/streamer_plus/capture.{h,cc}` | GStreamer PipeWire+PulseAudio → bounded queues |
| `cast/standalone_sender/streamer_plus/desktop_media_sender.{h,cc}` | Capture→encoder bridge, congestion, watchdog, telemetry |
| `cast/standalone_sender/media_sender.h` | Injectable media-producer interface |
| `cast/standalone_sender/looping_file_cast_agent.{h,cc}` | Cast V2 control channel (reused, factory-injected) |

## Testing

```bash
# Unit tests
./out/Release/streamer_plus_unittests

# Capture self-test
./out/Release/streamer_plus --capture-self-test
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
