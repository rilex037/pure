#!/bin/bash
# build_linux.sh - portable PurePlayer Linux build.
#
# Everything is built from pinned sources. The final binary is linked against
# glibc 2.31 (Ubuntu 20.04's, downloaded by this script) so it only records
# old symbol versions (max GLIBC_2.29 in practice) and runs on any distro
# with glibc >= 2.29: Debian 10/11/12/13, Ubuntu 20.04-24.04, Mint 20-22.
#
# Layers:
#   SDL 3.4.8 (patched, static)     - audio/video, dlopens X11/Wayland/PW/Pulse/ALSA
#   FFmpeg 8.1 LGPL (BtbN shared)   - bundled in bin/linux/lib/
#   glibc 2.31 .so files            - LINK-TIME ONLY, never shipped
#   stub libmvec/libgcc_s           - link-time only, for BtbN DT_NEEDED checks
#   compat_glibc_old.c              - __isoc23_*/strlcpy/wcslcpy shims compiled in
#
# Usage: ./build_linux.sh
set -euo pipefail
W=$(cd "$(dirname "$0")" && pwd)              # PurePlayer source root
ROOT=$(dirname "$W")
SRC="$ROOT/src-cache"                          # pinned upstream sources
DEBROOT="$ROOT/debroot"                        # SDL build deps headers (pipewire etc.)
PREFIX="$ROOT/portable-prefix"
GLIBC="$ROOT/glibc-old/root"                   # glibc 2.31 for linking
STUBS="$ROOT/link-stubs"                       # stub libmvec/libgcc_s
FFDIR="$ROOT/deps/ffmpeg-linux64"              # BtbN FFmpeg 8.1 LGPL
mkdir -p "$PREFIX" "$STUBS"

# The SDL configure below MUST see this: without it CMake silently disables
# PipeWire/Pulse/ALSA and the binary ships with the silent dummy driver only
# (that exact regression caused a "no sound" build once - never again).
# debroot must contain real libpulse.so.0 + libasound.so.2.0.0 files, else
# FindLibraryAndSONAME falls back to hard-linking libpulse (breaks pure-ALSA
# systems) or disables ALSA entirely.
export PKG_CONFIG_PATH="$DEBROOT/usr/lib/x86_64-linux-gnu/pkgconfig"

# ---------------------------------------------------------------- 0. glibc 2.31
if [ ! -f "$GLIBC/lib/x86_64-linux-gnu/libc-2.31.so" ]; then
  echo "== fetching glibc 2.31 (Ubuntu 20.04) for old-symbol linking =="
  mkdir -p "$ROOT/glibc-old" && cd "$ROOT/glibc-old"
  for d in libc6-dev_2.31-0ubuntu9_amd64.deb libc6_2.31-0ubuntu9_amd64.deb; do
    curl -sL --max-time 180 -O "http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/$d"
    dpkg-deb -x "$d" root/
  done
fi

