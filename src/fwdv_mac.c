/*
 * macOS backend (see fwdv.h): the AV/C unit through IOFireWireAVCLib for the
 * tape commands, and its FireWire device through IOFireWireLib for the
 * isochronous stream, received with a classic DCL program (a ring of
 * receive-packet commands with a callback every GROUP packets). Both APIs are
 * there from Mac OS X 10.3 to the last macOS with FireWire.
 *
 * Recent SDKs no longer carry the DCL structures, so the modern build takes
 * the FireWire headers from vendor/apple-firewire (see the Makefile); the
 * legacy build uses its 10.4u SDK's own.
 */
#include "fwdv.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/avc/IOFireWireAVCLib.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <IOKit/firewire/IOFireWireLibIsoch.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* the 10.4u SDK's DCLCallProc has a plain UInt32 procData */
#ifdef LEGACY_MAC
typedef UInt32 procdata_t;
#else
typedef DCLCallProcDataType procdata_t;
#endif

#define NPKT 1600  /* 200 ms of cycles */
#define PKTSZ 512  /* isoch header, CIP header and 480 bytes of DIF blocks fit */
#define GROUP 100  /* a callback every 12.5 ms */

struct fwdv {
    io_service_t unit;
    IOFireWireAVCLibUnitInterface **avc;
    IOCFPlugInInterface **plugin; /* kept until close: the AV/C library lives in it */
    IOFireWireLibDeviceRef dev;
    pthread_mutex_t avc_lock;
    char name[128];
    UInt64 guid;

    /* receive thread */
    pthread_t thread;
    int running, start_result;
    pthread_mutex_t lock;
    pthread_cond_t cv;
    CFRunLoopRef loop;
    int channel;
    uint8_t *bufs;
    DCLLabel label;
    DCLTransferPacket xfer[NPKT];
    DCLCallProc calls[NPKT / GROUP];
    DCLJump jump;
    IOFireWireLibLocalIsochPortRef lport;
    IOFireWireLibRemoteIsochPortRef rport;
    IOFireWireLibIsochChannelRef chan;
    dv_assembler_t as;
};

static fwdv_t *g_rx; /* the remote port's callbacks have no refcon on 10.3 */

static int avc_raw(fwdv_t *d, const uint8_t *cmd, int n, uint8_t *resp, int *rlen)
{
    UInt32 rl = (UInt32)*rlen;
    pthread_mutex_lock(&d->avc_lock);
    IOReturn e = (*d->avc)->AVCCommand(d->avc, cmd, (UInt32)n, resp, &rl);
    if (e == kIOReturnNotOpen && d->unit && !(*d->avc)->open(d->avc)) {
        /* closing the device after receiving also closes the unit's session it was opened with */
        rl = (UInt32)*rlen;
        e = (*d->avc)->AVCCommand(d->avc, cmd, (UInt32)n, resp, &rl);
    }
    pthread_mutex_unlock(&d->avc_lock);
    *rlen = (int)rl;
    if (fwdv_verbose > 1) {
        fprintf(stderr, "avc:");
        for (int i = 0; i < n; i++)
            fprintf(stderr, " %02x", cmd[i]);
        fprintf(stderr, " -> %08x:", e);
        for (UInt32 i = 0; !e && i < rl; i++)
            fprintf(stderr, " %02x", resp[i]);
        fprintf(stderr, "\n");
    }
    if (e == kIOReturnNoDevice || e == kIOReturnNotAttached || e == kIOReturnOffline)
        return FWDV_ERR_GONE;
    return e ? FWDV_ERR_IO : FWDV_OK;
}

