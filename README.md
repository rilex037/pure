# Pure Player

A portable, lightweight media player built with **FFmpeg** and **SDL3**, available for **Linux** and **Windows**.

[![Version](https://img.shields.io/badge/version-1.0.2-blue.svg)](https://github.com/rilex037/pure/releases)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows-lightgrey.svg)](https://github.com/rilex037/pure/releases)

---

## 📖 About

Pure Player is portable media player that plays common audio and video formats using FFmpeg for decoding and SDL3 for rendering and audio output. It is designed to be lightweight, self-contained, and easy to run without installation.

### Key highlights

- **Cross-platform**: Linux (x86_64) and Windows (10/11, 64-bit)
- **Portable**: No installation required — run the binary directly
- **FFmpeg-powered**: Broad codec and container support
- **SDL3 rendering**: Hardware-accelerated video output and low-latency audio
- **Native PipeWire audio on Linux** (with PulseAudio and ALSA fallbacks)
- **WASAPI audio on Windows**
- **Debuggable**: `PURE_DEBUG=1` runtime diagnostics for audio/video sync

---

## 🚀 Quick Start

### Download

Grab the latest prebuilt binaries from the [Releases page](https://github.com/rilex037/pure/releases).

### Run

**Linux:**
```bash
chmod +x bin/linux/PurePlayer
./bin/linux/PowerPlayer file.mkv
```

**Windows:**
```cmd
bin\windows\PurePlayer.exe file.mkv
```

Enable debug logging (audio/video pipeline diagnostics):
```bash
# Linux
PURE_DEBUG=1 ./bin/linux/PurePlayer file.mkv

# Windows
set PURE_DEBUG=1
bin\windows\PurePlayer.exe file.mkv
```

---

## 📦 Project Structure

```
pure/
├── bin/                # Build output (portable binaries + DLLs/shared libs)
├── src/                # Player source code (ff.c, ui_core.c, ui_draw.c, plat.c)
├── assets/             # Icons and resources used by the Windows build
├── licenses/           # FFmpeg (LGPL), SDL (zlib), stb_truetype licenses
├── sdl-patch/          # SDL3 audio-timing patches (portable since v1.0.2)
├── build_linux.sh      # Linux build script (fetches + builds SDL3, FFmpeg, player)
├── build_windows.bat   # Native Windows build (MinGW + SDL3 dev files)
├── build_windows_cross.sh  # Cross-build Windows from Linux (MinGW toolchain)
├── Makefile            # Unified Makefile for Linux / Windows / cross builds
├── run_player.sh       # Convenience runner script
└── VERSION             # Current version (1.0.2)
```

---

## 🛠️ Building from Source

### Linux (native)

```bash
./build_linux.sh
```

This fetches and builds SDL 3.4.8 (patched), FFmpeg, and the player, outputting into `bin/linux/`.

Requirements: `pkg-config` files for FFmpeg + SDL3, a C compiler, `make`.

### Windows (native, MinGW)

```cmd
build_windows.bat
```

Requires local MinGW + SDL3 dev files. FFmpeg dev files should be in `./ffmpeg`, SDL3 mingw dev files in `./sdl3`.

### Cross-build Windows from Linux

```bash
./build_windows_cross.sh
```

Requires the MinGW toolchain, BtbN win64 FFmpeg dev zip, and the patched SDL tree (see script header).

### Using Make

```bash
# Linux
make

# Windows (MinGW)
make

# Cross-build on Linux
make CROSS=x86_64-w64-mingw32-
```

---

## 🔧 Configuration & Patches

### SDL3 audio position patch

The `sdl-patch/` directory contains audio-timing patches that enable accurate audio-master clocking (replacing the guessed-device-buffer approach). These are essential for correct A/V sync on Linux.

```bash
sdl-patch/patch_sdl_audio_position.sh <SDL-source-root>   # part 1
sdl-patch/patch_sdl_part2.sh <SDL-source-root>             # part 2
sdl-patch/patch_player_sync.sh <player-src-dir>            # player side
```

SDL root defaults to the `SDL_ROOT` env var or `../../src-cache/SDL`.  
Alternatively, apply `sdl3-pure-audio.patch` on a pristine SDL 3.4.8 tree:
```bash
git apply sdl3-pure-audio.patch
```

---

## 📄 License

This project is licensed under the **MIT License**. See [LICENSE](LICENSE) for details.

Third-party components included in the `licenses/` directory:
- **FFmpeg** — LGPL
- **SDL3** — zlib
- **stb_truetype** — public domain / MIT

---

## 🤝 Contributing

Contributions are welcome! Please open an issue or submit a pull request.

---

## 🙏 Acknowledgments

- [FFmpeg](https://ffmpeg.org/) — multimedia framework
- [SDL3](https://libsdl.org/) — cross-platform multimedia library
- [stb_truetype](https://github.com/nothings/stb) — font rendering