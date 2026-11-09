#!/bin/sh
# Builds "FireWireRecorder Legacy.app" and fwdv-capture for old Macs: one
# Mach-O with a PowerPC slice (Mac OS X 10.3 and later) and an i386 slice
# (10.4 and later), with a matching ffmpeg. The regular app needs 10.9.
#
# It runs in the powerpc-apple-darwin8 cross toolchain
#   ghcr.io/variantxyz/gcc-powerpc-apple-darwin8:build-gcc-14.2-MacOSXSDK10.4u
# (GCC 14 for PowerPC, cctools' ld64 for both CPUs, the 10.4u SDK), with clang
# for the i386 slice, nasm and pkg-config added:
#   scripts/build-legacy-mac.sh            everything into build/legacy/out
#   BUNDLE_FFMPEG=0 scripts/build-legacy-mac.sh   without ffmpeg
set -e
cd "$(dirname "$0")/.."
TOP=$PWD
B=$TOP/build/legacy
OUT=$B/out
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
T=powerpc-apple-darwin8
SDKS=${LEGACY_SDKS:-$B/sdk}
SDK=$SDKS/MacOSX10.4u.sdk
SDK103=$SDKS/MacOSX10.3.9.sdk
mkdir -p "$B" "$OUT" "$SDKS"

# ---- SDKs: the image's 10.4u SDK has its libraries cut down to PowerPC and a crt1.o rebuilt for
# today's linker; make a copy with the i386 slices back, and fetch the 10.3.9 SDK to check that the
# PowerPC build only uses what 10.3 has. ----
if [ ! -f "$SDK/.ready" ]; then
  rm -rf "$SDK"
  cp -a /usr/local/MacOSX10.4u.sdk "$SDK"
  (cd "$SDK/usr/lib" && for f in $(find . -name '*.universal'); do mv "$f" "${f%.universal}"; done)
  $T-lipo -thin i386 "$SDK/usr/lib/crt1.o" -output "$B/crt1-i386.o"
  $T-lipo -create "$B/crt1-i386.o" /usr/local/MacOSX10.4u.sdk/usr/lib/crt1.o -output "$SDK/usr/lib/crt1.o"
  touch "$SDK/.ready"
fi
if [ ! -d "$SDK103" ]; then
  curl -fsSL https://github.com/phracker/MacOSX-SDKs/releases/download/11.3/MacOSX10.3.9.sdk.tar.xz | tar -C "$SDKS" -xJ
fi

INC=$B/include
mkdir -p "$INC"

# the 10.4u assert.h predates C11's static_assert, which ffmpeg needs
mkdir -p "$B/ccinc"
printf '%s\n' '#include_next <assert.h>' '#if !defined(static_assert) && !defined(__cplusplus)' \
  '#define static_assert _Static_assert' '#endif' > "$B/ccinc/assert.h"

LD=$(command -v $T-ld)
cc_for() {
  case $1 in
  ppc)  echo "$T-gcc -isysroot $SDK -mmacosx-version-min=10.3 -static-libgcc -I$B/ccinc" ;;
  i386) echo "clang -target i386-apple-darwin8 -isysroot $SDK -mmacosx-version-min=10.4 -fuse-ld=$LD -Wno-unused-command-line-argument -I$B/ccinc" ;;
  esac
}
# runtime helpers the compiler calls: for PowerPC the few 10.3 lacks (scripts/legacy-ppc-runtime.c), for
# i386 (64-bit division and the like) Apple's GCC 4.0 libgcc from the SDK, as clang has none this old
libs_for() {
  case $1 in
  ppc)  echo "$B/ppc-runtime.o" ;;
  i386) echo "$SDK/usr/lib/gcc/i686-apple-darwin9/4.0.1/libgcc.a" ;;
  esac
}
# what 10.3 lacks for GCC 14's PowerPC code (see the file)
$T-gcc -isysroot "$SDK" -mmacosx-version-min=10.3 -O2 -c scripts/legacy-ppc-runtime.c -o "$B/ppc-runtime.o"

SRC="src/fwdv_mac.c src/fwdv_dv.c src/app_core.c src/decoder.c src/sink.c src/audio_mac_legacy.c"
FRAMEWORKS="-framework IOKit -framework CoreFoundation -framework CoreAudio -framework AudioUnit -framework CoreServices"