/* the unit's own name is its kind ("DV-VCR"); the model is on the FireWire device above it */
static void reg_string(io_service_t s, CFStringRef key, char *out, size_t len)
{
    out[0] = 0;
    io_registry_entry_t dev = 0, cur = s, parent;
    IOObjectRetain(cur);
    while (!IORegistryEntryGetParentEntry(cur, kIOServicePlane, &parent)) {
        IOObjectRelease(cur);
        cur = parent;
        if (IOObjectConformsTo(cur, "IOFireWireDevice")) {
            dev = cur;
            break;
        }
    }
    if (!dev)
        IOObjectRelease(cur);
    CFTypeRef v = IORegistryEntrySearchCFProperty(dev ? dev : s, kIOServicePlane, key, kCFAllocatorDefault,
                                                  kIORegistryIterateRecursively | kIORegistryIterateParents);
    if (dev)
        IOObjectRelease(dev);
    if (v && CFGetTypeID(v) == CFStringGetTypeID())
        CFStringGetCString(v, out, (CFIndex)len, kCFStringEncodingUTF8);
    if (v)
        CFRelease(v);
}

static UInt64 reg_guid(io_service_t s)
{
    UInt64 g = 0;
    CFTypeRef v = IORegistryEntrySearchCFProperty(s, kIOServicePlane, CFSTR("GUID"), kCFAllocatorDefault,
                                                  kIORegistryIterateRecursively | kIORegistryIterateParents);
    if (v && CFGetTypeID(v) == CFNumberGetTypeID())
        CFNumberGetValue(v, kCFNumberSInt64Type, &g);
    if (v)
        CFRelease(v);
    return g;
}

/* a unit with a tape (type 4) or camera (type 7) subunit, by SUBUNIT INFO */
static int is_dv_unit(fwdv_t *d)
{
    uint8_t cmd[8] = { 0x01, 0xff, 0x31, 0x07, 0xff, 0xff, 0xff, 0xff }, r[16];
    int rl = sizeof(r);
    if (avc_raw(d, cmd, 8, r, &rl) || rl < 8 || r[0] != 0x0c)
        return 1; /* cannot tell: try it */
    for (int i = 4; i < 8; i++)
        if (r[i] != 0xff && ((r[i] >> 3) == 4 || (r[i] >> 3) == 7))
            return 1;
    return 0;
}

int fwdv_open(fwdv_t **out)
{
    *out = NULL;
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMasterPortDefault, IOServiceMatching("IOFireWireAVCUnit"), &it))
        return FWDV_ERR_NO_DEVICE;
    io_service_t s;
    int result = FWDV_ERR_NO_DEVICE;
    while ((s = IOIteratorNext(it))) {
        fwdv_t *d = calloc(1, sizeof(*d));
        IOCFPlugInInterface **pi = NULL;
        SInt32 score;
        if (!d || IOCreatePlugInInterfaceForService(s, kIOFireWireAVCLibUnitTypeID, kIOCFPlugInInterfaceID, &pi,
                                                    &score) || !pi) {
            free(d);
            IOObjectRelease(s);
            continue;
        }
        (*pi)->QueryInterface(pi, CFUUIDGetUUIDBytes(kIOFireWireAVCLibUnitInterfaceID), (LPVOID *)&d->avc);
        if (!d->avc) {
            IODestroyPlugInInterface(pi);
            free(d);
            IOObjectRelease(s);
            continue;
        }
        pthread_mutex_init(&d->avc_lock, NULL);
        pthread_mutex_init(&d->lock, NULL);
        pthread_cond_init(&d->cv, NULL);
        d->plugin = pi;
        IOReturn e = (*d->avc)->open(d->avc);
        if (e) {
            if (fwdv_verbose)
                fprintf(stderr, "fwdv: AV/C open failed: %08x\n", e);
            result = e == kIOReturnExclusiveAccess ? FWDV_ERR_BUSY : FWDV_ERR_IO;
            (*d->avc)->Release(d->avc);
            IODestroyPlugInInterface(d->plugin);
            free(d);
            IOObjectRelease(s);
            continue;
        }
        if (!is_dv_unit(d)) { /* AV/C commands need the unit open */
            (*d->avc)->close(d->avc);
            (*d->avc)->Release(d->avc);
            IODestroyPlugInInterface(d->plugin);
            free(d);
            IOObjectRelease(s);
            continue;
        }
        d->unit = s;
        d->guid = reg_guid(s);
        char vendor[64], product[64];
        reg_string(s, CFSTR("FireWire Vendor Name"), vendor, sizeof(vendor));
        reg_string(s, CFSTR("FireWire Product Name"), product, sizeof(product));
        if (vendor[0] && product[0])
            snprintf(d->name, sizeof(d->name), "%s %s", vendor, product);
        else
            snprintf(d->name, sizeof(d->name), "%s", product[0] ? product : vendor[0] ? vendor : "DV device");
        *out = d;
        result = FWDV_OK;
        break;
    }
    IOObjectRelease(it);
    return result;
}

