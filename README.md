# MirrorDesk-Engine

AirPlay screen mirroring receiver engine for **MirrorDesk** (macOS), based on the open-source [UxPlay](https://github.com/FDH2/UxPlay) project and distributed under the **GNU General Public License v3.0 (GPLv3)**.

This is the complete source of the engine that ships inside MirrorDesk, including all of MirrorDesk's modifications.

## How MirrorDesk uses it

The engine is built as a separate helper program, `MirrorDeskEngine`, which MirrorDesk bundles at `MirrorDesk.app/Contents/Helpers/MirrorDeskEngine` and starts once per AirPlay receiver. The helper has no windows. It talks to the app over its standard streams:

- **stdin**: commands, one per line (`stop`, `volume <0..1>`, `mute <0|1>`, `record-start <audio> <video> <path>`, `record-stop`)
- **stdout**: binary messages: the compressed H.264/H.265 video frames with their display time, and command replies (see `renderers/mirrordesk_channel.h`)
- **stderr**: the engine's log

The app decodes and shows the video itself. The engine receives the AirPlay stream, plays the audio and writes direct recordings.

## MirrorDesk modifications

- `mirrordesk_helper.c`: the helper's `main()` (command reader, message channel, clean exit when the app quits)
- `renderers/video_renderer_remote.c`: a video renderer that forwards the stream to the app instead of opening a window, with H.264 and H.265 (HEVC) support, up to 4K
- `renderers/mirrordesk_channel.c/.h`: the message format between helper and app
- `renderers/at_decoder.c/.h`: AirPlay audio (AAC-ELD, AAC-LC, ALAC) decoded with Apple's AudioToolbox instead of FFmpeg
- `renderers/h264_low_latency.c/.h`: rewrites each H.264 SPS to declare no frame reordering, so frames are shown as soon as they are decoded
- `renderers/mux_renderer.c`: direct recording; silent gaps in the audio are filled so recordings keep their timing
- `renderers/audio_renderer.c`: proper flush when the sender pauses, AudioToolbox decoding
- `lib/crypto.c/.h`: OpenSSL set up so the engine can be started and stopped repeatedly
- `uxplay.cpp`: entry points used by MirrorDesk (start/stop, volume, recording), rotation, connection handling and stability fixes, and the last-client-disconnect signal used to close the app's video window

## Building (macOS)

Requirements: Xcode command line tools, CMake, and Homebrew packages `gstreamer` (with its base, good and bad plugins), `openssl@3` and `libplist`.

```sh
cmake -S . -B build
cmake --build build --target MirrorDeskEngine   # the helper MirrorDesk uses
cmake --build build --target uxplay             # the plain UxPlay command-line receiver
```

MirrorDesk embeds the GStreamer libraries and plugins the engine needs inside the app bundle; their licences (GNU LGPL 2.1 and others) ship in `MirrorDesk.app/Contents/Resources/Licenses`.

## Credits

Based on [UxPlay](https://github.com/FDH2/UxPlay) by its maintainers and contributors, which builds on earlier open-source AirPlay projects, including RPiPlay and shairplay.

## License

This project is licensed under the GNU General Public License v3.0 (GPLv3). See [LICENSE](LICENSE) for the full licence text.