for arch in ppc i386; do
  CC=$(cc_for $arch)
  O=$B/$arch
  mkdir -p "$O"
  echo "== $arch: FireWireRecorder"
  CFLAGS="-O2 -std=gnu11 -Wall -Wno-unknown-pragmas -DLEGACY_MAC=1 -I$INC -Isrc"
  objs=
  for f in $SRC; do
    o=$O/$(basename "$f" .c).o
    $CC $CFLAGS -c "$f" -o "$o"
    objs="$objs $o"
  done
  $CC -O2 -Wall -DLEGACY_MAC=1 -I$INC -Isrc -c src/viewer_legacy.m -o "$O/viewer_legacy.o"
  $CC "$O/viewer_legacy.o" $objs $FRAMEWORKS -framework Cocoa -framework OpenGL $(libs_for $arch) \
      -o "$O/FireWireRecorder"
  $CC $CFLAGS src/capture.c "$O/fwdv_mac.o" "$O/fwdv_dv.o" $FRAMEWORKS $(libs_for $arch) -o "$O/fwdv-capture"
done

for p in FireWireRecorder fwdv-capture; do
  $T-lipo -create "$B/ppc/$p" "$B/i386/$p" -output "$B/$p"
done

# ---- the PowerPC slice must only use what Mac OS X 10.3.9 has ----
check_103() {
  undef=$($T-nm -arch ppc -u "$1" | sed 's/^ *//' | sort -u)
  have=$B/have-103.txt
  if [ ! -f "$have" ]; then
    # every library and framework binary in the 10.3.9 SDK (stubs; nm skips what is not Mach-O)
    find "$SDK103/usr/lib" "$SDK103/System/Library/Frameworks" -type f \( -name '*.dylib' -o ! -name '*.*' \) |
      while read -r f; do $T-nm -arch ppc -gU "$f" 2>/dev/null; done | awk 'NF >= 3 {print $NF}' | sort -u > "$have"
  fi
  missing=$(echo "$undef" | comm -23 - "$have" | grep -v '^\.objc_class_name_' || true)
  # ObjC classes are looked up by name; check those too
  for c in $(echo "$undef" | sed -n 's/^\.objc_class_name_//p'); do
    grep -qx ".objc_class_name_$c" "$have" || missing="$missing .objc_class_name_$c"
  done
  # and every library it loads must be there
  for d in $($T-otool -arch ppc -L "$1" | sed -n 's/^[[:space:]]*\(\/[^ ]*\) (.*/\1/p'); do
    [ -e "$SDK103$d" ] || missing="$missing $d"
  done
  if [ -n "$(echo $missing)" ]; then
    echo "error: $1 (ppc) uses what Mac OS X 10.3.9 does not have:" $missing >&2
    exit 1
  fi
}
check_103 "$B/FireWireRecorder"
check_103 "$B/fwdv-capture"
for p in FireWireRecorder fwdv-capture; do
  if $T-otool -arch ppc -L "$B/$p" | grep -q libgcc_s; then echo "error: $p needs libgcc_s" >&2; exit 1; fi
done

# ---- ffmpeg, for the preview, the sound and MPEG-2 recording ----
FF=$B/ffmpeg
if [ "${BUNDLE_FFMPEG:-1}" = 1 ]; then
  for arch in ppc i386; do
    if [ ! -f "$B/ffmpeg-$arch" ]; then
      env LEGACY_CC_$arch="$(cc_for $arch)" LEGACY_LIBS="$(libs_for $arch)" LEGACY_AR=$T-ar LEGACY_RANLIB=$T-ranlib LEGACY_NM=$T-nm \
          LEGACY_STRIP=$T-strip scripts/build-ffmpeg.sh legacy-$arch "$B/ffmpeg-$arch"
    fi
  done
  $T-lipo -create "$B/ffmpeg-ppc" "$B/ffmpeg-i386" -output "$FF"
  check_103 "$FF"
fi

# ---- the app ----
APP="$OUT/FireWireRecorder Legacy.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$B/FireWireRecorder" "$APP/Contents/MacOS/FireWireRecorder"
sed -e 's/@MACOSX_MIN@/10.3/' -e "s/@VERSION@/${VERSION:-$(sed -n 's/^VERSION *?= *//p' Makefile)}/" -e 's|<key>NSHighResolutionCapable</key>|<key>LSMinimumSystemVersionByArchitecture</key><dict><key>i386</key><string>10.4.0</string><key>ppc</key><string>10.3.0</string></dict><key>NSHighResolutionCapable</key>|' \
  resources/Info.plist > "$APP/Contents/Info.plist"
printf 'APPL????' > "$APP/Contents/PkgInfo"
cp resources/AppIcon-legacy.icns "$APP/Contents/Resources/AppIcon.icns" 2>/dev/null ||
  cp resources/AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"
if [ -f "$FF" ]; then
  cp "$FF" "$APP/Contents/MacOS/ffmpeg"
  mkdir -p "$APP/Contents/Resources/licenses"
  cp resources/licenses/* "$APP/Contents/Resources/licenses/"
  cp "$FF" "$OUT/ffmpeg"
fi
cp "$B/fwdv-capture" "$OUT/fwdv-capture"
echo "built $APP"