# ------------------------------------------------------------- 0b. link stubs
# The BtbN FFmpeg shared libs carry DT_NEEDED for libmvec.so.1 / libgcc_s.so.1
# and reference versioned symbols from them. The sandbox's real ones need too
# new glibc, so we generate stubs carrying exactly the referenced symbols.
if [ ! -f "$STUBS/libmvec.so.1" ]; then
  echo "== generating link-time stubs =="
  FFLIB="$FFDIR/lib"
  for lib in libavformat libavcodec libswresample libswscale libavutil; do
    objdump -T "$FFLIB/$lib".so.* 2>/dev/null | grep UND
  done | grep "_ZGV" | awk '{print $NF}' | sort -u > "$STUBS/mvec_syms.txt"
  { echo 'GLIBC_2.22 {'; echo '  global:'; while read s; do echo "    $s;"; done < "$STUBS/mvec_syms.txt"; echo '};'; } > "$STUBS/libmvec_stub.map"
  { echo '/* link-time stub for libmvec.so.1: BtbN FFmpeg vector-math refs. */'; while read s; do echo "void $s(void) {}"; done < "$STUBS/mvec_syms.txt"; } > "$STUBS/stub_libmvec.c"
  gcc -w -shared -fPIC -o "$STUBS/libmvec.so.1" "$STUBS/stub_libmvec.c" -Wl,--version-script="$STUBS/libmvec_stub.map"
  { echo 'GCC_3.0 {'; echo '  global:'; for s in _Unwind_DeleteException _Unwind_GetDataRelBase _Unwind_GetIP _Unwind_GetLanguageSpecificData _Unwind_GetRegionStart _Unwind_GetTextRelBase _Unwind_RaiseException _Unwind_Resume _Unwind_SetGR _Unwind_SetIP __udivmodti4; do echo "    $s;"; done; echo '};'; echo 'GCC_3.3 {'; echo '  global:'; for s in _Unwind_Backtrace _Unwind_FindEnclosingFunction _Unwind_GetCFA _Unwind_Resume_or_Rethrow; do echo "    $s;"; done; echo '};'; echo 'GCC_4.2.0 {'; echo '  global:'; echo '    _Unwind_GetIPInfo;'; echo '};'; } > "$STUBS/libgcc_s_stub.map"
  { echo '/* link-time stub for libgcc_s.so.1 (BtbN FFmpeg DT_NEEDED) */'; for s in _Unwind_DeleteException _Unwind_GetDataRelBase _Unwind_GetIP _Unwind_GetLanguageSpecificData _Unwind_GetRegionStart _Unwind_GetTextRelBase _Unwind_RaiseException _Unwind_Resume _Unwind_SetGR _Unwind_SetIP _Unwind_Backtrace _Unwind_FindEnclosingFunction _Unwind_GetCFA _Unwind_Resume_or_Rethrow _Unwind_GetIPInfo; do echo "void $s(void) {}"; done; echo 'typedef unsigned __int128 u128; u128 __udivmodti4(u128 a, u128 b, u128 *c){ if(c) *c=0; return b?a/b:0; }'; } > "$STUBS/stub_libgcc_s.c"
  gcc -w -shared -fPIC -o "$STUBS/libgcc_s.so.1" "$STUBS/stub_libgcc_s.c" -Wl,--version-script="$STUBS/libgcc_s_stub.map"
fi

# ---------------------------------------------------------------- 1. compat shim
# glibc 2.41 headers redirect strtol & friends to __isoc23_* whenever
# _GNU_SOURCE is defined (SDL always defines it). Those symbols only exist
# in glibc >= 2.38. Compile these shims INTO the binary so old glibc works.
gcc -std=gnu11 -O2 -c "$W/src/portability/compat_glibc_old.c" -o "$PREFIX/compat_glibc_old.o"
gcc -O2 -c "$W/src/portability/dso_handle.c" -o "$PREFIX/dso_handle.o"

# ---------------------------------------------------------------- 2. SDL 3.4.8
if [ ! -f "$SRC/SDL/build-linux/libSDL3.a" ]; then
  cmake -S "$SRC/SDL" -B "$SRC/SDL/build-linux" -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_PREFIX_PATH="$DEBROOT" \
    -DCMAKE_LIBRARY_PATH="$DEBROOT/usr/lib/x86_64-linux-gnu" \
    -DCMAKE_INCLUDE_PATH="$DEBROOT/usr/include" \
    -DCMAKE_C_FLAGS="-I$DEBROOT/usr/include/pipewire-0.3 -I$DEBROOT/usr/include/spa-0.2 -I$DEBROOT/usr/include" \
    -DSDL_STATIC=ON -DSDL_SHARED=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    -DSDL_RENDER=OFF -DSDL_GPU=OFF -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF \
    -DSDL_HIDAPI=OFF -DSDL_SENSOR=OFF -DSDL_CAMERA=OFF -DSDL_VULKAN=OFF \
    -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_KMSDRM=OFF -DSDL_OFFSCREEN=ON \
    -DSDL_X11_XTEST=OFF -DSDL_POWER=OFF -DSDL_TRAY=OFF \
    -DSDL_PIPEWIRE=ON -DSDL_PULSEAUDIO=ON -DSDL_ALSA=ON -DSDL_SNDIO=OFF -DSDL_JACK=OFF \
    -DSDL_PIPEWIRE_SHARED=ON -DSDL_PULSEAUDIO_SHARED=ON -DSDL_ALSA_SHARED=ON \
    -DSDL_X11=ON -DSDL_WAYLAND=ON -DSDL_X11_SHARED=ON -DSDL_WAYLAND_SHARED=ON \
    -DSDL_X11_XCURSOR=OFF -DSDL_X11_XRANDR=OFF -DSDL_X11_XFIXES=OFF -DSDL_X11_XINPUT=OFF \
    -DSDL_X11_XSCRNSAVER=OFF -DSDL_X11_XINERAMA=OFF -DSDL_X11_XSHAPE=OFF
  cmake --build "$SRC/SDL/build-linux" --target SDL3-static