const char *fwdv_name(fwdv_t *d) { return d->name; }

void fwdv_close(fwdv_t *d)
{
    if (!d)
        return;
    fwdv_stop(d);
    (*d->avc)->close(d->avc);
    (*d->avc)->Release(d->avc);
    IODestroyPlugInInterface(d->plugin);
    IOObjectRelease(d->unit);
    free(d);
}

int fwdv_alive(fwdv_t *d)
{
    io_iterator_t it;
    int found = 0;
    if (IOServiceGetMatchingServices(kIOMasterPortDefault, IOServiceMatching("IOFireWireAVCUnit"), &it))
        return 0;
    io_service_t s;
    while ((s = IOIteratorNext(it))) {
        if (reg_guid(s) == d->guid)
            found = 1;
        IOObjectRelease(s);
    }
    IOObjectRelease(it);
    return found;
}

/* ---- tape ---- */

int fwdv_transport(fwdv_t *d, fwdv_cmd_t cmd)
{
    uint8_t c[8], r[16];
    int n = avc_cmd_bytes(cmd, c), rl = sizeof(r);
    if (!n)
        return FWDV_ERR_UNSUPPORTED;
    int e = avc_raw(d, c, n, r, &rl);
    if (e)
        return e;
    return rl >= 1 && (r[0] == 0x09 || r[0] == 0x0f || r[0] == 0x0c) ? FWDV_OK : FWDV_ERR_UNSUPPORTED;
}

int fwdv_status(fwdv_t *d, fwdv_status_t *st)
{
    static const uint8_t ts[4] = { 0x01, 0x20, 0xd0, 0x7f };
    static const uint8_t tc[8] = { 0x01, 0x20, 0x51, 0x71, 0xff, 0xff, 0xff, 0xff };
    static const uint8_t mi[5] = { 0x01, 0x20, 0xda, 0x7f, 0x7f };
    uint8_t r[16];
    int rl = sizeof(r), e;
    memset(st, 0, sizeof(*st));
    st->has_tape = -1;
    if ((e = avc_raw(d, ts, 4, r, &rl)))
        return e;
    avc_parse_transport(r, rl, st);
    if (rl >= 1 && r[0] == 0x08) /* NOT IMPLEMENTED: no tape subunit answering, a camera */
        st->mode = FWDV_MODE_CAMERA;
    rl = sizeof(r);
    if (!avc_raw(d, mi, 5, r, &rl))
        avc_parse_medium(r, rl, st);
    rl = sizeof(r);
    if (!avc_raw(d, tc, 8, r, &rl))
        avc_parse_timecode(r, rl, st);
    return FWDV_OK;
}

/* ---- isochronous receive ---- */

static void got_group(DCLCommand *cmd)
{
    fwdv_t *d = g_rx;
    if (!d)
        return;
    long g = (long)((DCLCallProc *)cmd)->procData;
    for (int i = 0; i < GROUP; i++) {
        uint8_t *p = d->bufs + (size_t)(g * GROUP + i) * PKTSZ;
        /* the isochronous header as the controller stored it: data length in the top 16 bits */
        UInt32 hdr;
        memcpy(&hdr, p, 4);
        UInt32 len = hdr >> 16;
        if (len > PKTSZ - 4)
            len = CFSwapInt32(hdr) >> 16;
        if (len > PKTSZ - 4)
            continue;
        dv_assemble(&d->as, p + 4, len);
    }
}

static IOReturn rp_supported(IOFireWireLibIsochPortRef p, IOFWSpeed *speed, UInt64 *mask)
{
    (void)p;
    *speed = kFWSpeed100MBit;
    *mask = (UInt64)1 << (63 - (g_rx ? g_rx->channel : 63));
    return kIOReturnSuccess;
}
static IOReturn rp_ok(IOFireWireLibIsochPortRef p) { (void)p; return kIOReturnSuccess; }
static IOReturn rp_alloc(IOFireWireLibIsochPortRef p, IOFWSpeed s, UInt32 c)
{
    (void)p, (void)s, (void)c;
    return kIOReturnSuccess;
}

