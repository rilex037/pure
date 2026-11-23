# Pure Player

Portable media player (FFmpeg + SDL3) for Linux and Windows. v1.0.2.

## Run

**Linux**
```bash
chmod +x bin/linux/PurePlayer
./bin/linux/PurePlayer file.mkv
PURE_DEBUG=1 ./bin/linux/PurePlayer file.mkv   # debug logging
```

**Windows**
```cmd
bin\windows\PurePlayer.exe file.mkv
set PURE_DEBUG=1
bin\windows\PurePlayer.exe file.mkv            # debug logging
```

## Build

**Linux**
```bash
./build_linux.sh
```

**Windows (native MinGW)**
```cmd
build_windows.bat
```

**Cross-build Windows from Linux**
```bash
./build_windows_cross.sh
```

Or with `make`:
```bash
make                      # native
make CROSS=x86_64-w64-mingw32-   # cross
```

## Layout

```
bin/        built binaries (+ DLLs / shared libs)
src/        player source (ff.c, ui_core.c, ui_draw.c, plat.c)
assets/     icons (Windows build)
licenses/   FFmpeg (LGPL), SDL3 (zlib), stb_truetype
sdl-patch/  SDL3 audio-timing patches
```

## License

MIT. See `licenses/` for third-party components.