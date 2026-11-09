CC      ?= clang
PREFIX  ?= /usr/local

# Universal 2 (Intel + Apple silicon). 10.9 is the oldest macOS current
# toolchains can still target (libarclite is gone from the SDK, so the ARC file is
# compiled with -fobjc-arc but linked without it; libobjc has the ARC calls since 10.7);
# the arm64 slice is always 11.0 or later. Mac OS X 10.3-10.8 on PowerPC and Intel is
# scripts/build-legacy-mac.sh.
VERSION      ?= 0.1.0
ARCHS        ?= x86_64 arm64
MACOSX_MIN   ?= 10.9
ARCHFLAGS     = $(foreach a,$(ARCHS),-arch $(a)) -mmacosx-version-min=$(MACOSX_MIN)

# vendor/apple-firewire: the classic isochronous (DCL) definitions current SDKs dropped.
# -fno-modules: otherwise the SDK's module maps link the Swift overlays (@rpath/libswift*),
# which 10.9 does not have.
CFLAGS  += -O2 -Wall -Wextra -Wno-ignored-qualifiers -std=gnu11 -Isrc -Ivendor/apple-firewire -fno-modules \
           -Wno-deprecated-declarations $(ARCHFLAGS)
SYSLIBS  = -framework IOKit -framework CoreFoundation

BUILD   = build
APP     = $(BUILD)/FireWireRecorder.app
# ffmpeg (scripts/build-ffmpeg.sh) goes into the app for the preview, the sound and the
# MPEG-2/x264 recordings. BUNDLE_FFMPEG=0 skips it (then ffmpeg comes from PATH).
BUNDLE_FFMPEG ?= 1
FFMPEG   = $(BUILD)/ffmpeg
DEV      = $(BUILD)/fwdv_mac.o $(BUILD)/fwdv_dv.o
ENGINE   = $(DEV) $(BUILD)/app_core.o $(BUILD)/decoder.o $(BUILD)/sink.o

all: $(BUILD)/fwdv-capture app

$(FFMPEG): scripts/build-ffmpeg.sh
	for a in $(ARCHS); do scripts/build-ffmpeg.sh macos-$$a $(BUILD)/ffmpeg-build/out/ffmpeg-$$a || exit 1; done
	lipo -create $(foreach a,$(ARCHS),$(BUILD)/ffmpeg-build/out/ffmpeg-$(a)) -output $@
	codesign --force --sign - $@

ffmpeg: $(FFMPEG)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c src/*.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/fwdv-capture: src/capture.c $(DEV)
	$(CC) $(CFLAGS) $^ $(SYSLIBS) -o $@

$(BUILD)/firewirerecorder-app: src/viewer.m $(ENGINE)
	$(CC) $(CFLAGS) -fobjc-arc -Wno-unused-parameter -c src/viewer.m -o $(BUILD)/viewer.o
	$(CC) $(CFLAGS) $(BUILD)/viewer.o $(filter-out src/viewer.m,$^) $(SYSLIBS) \
	    -framework Cocoa -framework AVFoundation -framework QuartzCore -framework AudioUnit \
	    -framework AudioToolbox -framework CoreAudio -o $@

app: $(BUILD)/firewirerecorder-app resources/Info.plist resources/AppIcon.icns $(if $(filter 1,$(BUNDLE_FFMPEG)),$(FFMPEG))
	mkdir -p "$(APP)/Contents/MacOS" "$(APP)/Contents/Resources"
	sed -e "s/@MACOSX_MIN@/$(MACOSX_MIN)/" -e "s/@VERSION@/$(VERSION)/" resources/Info.plist > "$(APP)/Contents/Info.plist"
	cp resources/AppIcon.icns "$(APP)/Contents/Resources/AppIcon.icns"
	cp $(BUILD)/firewirerecorder-app "$(APP)/Contents/MacOS/FireWireRecorder"
	if [ "$(BUNDLE_FFMPEG)" = 1 ]; then \
	    cp $(FFMPEG) "$(APP)/Contents/MacOS/ffmpeg" && \
	    mkdir -p "$(APP)/Contents/Resources/licenses" && cp resources/licenses/* "$(APP)/Contents/Resources/licenses/"; fi
	codesign --force --sign - "$(APP)"

# engine test without a device: plays a raw DV file through it
$(BUILD)/test_engine: tests/test_engine.c $(ENGINE)
	$(CC) $(CFLAGS) $^ $(SYSLIBS) -o $@

$(BUILD)/sample.dv:
	ffmpeg -v error -y -f lavfi -i "testsrc2=size=720x480:rate=30000/1001" \
	    -f lavfi -i "sine=frequency=440:sample_rate=48000" -t 12 -target ntsc-dv -ac 2 $@

test: $(BUILD)/test_engine $(BUILD)/sample.dv
	mkdir -p $(BUILD)/test-out
	FWDV_FFMPEG=$${FWDV_FFMPEG:-$(CURDIR)/$(FFMPEG)} FWDV_REPLAY="$${SAMPLE:-$(BUILD)/sample.dv}" \
	    $(BUILD)/test_engine $(BUILD)/test-out

install: all
	install -d $(PREFIX)/bin
	install -m 755 $(BUILD)/fwdv-capture $(PREFIX)/bin/
	cp -R "$(APP)" /Applications/

clean:
	rm -rf $(BUILD)

.PHONY: all app ffmpeg test install clean