/* the channel the camera sends on: its output plug says, or broadcast channel 63 */
static int find_channel(fwdv_t *d)
{
    UInt32 q = 0;
    FWAddress a = { 0, 0xffff, 0xf0000904 }; /* nodeID (filled in for the device), addressHi, addressLo */
    io_object_t svc = (*d->dev)->GetDevice(d->dev);
    UInt32 gen = 0;
    (*d->dev)->GetBusGeneration(d->dev, &gen);
    IOReturn e = (*d->dev)->ReadQuadlet(d->dev, svc, &a, &q, false, gen);
    if (!e) {
        UInt32 o = CFSwapInt32BigToHost(q);
        if (fwdv_verbose)
            fprintf(stderr, "fwdv: oPCR[0] %08x\n", (unsigned)o);
        if ((o >> 30 & 1) || (o >> 24 & 0x3f)) /* a broadcast or point-to-point connection is up */
            return (int)(o >> 16 & 0x3f);
    } else if (fwdv_verbose) {
        fprintf(stderr, "fwdv: reading oPCR failed: %08x\n", e);
    }
    return 63;
}

static void teardown(fwdv_t *d)
{
    if (d->chan) {
        (*d->chan)->Stop(d->chan);
        (*d->chan)->ReleaseChannel(d->chan);
        (*d->chan)->Release(d->chan);
        d->chan = NULL;
    }
    if (d->lport) {
        (*d->lport)->Release(d->lport);
        d->lport = NULL;
    }
    if (d->rport) {
        (*d->rport)->Release(d->rport);
        d->rport = NULL;
    }
    if (d->dev) {
        (*d->dev)->RemoveIsochCallbackDispatcherFromRunLoop(d->dev);
        (*d->dev)->RemoveCallbackDispatcherFromRunLoop(d->dev);
        (*d->dev)->Close(d->dev);
        (*d->dev)->Release(d->dev);
        d->dev = NULL;
    }
}

