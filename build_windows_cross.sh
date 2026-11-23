#!/bin/bash
# build_windows_cross.sh - cross-compile PurePlayer.exe for Windows
# 10/11 64-bit from Linux, no Windows machine required.
#
# Layers:
#   SDL 3.4.8 (patched, static, MinGW) - video (GDI/D3D via SDL core), audio WASAPI
#   FFmpeg 8.1 LGPL win64 shared (BtbN) - av*.dll bundled next to the exe
#   MinGW-w64 gcc 14 (win32 threads flavor) - from Debian packages, extracted
#   user-side (no root) into ../mingw/root
#
# Usage: ./build_windows_cross.sh
# Env:   SDL_ROOT  (default ../..//src-cache/SDL - the patched SDL 3.4.8 tree)
#        MINGW    (default ../..//mingw/root - extracted cross toolchain)
#        FFWIN    (default ../..//deps/ffmpeg-win64 - BtbN win64 shared dev)
set -euo pipefail
W=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$W")
SDL="${SDL_ROOT:-$ROOT/src-cache/SDL}"
MINGW="${MINGW:-$ROOT/mingw/root}"
FFWIN="${FFWIN:-$ROOT/deps/ffmpeg-win64}"
CB="$MINGW/cmake-pkg/usr/bin"
CC="$MINGW/usr/bin/x86_64-w64-mingw32-gcc-win32"
WINDRES="$MINGW/usr/bin/x86_64-w64-mingw32-windres"
export PATH="$CB:$MINGW/usr/bin:$PATH"
export LD_LIBRARY_PATH="$MINGW/cmake-pkg/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

for f in "$CC" "$WINDRES" "$CB/cmake" "$CB/ninja"; do
  [ -x "$f" ] || { echo "ERROR: missing $f - see scripts/README-toolchain"; exit 1; }
done
[ -f "$FFWIN/include/libavformat/avformat.h" ] || { echo "ERROR: $FFWIN missing (BtbN win64 shared dev zip)"; exit 1; }
[ -f "$SDL/src/audio/SDL_audio.c" ] || { echo "ERROR: $SDL missing (git clone release-3.4.8, run sdl-patch/ scripts)"; exit 1; }
grep -q "GetDevicePending" "$SDL/src/audio/SDL_sysaudio.h" || {
  echo "ERROR: SDL tree is NOT patched - run:"; echo "  sdl-patch/patch_sdl_audio_position.sh $SDL"; echo "  sdl-patch/patch_sdl_part2.sh $SDL"; exit 1; }

# ------------------------------------------------------------- 1. SDL (static)
if [ ! -f "$SDL/build-win64/libSDL3.a" ]; then
  echo "== configuring SDL (MinGW, WASAPI) =="
  cmake -S "$SDL" -B "$SDL/build-win64" -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER="$MINGW/usr/bin/x86_64-w64-mingw32-g++-win32" \
    -DCMAKE_CXX_COMPILER_WORKS=1 \
    -DCMAKE_RC_COMPILER="$WINDRES" \
    -DCMAKE_SYSTEM_PROCESSOR=x86_64 -DCMAKE_C_STANDARD=17 \
    -DSDL_STATIC=ON -DSDL_SHARED=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    -DSDL_RENDER=OFF -DSDL_GPU=OFF -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF \
    -DSDL_HIDAPI=OFF -DSDL_SENSOR=OFF -DSDL_CAMERA=OFF -DSDL_VULKAN=OFF \
    -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_POWER=OFF -DSDL_TRAY=OFF
  ninja -C "$SDL/build-win64" -j"$(nproc)" SDL3-static
fi

# ------------------------------------------------------------- 2. resources
# NOTE: upstream assets/app.rc #includes <winver.h>, which windres's bundled
# preprocessor cannot handle in this cross setup; app_min.rc is the identical
# resource content with numeric FILEOS/FILETYPE literals so it needs no headers.
echo "== compiling resources =="
"$WINDRES" --target=pe-x86-64 -I "$W" "$W/assets/app_min.rc" -O coff -o "$W/app_res.o"

# ------------------------------------------------------------- 3. exe
echo "== linking PurePlayer.exe =="
"$CC" -O2 -DNDEBUG -mwindows -static-libgcc -ffunction-sections -fdata-sections \
  -I "$W/src" -I "$FFWIN/include" -I "$SDL/include" \
  "$W/src/ff.c" "$W/src/ui_core.c" "$W/src/ui_draw.c" "$W/src/plat.c" "$W/app_res.o" \
  -o "$W/bin/windows/PurePlayer.exe" -Wl,--gc-sections -s \
  "$FFWIN/lib/avformat.lib" "$FFWIN/lib/avcodec.lib" \
  "$FFWIN/lib/swscale.lib" "$FFWIN/lib/swresample.lib" "$FFWIN/lib/avutil.lib" \
  "$SDL/build-win64/libSDL3.a" \
  -lm -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion \
  -luuid -ladvapi32 -lsetupapi -lshell32 -lcfgmgr32 -lcomdlg32

# ------------------------------------------------------------- 4. dlls
mkdir -p "$W/bin/windows"
for d in avcodec-62 avformat-62 avutil-60 swscale-9 swresample-6; do
  cp "$FFWIN/bin/$d.dll" "$W/bin/windows/"
done

# ------------------------------------------------------------- 5. report
BAD=$(objdump -x "$W/bin/windows/PurePlayer.exe" | grep "DLL Name" | grep -vcE "ADVAPI32|GDI32|IMM32|KERNEL32|SETUPAPI|SHELL32|USER32|VERSION|WINMM|ole32|msvcrt|avcodec-62|avformat-62|avutil-60|swscale-9|swresample-6" || true)
if [ "${BAD:-0}" -ne 0 ]; then
  echo "FATAL: unexpected DLL dependency in exe:" >&2
  objdump -x "$W/bin/windows/PurePlayer.exe" | grep "DLL Name" | grep -vE "ADVAPI32|GDI32|IMM32|KERNEL32|SETUPAPI|SHELL32|USER32|VERSION|WINMM|ole32|msvcrt|avcodec-62|avformat-62|avutil-60|swscale-9|swresample-6" >&2
  exit 1
fi
echo "Built bin/windows/PurePlayer.exe (deps: av*.dll bundled, system dlls only)"
strings "$W/bin/windows/PurePlayer.exe" | grep -q "pure-audio" && echo "debug logging: present (PURE_DEBUG=1)"