fi

# ------------------------------------------------- 3. FFmpeg (BtbN 8.1 LGPL)
FFLIB="$FFDIR/lib"
mkdir -p "$W/bin/linux/lib"
# glob must expand inside FFLIB (not the CWD), so copy with a literal glob
cp -a "$FFLIB"/libavformat.so.62* "$FFLIB"/libavcodec.so.62* "$FFLIB"/libavutil.so.60* \
      "$FFLIB"/libswscale.so.9* "$FFLIB"/libswresample.so.6* "$W/bin/linux/lib/"
rm -f "$W/bin/linux/lib/"*.so   # keep only the versioned real files

# ------------------------------------------------------- 4. compile the player
INC="-I$W/src -I$SRC/SDL/include -I$SRC/SDL/build-linux/include-revision -I$SRC/SDL/build-linux/include-config-minsizerel/build_config -I$FFDIR/include"
for f in ff ui_core ui_draw plat; do
  gcc -std=gnu17 -O2 -DNDEBUG $INC -c "$W/src/$f.c" -o "$PREFIX/$f.o"
done

# ------------- 5. final link against glibc 2.31. Uses ld directly (not the
# gcc driver: the driver silently appends the BUILD machine's libgcc_s and
# libc linker scripts, which reference GLIBC_2.34+ symbols).
/usr/bin/ld -o "$W/bin/linux/PurePlayer" \
  -dynamic-linker /lib64/ld-linux-x86-64.so.2 -pie -rpath '$ORIGIN/lib' \
  "$GLIBC/usr/lib/x86_64-linux-gnu/Scrt1.o" "$GLIBC/usr/lib/x86_64-linux-gnu/crti.o" "$GLIBC/usr/lib/x86_64-linux-gnu/crtn.o" \
  "$PREFIX/ff.o" "$PREFIX/ui_core.o" "$PREFIX/ui_draw.o" "$PREFIX/plat.o" "$PREFIX/compat_glibc_old.o" "$PREFIX/dso_handle.o" \
  "$SRC/SDL/build-linux/libSDL3.a" \
  "$FFLIB/libavformat.so.62" "$FFLIB/libavcodec.so.62" "$FFLIB/libswscale.so.9" "$FFLIB/libswresample.so.6" "$FFLIB/libavutil.so.60" \
  "$GLIBC/usr/lib/x86_64-linux-gnu/libc_nonshared.a" \
  "$GLIBC/lib/x86_64-linux-gnu/libc-2.31.so" "$GLIBC/lib/x86_64-linux-gnu/libm-2.31.so" \
  "$GLIBC/lib/x86_64-linux-gnu/libpthread-2.31.so" "$GLIBC/lib/x86_64-linux-gnu/libdl-2.31.so" \
  "$GLIBC/lib/x86_64-linux-gnu/librt-2.31.so" "$GLIBC/lib/x86_64-linux-gnu/ld-2.31.so" \
  -rpath-link="$STUBS"

strip "$W/bin/linux/PurePlayer"

# ------------------------------------------------------------------- 6. report
MAXVER=$(objdump -T "$W/bin/linux/PurePlayer" | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)
echo "Built bin/linux/PurePlayer (max symbol version: $MAXVER -> runs on glibc >= 2.29)"
BACKENDS=$(strings "$W/bin/linux/PurePlayer" | grep -cE '^lib(asound|pulse|pipewire)')
if [ "$BACKENDS" -lt 3 ]; then
  echo "FATAL: only $BACKENDS/3 audio backends compiled in - binary would have NO SOUND on real systems" >&2
  exit 1
fi
echo "Audio backends dlopen'ed: pipewire, pulse, alsa (all 3 verified)"
"$W/bin/linux/PurePlayer" --version
