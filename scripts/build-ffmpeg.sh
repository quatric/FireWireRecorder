#!/bin/sh
# Builds the small, static ffmpeg that is bundled with FireWireRecorder.
# It holds only what the app uses (src/app_core.c): DV in through a pipe, DV
# video and PCM decoding, yadif and scale, raw video frames or 16-bit PCM out
# through a pipe for the preview, and the recordings' MPEG-2 + MP2 in a DVD
# program stream and H.264 (x264) + AAC in MP4.
# With x264 linked in, this ffmpeg is GPL (version 2 or later); X264=0 leaves
# x264 out and keeps it LGPL.
#
#   scripts/build-ffmpeg.sh TARGET OUTFILE
#
# TARGET is one of
#   macos-x86_64 macos-arm64                    (Xcode clang; lipo them together)
#   x86_64-windows x86-windows aarch64-windows  ($WIN_PREFIX_<arch>-clang/gcc, e.g. llvm-mingw msvcrt)
#   x86_64-linux x86-linux aarch64-linux arm-linux   (zig cc, static musl)
#   legacy-ppc legacy-i386                      (Mac OS X 10.3/10.4, from scripts/build-legacy-mac.sh)
# nasm is needed for the x86 targets.
set -e
cd "$(dirname "$0")/.."
TARGET=$1
OUTFILE=$2
[ -n "$TARGET" ] && [ -n "$OUTFILE" ] || { echo "usage: $0 TARGET OUTFILE" >&2; exit 1; }
case $OUTFILE in /*) ;; *) OUTFILE=$PWD/$OUTFILE ;; esac

VER=7.1.5
SHA256=de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f
X264_COMMIT=b35605ace3ddf7c1a5d67a2eb553f034aef41d55   # x264 "stable" branch
X264=${X264:-1}
TOP=$PWD/build/ffmpeg-build
SRC=$TOP/ffmpeg-$VER
BLD=$TOP/build-$TARGET
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
ZIG=${ZIG:-zig}

mkdir -p "$TOP"
if [ ! -f "$SRC/configure" ]; then
  curl -fsSL -o "$TOP/ffmpeg-$VER.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz"
  echo "$SHA256  $TOP/ffmpeg-$VER.tar.xz" | (sha256sum -c - 2>/dev/null || shasum -a 256 -c -) >&2
  tar -C "$TOP" -xJf "$TOP/ffmpeg-$VER.tar.xz"
fi

FEATURES="--disable-everything --disable-autodetect --disable-doc --disable-debug --disable-network
  --disable-ffplay --disable-ffprobe --disable-avdevice --enable-static --disable-shared
  --enable-swscale --enable-swresample
  --enable-protocol=file,pipe
  --enable-demuxer=dv,mpegts
  --enable-parser=mpegvideo,mpegaudio
  --enable-decoder=dvvideo,pcm_s16le,mpeg2video,mp2,mp2float
  --enable-encoder=rawvideo,pcm_s16le,pcm_s16be,mpeg2video,mp2,aac
  --enable-muxer=rawvideo,pcm_s16le,pcm_s16be,mpeg2dvd,mp4
  --enable-filter=yadif,scale,format,null,anull,aformat,aresample"

# x264 for each target: X264_HOST is its --host, X264_ENV its compiler settings
X264_FLAGS=
case $TARGET in
macos-*)
  arch=${TARGET#macos-}
  min=10.9; [ "$arch" = arm64 ] && min=11.0
  CROSS="--target-os=darwin --arch=$arch --enable-cross-compile
    --cc=clang --extra-cflags=-arch\ $arch\ -mmacosx-version-min=$min
    --extra-ldflags=-arch\ $arch\ -mmacosx-version-min=$min"
  X264_HOST=$( [ "$arch" = arm64 ] && echo aarch64 || echo "$arch")-apple-darwin
  X264_ENV="CC=clang"
  X264_FLAGS="--extra-cflags=-arch\ $arch\ -mmacosx-version-min=$min --extra-ldflags=-arch\ $arch\ -mmacosx-version-min=$min --extra-asflags=-arch\ $arch"
  [ "$arch" = x86_64 ] && X264_FLAGS="--extra-cflags=-arch\ $arch\ -mmacosx-version-min=$min --extra-ldflags=-arch\ $arch\ -mmacosx-version-min=$min"
  ;;
legacy-*)
  # Mac OS X 10.3 (PowerPC) and 10.4 (Intel), built in the cross toolchain of
  # scripts/build-legacy-mac.sh, which sets LEGACY_CC_<arch> and LEGACY_SDK.
  arch=${TARGET#legacy-}
  eval lcc=\$LEGACY_CC_$arch
  [ -n "$lcc" ] || { echo "LEGACY_CC_$arch is not set" >&2; exit 1; }
  lprefix=$(dirname "$(command -v "${lcc%% *}")")/
  case $arch in
  ppc) farch=ppc; min=10.3; X264=0 ;;   # x264 cannot keep up on a G4/G5; DV and MPEG-2 instead
  i386) farch=x86_32; min=10.4 ;;
  esac
  mkdir -p "$BLD"
  printf '#!/bin/sh\nexec %s "$@"\n' "$lcc" > "$BLD/legacy-cc"
  chmod +x "$BLD/legacy-cc"
  CROSS="--target-os=darwin --arch=$farch --enable-cross-compile --cc=$BLD/legacy-cc
    --ar=${LEGACY_AR:-ar} --ranlib=${LEGACY_RANLIB:-ranlib} --nm=${LEGACY_NM:-nm} --strip=${LEGACY_STRIP:-strip}
    --extra-cflags=-mmacosx-version-min=$min --extra-ldflags=-mmacosx-version-min=$min"
  # configure finds CoreVideo and GCC's libatomic and links them although nothing here uses them;
  # Mac OS X 10.3 has neither (the atomics come from LEGACY_LIBS)
  CROSS="$CROSS --extra-ldflags=-Wl,-dead_strip_dylibs"
  [ -n "$LEGACY_LIBS" ] && CROSS="$CROSS --extra-libs=$LEGACY_LIBS"
  [ $arch = ppc ] && CROSS="$CROSS --cpu=g3 --enable-altivec --enable-runtime-cpudetect"
  [ $arch = i386 ] && CROSS="$CROSS --cpu=pentium-m --disable-inline-asm"
  X264_HOST=i686-apple-darwin
  X264_ENV="CC=$BLD/legacy-cc AR=${LEGACY_AR:-ar} RANLIB=${LEGACY_RANLIB:-ranlib} STRIP=${LEGACY_STRIP:-strip}"
  X264_FLAGS="--extra-cflags=-mmacosx-version-min=$min --extra-ldflags=-mmacosx-version-min=$min"
  ;;
*-windows)
  arch=${TARGET%-windows}
  case $arch in x86) tp=i686 ;; *) tp=$arch ;; esac
  eval prefix=\${WIN_PREFIX_$arch:-$tp-w64-mingw32}
  cc=gcc; command -v "$prefix-clang" >/dev/null 2>&1 && cc=clang
  CROSS="--target-os=mingw32 --arch=$arch --enable-cross-compile --cross-prefix=$prefix-
    --cc=$prefix-$cc --extra-cflags=-D_WIN32_WINNT=0x0601 --extra-ldflags=-static"
  case $arch in x86) X264_HOST=i686-w64-mingw32 ;; *) X264_HOST=$arch-w64-mingw32 ;; esac
  X264_ENV="CC=$prefix-$cc AR=$prefix-ar RANLIB=$prefix-ranlib STRIP=$prefix-strip"
  X264_FLAGS="--extra-cflags=-D_WIN32_WINNT=0x0601"
  ;;
*-linux)
  arch=${TARGET%-linux}
  case $arch in arm) zt=arm-linux-musleabihf; fa=arm ;; x86) zt=x86-linux-musl; fa=x86 ;; *) zt=$arch-linux-musl; fa=$arch ;; esac
  mkdir -p "$BLD"
  # configure wants single-word tools
  for t in cc ar ranlib; do
    printf '#!/bin/sh\nexec %s %s %s "$@"\n' "$ZIG" "$t" "$( [ $t = cc ] && echo "-target $zt")" > "$BLD/zig-$t"
    chmod +x "$BLD/zig-$t"
  done
  CROSS="--target-os=linux --arch=$fa --enable-cross-compile --cc=$BLD/zig-cc --ar=$BLD/zig-ar
    --ranlib=$BLD/zig-ranlib --nm=${NM:-llvm-nm} --strip=${STRIP:-llvm-strip} --extra-ldflags=-static"
  # baseline armhf like the rest of the build (ARMv6 boards too), so no NEON
  [ $arch = arm ] && CROSS="$CROSS --disable-neon"
  case $arch in arm) X264_HOST=arm-linux-musleabihf ;; x86) X264_HOST=i686-linux-musl ;; *) X264_HOST=$arch-linux-musl ;; esac
  X264_ENV="CC=$BLD/zig-cc AR=$BLD/zig-ar RANLIB=$BLD/zig-ranlib STRIP=${STRIP:-llvm-strip}"
  # x264's ARM assembly needs NEON
  [ $arch = arm ] && X264_FLAGS="--disable-asm"
  ;;
*) echo "unknown target $TARGET" >&2; exit 1 ;;
esac

if [ "$X264" = 1 ]; then
  X264_SRC=$TOP/x264-$X264_COMMIT
  if [ ! -f "$X264_SRC/configure" ]; then
    rm -rf "$X264_SRC.tmp" && mkdir -p "$X264_SRC.tmp"
    git -C "$X264_SRC.tmp" init -q
    git -C "$X264_SRC.tmp" fetch -q --depth 1 https://code.videolan.org/videolan/x264.git "$X264_COMMIT"
    git -C "$X264_SRC.tmp" checkout -q FETCH_HEAD
    mv "$X264_SRC.tmp" "$X264_SRC"
  fi
  XP=$BLD/x264-prefix
  rm -rf "$BLD/x264" "$XP"; mkdir -p "$BLD/x264"
  # x264 builds in its source tree, so build a copy
  (cd "$X264_SRC" && tar cf - --exclude=.git .) | (cd "$BLD/x264" && tar xf -)
  (cd "$BLD/x264" && eval "env $X264_ENV ./configure --host=$X264_HOST --prefix=\"$XP\" --enable-static --disable-cli \
      --disable-opencl --disable-lavf --disable-swscale --disable-ffms --disable-gpac --disable-lsmash \
      --bit-depth=8 --chroma-format=420 $X264_FLAGS" > configure.log 2>&1 &&
    make -j"$JOBS" > make.log 2>&1 && make install-lib-static > install.log 2>&1) ||
    { tail -30 "$BLD/x264/configure.log" "$BLD/x264/config.log" "$BLD/x264/make.log" 2>/dev/null; exit 1; }
  FEATURES="$FEATURES --enable-gpl --enable-libx264 --enable-encoder=libx264
    --extra-cflags=-I$XP/include --extra-ldflags=-L$XP/lib --pkg-config=pkg-config --pkg-config-flags=--static"
  export PKG_CONFIG_PATH=$XP/lib/pkgconfig PKG_CONFIG_LIBDIR=$XP/lib/pkgconfig
fi

rm -rf "$BLD/obj"; mkdir -p "$BLD/obj"
cd "$BLD/obj"
# shellcheck disable=SC2086
ARGS=$(echo $FEATURES $CROSS)   # one line; "\ " keeps flags with spaces together
eval "\"$SRC/configure\" $ARGS" > configure.log 2>&1 || { tail -40 configure.log; tail -60 ffbuild/config.log; exit 1; }
EXT=; case $TARGET in *-windows) EXT=.exe ;; esac
make -j"$JOBS" "ffmpeg$EXT" > make.log 2>&1 || { tail -40 make.log; exit 1; }
mkdir -p "$(dirname "$OUTFILE")"
cp "ffmpeg$EXT" "$OUTFILE"
case $TARGET in macos-*)
  # same SDK quirk as the app (see Makefile): CoreMedia recorded as @rpath/libswiftCoreMedia.dylib
  if otool -L "$OUTFILE" | grep -q '@rpath/libswiftCoreMedia.dylib'; then
    install_name_tool -change @rpath/libswiftCoreMedia.dylib \
      /System/Library/Frameworks/CoreMedia.framework/Versions/A/CoreMedia "$OUTFILE" 2>/dev/null
  fi
  codesign --force --sign - "$OUTFILE" ;;
esac
echo "built $OUTFILE"