static int setup(fwdv_t *d)
{
    d->dev = (IOFireWireLibDeviceRef)(*d->avc)->getAncestorInterface(
        d->avc, "IOFireWireDevice", CFUUIDGetUUIDBytes(kIOFireWireLibTypeID),
        CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID));
    if (!d->dev)
        return FWDV_ERR_IO;
    IOReturn e = (*d->dev)->OpenWithSessionRef(d->dev, (*d->avc)->getSessionRef(d->avc));
    if (e) {
        (*d->dev)->Release(d->dev);
        d->dev = NULL;
        return e == kIOReturnExclusiveAccess ? FWDV_ERR_BUSY : FWDV_ERR_IO;
    }
    (*d->dev)->AddCallbackDispatcherToRunLoop(d->dev, d->loop);
    (*d->dev)->AddIsochCallbackDispatcherToRunLoop(d->dev, d->loop);
    d->channel = find_channel(d);

    /* the DCL program: label, NPKT receives with a callback after every GROUP, jump back */
    DCLCommand *prev = (DCLCommand *)&d->label;
    memset(&d->label, 0, sizeof(d->label));
    d->label.opcode = kDCLLabelOp;
    for (int i = 0; i < NPKT; i++) {
        DCLTransferPacket *x = &d->xfer[i];
        memset(x, 0, sizeof(*x));
        x->opcode = kDCLReceivePacketStartOp;
        x->buffer = d->bufs + (size_t)i * PKTSZ;
        x->size = PKTSZ;
        prev->pNextDCLCommand = (DCLCommand *)x;
        prev = (DCLCommand *)x;
        if ((i + 1) % GROUP == 0) {
            DCLCallProc *c = &d->calls[i / GROUP];
            memset(c, 0, sizeof(*c));
            c->opcode = kDCLCallProcOp;
            c->proc = got_group;
            c->procData = (procdata_t)(long)(i / GROUP);
            prev->pNextDCLCommand = (DCLCommand *)c;
            prev = (DCLCommand *)c;
        }
    }
    memset(&d->jump, 0, sizeof(d->jump));
    d->jump.opcode = kDCLJumpOp;
    d->jump.pJumpDCLLabel = &d->label;
    prev->pNextDCLCommand = (DCLCommand *)&d->jump;

    IOVirtualRange br = { (IOVirtualAddress)d->bufs, NPKT * PKTSZ };
    d->lport = (*d->dev)->CreateLocalIsochPort(d->dev, false, (DCLCommand *)&d->label, 0, 0, 0, NULL, 0, &br, 1,
                                               CFUUIDGetUUIDBytes(kIOFireWireLocalIsochPortInterfaceID));
    d->rport = (*d->dev)->CreateRemoteIsochPort(d->dev, true, CFUUIDGetUUIDBytes(kIOFireWireRemoteIsochPortInterfaceID));
    d->chan = (*d->dev)->CreateIsochChannel(d->dev, false, 488, kFWSpeed100MBit,
                                            CFUUIDGetUUIDBytes(kIOFireWireIsochChannelInterfaceID));
    if (!d->lport || !d->rport || !d->chan)
        return FWDV_ERR_IO;
    (*d->rport)->SetGetSupportedHandler(d->rport, rp_supported);
    (*d->rport)->SetAllocatePortHandler(d->rport, rp_alloc);
    (*d->rport)->SetReleasePortHandler(d->rport, rp_ok);
    (*d->rport)->SetStartHandler(d->rport, rp_ok);
    (*d->rport)->SetStopHandler(d->rport, rp_ok);
    if ((e = (*d->chan)->SetTalker(d->chan, (IOFireWireLibIsochPortRef)d->rport)) ||
        (e = (*d->chan)->AddListener(d->chan, (IOFireWireLibIsochPortRef)d->lport)) ||
        (e = (*d->chan)->AllocateChannel(d->chan)) || (e = (*d->chan)->Start(d->chan))) {
        if (fwdv_verbose)
            fprintf(stderr, "fwdv: isochronous setup failed: %08x\n", e);
        return FWDV_ERR_IO;
    }
    if (fwdv_verbose)
        fprintf(stderr, "fwdv: receiving on channel %d\n", d->channel);
    return FWDV_OK;
}

static void *rx_thread(void *arg)
{
    fwdv_t *d = arg;
    d->loop = CFRunLoopGetCurrent();
    int r = setup(d);
    pthread_mutex_lock(&d->lock);
    d->start_result = r;
    d->running = r == FWDV_OK ? 1 : 0;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->lock);
    if (r == FWDV_OK) {
        /* a run loop needs a source to keep running; the isoch dispatcher is one */
        while (d->running)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, false);
    }
    teardown(d);
    return NULL;
}

int fwdv_start(fwdv_t *d, fwdv_frame_cb cb, void *user)
{
    if (d->thread)
        return FWDV_OK;
    if (!d->bufs && !(d->bufs = calloc(NPKT, PKTSZ)))
        return FWDV_ERR_IO;
    memset(&d->as, 0, sizeof(d->as));
    d->as.cb = cb;
    d->as.user = user;
    d->start_result = 1;
    g_rx = d;
    if (pthread_create(&d->thread, NULL, rx_thread, d)) {
        d->thread = 0;
        return FWDV_ERR_IO;
    }
    pthread_mutex_lock(&d->lock);
    while (d->start_result == 1)
        pthread_cond_wait(&d->cv, &d->lock);
    int r = d->start_result;
    pthread_mutex_unlock(&d->lock);
    if (r) {
        pthread_join(d->thread, NULL);
        d->thread = 0;
        g_rx = NULL;
    }
    return r;
}

void fwdv_stop(fwdv_t *d)
{
    if (!d->thread)
        return;
    d->running = 0;
    if (d->loop)
        CFRunLoopStop(d->loop);
    pthread_join(d->thread, NULL);
    d->thread = 0;
    d->loop = NULL;
    g_rx = NULL;
    free(d->bufs);
    d->bufs = NULL;
}
