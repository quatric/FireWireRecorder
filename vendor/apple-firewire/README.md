# Apple FireWire headers

`IOFireWireFamilyCommon.h`, `IOFireWireLib.h`, `IOFireWireLibIsoch.h` and
`IOFireWireAVCLib.h` from the macOS 10.13 SDK (IOFireWireFamily and
IOFireWireAVC), under the Apple Public Source License 1.1 as stated in each
file. Current SDKs no longer include the classic DCL definitions that
`src/fwdv_mac.c` uses to receive isochronous data, so the Mac build puts this
folder ahead of the SDK's own headers. The legacy (PowerPC/i386) build uses
its 10.4u SDK's headers instead.
