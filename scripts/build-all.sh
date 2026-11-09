#!/bin/sh
# Cross-builds fwdv-capture for Windows and Linux, and the FireWireRecorder GUI
# for Windows (with ffmpeg next to it). Nothing else is needed: Windows uses its
# own DV driver through DirectShow, Linux the kernel's /dev/fw* interface.
#   ZIG=/path/to/zig scripts/build-all.sh [target ...]
# Targets are Zig triples; the default is the full list below.
#
# Windows 7 needs binaries linked against msvcrt.dll (Zig links the Universal
# CRT, which Windows 7 lacks). Point these at MinGW-w64 GCC/Clang compilers
# that use msvcrt (llvm-mingw's msvcrt release or Debian's gcc-mingw-w64) to
# use them instead:
#   WIN_CC_x86_64=x86_64-w64-mingw32-clang WIN_CC_x86=i686-w64-mingw32-clang
#   WIN_CC_aarch64=aarch64-w64-mingw32-clang  (optional, for Windows on Arm)
set -e
cd "$(dirname "$0")/.."
ZIG=${ZIG:-zig}
OUT=${OUT:-dist}
CACHE=${ZIG_CACHE:-$PWD/build/zig-cache}
export ZIG_GLOBAL_CACHE_DIR=$CACHE ZIG_LOCAL_CACHE_DIR=$CACHE

TARGETS=${*:-"x86_64-windows-gnu x86-windows-gnu aarch64-windows-gnu \
x86_64-linux-gnu.2.31 x86-linux-gnu.2.31 aarch64-linux-gnu.2.31 arm-linux-gnueabihf.2.31 \
x86_64-linux-musl aarch64-linux-musl"}

COMMON="-O2 -Wall -Wextra -Wno-missing-field-initializers -std=gnu11 -Isrc"
SRC="src/fwdv_dv.c src/app_core.c src/decoder.c src/sink.c"

for t in $TARGETS; do
  name=$(echo "$t" | sed 's/\.2\.31$//')
  mkdir -p "$OUT/$name"
  CC="$ZIG cc -target $t"
  case $t in
  *windows*)
    os=windows; ext=.exe
    DEV="src/fwdv_win32.c"
    FL="-U_WIN32_WINNT -D_WIN32_WINNT=0x0601"
    OSSRC="src/audio_windows.c"
    LIBS="-lole32 -loleaut32 -lstrmiids -luuid -lwinmm"
    GUI="src/gui_win32.c"; GUILIBS="-lcomctl32 -lcomdlg32 -lshell32 -lgdi32 -luser32 -ldwmapi -Wl,--subsystem,windows"
    case $t in
    x86_64-*)  wcc=$WIN_CC_x86_64;  ff=x86_64-windows ;;
    x86-*)     wcc=$WIN_CC_x86;     ff=x86-windows ;;
    aarch64-*) wcc=$WIN_CC_aarch64; ff=aarch64-windows ;;
    esac
    # the icon: Zig compiles .rc itself, MinGW needs windres
    RES=resources/windows/app.rc
    if [ -n "$wcc" ]; then
      CC="$wcc"; LIBS="$LIBS -static -lpthread"
      RES=build/app-$name.o
      mkdir -p build
      "$(echo "$wcc" | sed -E 's/-(clang|gcc).*$//')-windres" -I resources/windows -O coff \
          resources/windows/app.rc -o "$RES"
    else
      echo "warning: $name built with Zig links the Universal CRT and will not start on a stock Windows 7" >&2
    fi
    ;;
  *linux*)
    os=linux; ext=; RES=; GUI=
    DEV="src/fwdv_linux.c"
    FL=
    LIBS="-lpthread"
    GUILIBS=
    ;;
  *) echo "unsupported target $t" >&2; exit 1 ;;
  esac
  echo "== $name ($CC)"
  $CC $COMMON $FL $DEV src/fwdv_dv.c src/capture.c $RES $LIBS -o "$OUT/$name/fwdv-capture$ext"
  if [ -n "$GUI" ]; then
    $CC $COMMON $FL $DEV $SRC $OSSRC $GUI $RES $GUILIBS $LIBS -o "$OUT/$name/FireWireRecorder$ext"
    if [ "${BUNDLE_FFMPEG:-1}" = 1 ]; then
      ffbin=build/ffmpeg-build/out/$ff/ffmpeg$ext
      [ -f "$ffbin" ] || scripts/build-ffmpeg.sh "$ff" "$ffbin"
      cp "$ffbin" "$OUT/$name/ffmpeg$ext"
      mkdir -p "$OUT/$name/licenses"
      cp resources/licenses/* "$OUT/$name/licenses/"
    fi
  fi
  rm -f "$OUT/$name"/*.pdb
done
