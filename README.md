# FireWireRecorder

Preview, tape control and recording for **DV camcorders and decks on FireWire** (IEEE 1394, i.LINK)
on macOS, Windows and Linux. Plug in a MiniDV or Digital8 camcorder in VCR/playback mode and
FireWireRecorder shows the picture, runs the tape (rewind, play, pause, stop, fast forward) and
records what comes over the cable:

- **Raw DV** (`.dv`): the frames exactly as they are on the tape, bit for bit (**HDV** tapes: the MPEG-2
  transport stream exactly as the camcorder sends it, `.m2t`)
- **MPEG-2** (`.mpg`): DVD-compatible, interlaced, 8 Mbit/s with MP2 audio
- **x264** (`.mp4`): deinterlaced H.264 with AAC audio, small and plays anywhere

The tape's timecode and state (playing, paused, rewinding, no tape) are shown next to the
transport buttons. "Stop at end of tape" ends the recording by itself once the tape stops or the
picture goes away. The preview can be switched off (View → Show Preview, or the Preview checkbox)
to save CPU on slow machines while recording.
"Stop after" ends a recording by itself after a time you choose (default: no limit): pick 5 minutes up to 12 hours (4, 6, 8, 10 and 12 hours match VHS tape lengths), or type your own (`45`, `90 min`, `2h`, `1:30`).

**HDV** camcorders work too (macOS and Linux): set the camcorder's i.LINK output to HDV (not "DV Conv.")
and the preview, the sound and all three recording formats follow; MPEG-2 and x264 are made from the HDV
picture (DVD size and full 1080 resolution respectively). On Windows, HDV is not supported yet. HDV is
untested on a device.

**Record to tape** (macOS, experimental): File → Record File to Tape… (⌘T) sends any video file ffmpeg can read
back to the camcorder, which records it from the tape's current position, over what is there; the camcorder
must be in VCR mode with a tape that is not write-protected. The video is converted to DV and letterboxed
to 4:3, in the standard (NTSC or PAL) of the last tape played. On the command line,
`fwdv-capture --send file.dv` records a raw DV file the same way. `-vv` logs what happens on the bus; `FWDV_SYT=none` sends the packets without time stamps, which some camcorders prefer. This has not been tried on a camcorder yet.

Status: tested on macOS 10.9.5 (Intel) with a Sony DCR-HC21 (NTSC): live preview, tape control,
timecode, and recording in all three formats; the PowerPC/i386 legacy build's capture was tested on
the same Mac. **The Windows and Linux builds have not been tested on a device yet.** PAL (625/50)
is handled but untested.

## Platforms

| | |
|---|---|
| macOS 10.9 and later | `FireWireRecorder.app`, Universal 2 (Intel + Apple silicon) |
| Mac OS X 10.3–10.8 | `FireWireRecorder Legacy.app`, PowerPC + i386 (x264 on i386 only) |
| Windows 7 and later | x86_64, x86, ARM64: app + `fwdv-capture`, zip or setup program |
| Linux | `fwdv-capture` command line recorder (x86_64, x86, aarch64, armhf; glibc 2.31+ or static musl) |

macOS 26 no longer supports FireWire at all; use macOS 15 or earlier (a Thunderbolt to FireWire
adapter works on Macs without a FireWire port).

On **Windows**, the camcorder uses Windows' own DV driver (MSDV, "Microsoft DV Camera and VCR"),
which Windows installs by itself when the camcorder is plugged in; FireWireRecorder talks to it
through DirectShow. Windows on Arm has no FireWire driver, so that build is only useful where one
exists.

On **Linux**, `fwdv-capture` uses the kernel's FireWire stack (`firewire-ohci`, `/dev/fw*`). The
device nodes are usually only accessible to root or the `video` group:

```bash
sudo usermod -aG video $USER    # then log in again
```

## Use

The app needs no setup: start it with the camcorder connected and switched to VCR/PLAY. Keyboard
shortcuts (⌘ on macOS, Ctrl on Windows): Return play, P pause, . stop, ← rewind, → fast forward,
R record, S snapshot, E preview on/off.

```bash
fwdv-capture --info                           # device, tape state and timecode
fwdv-capture -o tape.dv --play --until-stop   # play the tape and record it to the end
fwdv-capture -o clip.dv -t 60                 # record 60 seconds of whatever is playing
fwdv-capture -o - | ffplay -f dv -            # watch live
fwdv-capture --rewind                         # tape commands on their own
```

| Option | |
|---|---|
| `-o FILE` | record raw DV to FILE (`-` for standard output) |
| `-t SEC` | stop after SEC seconds (counted from the first frame) |
| `--play` | start the tape first, and stop it again at the end |
| `--until-stop` | record until the tape stops (end of tape or recording) |
| `--stop`, `--pause`, `--rewind`, `--ffwd` | tape commands |
| `--send FILE` | record a raw DV file onto the tape (macOS) |
| `--info` | print the device, tape state and timecode |
| `-v`, `-vv` | log what happens on the bus |

Raw `.dv` files open in iMovie, Final Cut, Premiere, VLC and ffmpeg, and can be converted later
without losing anything.

## Build

```bash
make                     # macOS: build/fwdv-capture and build/FireWireRecorder.app
make test                # engine test without a device (plays a generated DV file through it)
scripts/build-all.sh     # Windows and Linux, with Zig: dist/<target>/
```

`make` builds ffmpeg (with x264) for the app with `scripts/build-ffmpeg.sh`; `BUNDLE_FFMPEG=0`
uses the ffmpeg on PATH instead. Current macOS SDKs no longer have the classic FireWire
isochronous headers, so `vendor/apple-firewire` carries them from Apple's 10.13 SDK (APSL 1.1).

`scripts/build-legacy-mac.sh` builds the PowerPC and i386 app for old Macs in the
`powerpc-apple-darwin8` cross toolchain
(`ghcr.io/variantxyz/gcc-powerpc-apple-darwin8:build-gcc-14.2-MacOSXSDK10.4u`, with clang, nasm and
pkg-config added), as the release workflow does.

Zig links the Universal CRT, which a stock Windows 7 lacks. For Windows 7 builds, point
`WIN_CC_x86_64`, `WIN_CC_x86` (and optionally `WIN_CC_aarch64`) at an msvcrt MinGW-w64 compiler such
as [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)'s msvcrt release. Pushing a `v*` tag runs
`.github/workflows/release.yml`, which does all of this and publishes the macOS archives (including
the legacy one), the Linux tarballs, and the Windows zips and setup programs.

## How it works

DV arrives as IEC 61883 isochronous packets (a CIP header and six 80-byte DIF blocks each) that are
put back together into 120000-byte (NTSC) or 144000-byte (PAL) frames. The tape is driven with
AV/C Tape Recorder/Player commands over FCP. On macOS this is IOFireWireLib's isochronous receive
and IOFireWireAVCLib, on Linux firewire-cdev, and on Windows the MSDV driver through DirectShow
(`IAMExtTransport`, `IAMTimecodeReader`). The preview, the sound and the MPEG-2/x264 encoding are
done by ffmpeg running beside the app.

## License

FireWireRecorder bundles FFmpeg built with x264, so the binary releases are under the GNU GPL
version 2 or later; see `resources/licenses`. FireWire and the FireWire logo are trademarks of
Apple Inc.; FireWireRecorder is not affiliated with or endorsed by Apple. The icon is made from the
public-domain FireWire logo on Wikimedia Commons (`resources/icon`).
