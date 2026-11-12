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
#include <sys/time.h>
#include <unistd.h>

/* the 10.4u SDK's DCLCallProc has a plain UInt32 procData */
#ifdef LEGACY_MAC
typedef UInt32 procdata_t;
#else
typedef DCLCallProcDataType procdata_t;
#endif

#define NPKT 1600  /* 200 ms of cycles */
#define PKTSZ 2048 /* isoch header, CIP header and up to 2000 bytes: DV (480) or HDV (several 192-byte source packets) */
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

    /* transmit (recording onto the tape) */
#ifndef LEGACY_MAC
    pthread_t tx_thread;
    int tx_running, tx_result, tx_pal, tx_started;
    pthread_mutex_t tx_lock;
    pthread_cond_t tx_cv;
    uint8_t *tx_q;       /* TXQ frames, a ring */
    int tx_head, tx_count, tx_draining;
    uint64_t tx_queued, tx_sent;
    dv_tx_t tx;
    int tx_cur_slot;     /* the queue slot the packetizer is reading, or -1 */
    int tx_start_cycle, tx_connected, tx_synced, tx_modify_errors;
    uint64_t tx_switched_groups; /* callbacks seen: 0 means the DMA program is not running */
    UInt16 tx_sizes[NPKT];
    uint8_t tx_sel[NPKT]; /* what each cycle is wired to send now: 1 its data packet, 0 its empty one */
    /* every cycle has two send DCLs, a full data packet (A) and an empty one (E); the jumps after
     * cycle i-1's packets point at the one cycle i sends */
    DCLSetTagSyncBits tx_tag;
    DCLLabel tx_labA[NPKT], tx_labE[NPKT];
    DCLTransferPacket tx_sendE[NPKT];
    DCLJump tx_jmpA[NPKT], tx_jmpE[NPKT];
    DCLCallProc tx_callsA[NPKT / GROUP], tx_callsE[NPKT / GROUP];
#endif
};

#define TXQ 24      /* frames waiting to go out: 0.8 s */
#define TXCHAN 1    /* the isochronous channel the camcorder listens on */
#define TXDATA 488  /* a data packet: 8 CIP + 480 */
#define TXEMPTY 8   /* an empty packet: the CIP header alone */
#define TXEOFF 1024 /* where in its slot a cycle's empty packet is kept */

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
#ifndef LEGACY_MAC
    if (d->tx_connected) {
        (*d->avc)->breakP2PInputConnection(d->avc, 0);
        d->tx_connected = 0;
    }
#endif
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

/* ---- isochronous transmit: recording a file onto the camcorder's tape ---- */

#ifndef LEGACY_MAC

/* Produces the next cycle's packet into slot i of the ring. Called with tx_lock held. */
static void tx_fill(fwdv_t *d, int i)
{
    if (!d->tx.frame && d->tx_count > 0) {
        d->tx_cur_slot = d->tx_head;
        d->tx.frame = d->tx_q + (size_t)d->tx_head * DV_FRAME_625;
    }
    uint8_t *buf = d->bufs + (size_t)i * PKTSZ;
    int n = dv_tx_packet(&d->tx, buf);
    d->tx_sizes[i] = (UInt16)n;
    if (n != TXDATA)
        memcpy(buf + TXEOFF, buf, TXEMPTY); /* the empty DCL sends from here */
    if (d->tx.frame_done) { /* the frame is entirely in packets: its slot is free again */
        d->tx.frame_done = 0;
        d->tx_head = (d->tx_head + 1) % TXQ;
        d->tx_count--;
        d->tx_sent++;
        pthread_cond_broadcast(&d->tx_cv);
    }
}

static void tx_group(DCLCommand *cmd)
{
    fwdv_t *d = g_rx;
    if (!d)
        return;
    long g = (long)((DCLCallProc *)cmd)->procData;
    pthread_mutex_lock(&d->tx_lock);
    if (!d->tx_synced) {
        /* The DMA started at some moment after we read the cycle timer. Group 0's last packet has just gone
         * out: shift our timeline so that its cycle is the bus's current one, which makes the time stamps in
         * the packets we make from here on mean what the camcorder's cycle timer says. */
        UInt32 ct = 0;
        if (!(*d->dev)->GetCycleTime(d->dev, &ct)) {
            int now = (int)(ct >> 12 & 0x1fff);
            int believed = (int)((d->tx_start_cycle + (g + 1) * GROUP - 1) % 8000);
            int delta = ((now - believed) % 8000 + 8000) % 8000;
            d->tx.cycle += (uint64_t)delta;
            d->tx.next_pt += (double)delta * 3072.0;
            if (fwdv_verbose)
                fprintf(stderr, "fwdv: transmit timeline shifted by %d cycles\n", delta);
        }
        d->tx_synced = 1;
    }
    /* Group g has just gone out; refill it. A packet's size cannot be changed once the program is
     * compiled (IOFireWireLib's ModifyTransferPacketDCLSize is a stub returning kIOReturnUnsupported),
     * so each cycle has a data DCL and an empty DCL, and the jumps before it pick one. */
    for (int i = 0; i < GROUP; i++) {
        int slot = (int)(g * GROUP + i);
        tx_fill(d, slot);
        int want = d->tx_sizes[slot] == TXDATA;
        if (want != d->tx_sel[slot]) {
            int prev = (slot + NPKT - 1) % NPKT;
            DCLLabel *to = want ? &d->tx_labA[slot] : &d->tx_labE[slot];
            IOReturn e = (*d->lport)->ModifyJumpDCL(d->lport, &d->tx_jmpA[prev], to);
            if (!e)
                e = (*d->lport)->ModifyJumpDCL(d->lport, &d->tx_jmpE[prev], to);
            if (e) {
                if (!d->tx_modify_errors++ && fwdv_verbose)
                    fprintf(stderr, "fwdv: switching a cycle's packet failed: %08x\n", e);
            } else {
                d->tx_sel[slot] = (uint8_t)want;
            }
        }
    }
    d->tx_switched_groups++;
    pthread_mutex_unlock(&d->tx_lock);
}

static int tx_setup(fwdv_t *d)
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
    d->channel = TXCHAN;

    /* count our packets from the bus cycle it is now; tx_group corrects this once the DMA is running */
    UInt32 ct = 0;
    (*d->dev)->GetCycleTime(d->dev, &ct);
    int start = (int)((ct >> 12 & 0x1fff) % 8000);
    d->tx_start_cycle = start;
    d->tx_synced = 0;
    d->tx_modify_errors = 0;
    dv_tx_init(&d->tx, d->tx_pal, (uint32_t)start);
    UInt16 node = 0;
    if (!(*d->dev)->GetLocalNodeID(d->dev, &node))
        d->tx.sid = (uint8_t)(node & 0x3f);
    d->tx_cur_slot = -1;

    pthread_mutex_lock(&d->tx_lock);
    d->tx_switched_groups = 0;
    for (int i = 0; i < NPKT; i++)
        tx_fill(d, i);
    /* The program: a start (tag 1, CIP data; then a jump to cycle 0's packet), and for every cycle i
     *   A_i: label, send data packet (488), [call every GROUP cycles], jump to cycle i+1's packet
     *   E_i: label, send empty packet (8),  [call every GROUP cycles], jump to cycle i+1's packet
     * The last cycle's jumps lead back to cycle 0. */
    memset(&d->tx_tag, 0, sizeof(d->tx_tag));
    d->tx_tag.opcode = kDCLSetTagSyncBitsOp;
    d->tx_tag.tagBits = 1;
    d->tx_tag.syncBits = 0;
    memset(&d->label, 0, sizeof(d->label));
    d->label.opcode = kDCLLabelOp;
    d->label.pNextDCLCommand = (DCLCommand *)&d->tx_tag;
    memset(&d->jump, 0, sizeof(d->jump));
    d->jump.opcode = kDCLJumpOp;
    d->tx_tag.pNextDCLCommand = (DCLCommand *)&d->jump;
    DCLCommand *prev = (DCLCommand *)&d->jump;
    for (int i = 0; i < NPKT; i++) {
        d->tx_sel[i] = d->tx_sizes[i] == TXDATA;
        for (int k = 0; k < 2; k++) {
            DCLLabel *lab = k ? &d->tx_labE[i] : &d->tx_labA[i];
            DCLTransferPacket *x = k ? &d->tx_sendE[i] : &d->xfer[i];
            DCLJump *j = k ? &d->tx_jmpE[i] : &d->tx_jmpA[i];
            memset(lab, 0, sizeof(*lab));
            lab->opcode = kDCLLabelOp;
            prev->pNextDCLCommand = (DCLCommand *)lab;
            memset(x, 0, sizeof(*x));
            x->opcode = kDCLSendPacketStartOp;
            x->buffer = d->bufs + (size_t)i * PKTSZ + (k ? TXEOFF : 0);
            x->size = k ? TXEMPTY : TXDATA;
            lab->pNextDCLCommand = (DCLCommand *)x;
            prev = (DCLCommand *)x;
            if ((i + 1) % GROUP == 0) {
                DCLCallProc *c = k ? &d->tx_callsE[i / GROUP] : &d->tx_callsA[i / GROUP];
                memset(c, 0, sizeof(*c));
                c->opcode = kDCLCallProcOp;
                c->proc = tx_group;
                c->procData = (procdata_t)(long)(i / GROUP);
                prev->pNextDCLCommand = (DCLCommand *)c;
                prev = (DCLCommand *)c;
            }
            memset(j, 0, sizeof(*j));
            j->opcode = kDCLJumpOp;
            prev->pNextDCLCommand = (DCLCommand *)j;
            prev = (DCLCommand *)j;
        }
    }
    prev->pNextDCLCommand = NULL;
    for (int i = 0; i < NPKT; i++) {
        int n = (i + 1) % NPKT;
        DCLLabel *to = d->tx_sel[n] ? &d->tx_labA[n] : &d->tx_labE[n];
        d->tx_jmpA[i].pJumpDCLLabel = to;
        d->tx_jmpE[i].pJumpDCLLabel = to;
    }
    d->jump.pJumpDCLLabel = d->tx_sel[0] ? &d->tx_labA[0] : &d->tx_labE[0];
    pthread_mutex_unlock(&d->tx_lock);

    IOVirtualRange br = { (IOVirtualAddress)d->bufs, NPKT * PKTSZ };
    d->lport = (*d->dev)->CreateLocalIsochPort(d->dev, true, (DCLCommand *)&d->label, 0, 0, 0, NULL, 0, &br, 1,
                                               CFUUIDGetUUIDBytes(kIOFireWireLocalIsochPortInterfaceID));
    d->rport = (*d->dev)->CreateRemoteIsochPort(d->dev, false, CFUUIDGetUUIDBytes(kIOFireWireRemoteIsochPortInterfaceID));
    d->chan = (*d->dev)->CreateIsochChannel(d->dev, true, 488, kFWSpeed100MBit,
                                            CFUUIDGetUUIDBytes(kIOFireWireIsochChannelInterfaceID));
    if (!d->lport || !d->rport || !d->chan) {
        if (fwdv_verbose)
            fprintf(stderr, "fwdv: creating the transmit ports failed (%p %p %p)\n", (void *)d->lport,
                    (void *)d->rport, (void *)d->chan);
        return FWDV_ERR_IO;
    }
    (*d->rport)->SetGetSupportedHandler(d->rport, rp_supported);
    (*d->rport)->SetAllocatePortHandler(d->rport, rp_alloc);
    (*d->rport)->SetReleasePortHandler(d->rport, rp_ok);
    (*d->rport)->SetStartHandler(d->rport, rp_ok);
    (*d->rport)->SetStopHandler(d->rport, rp_ok);
    if ((e = (*d->chan)->SetTalker(d->chan, (IOFireWireLibIsochPortRef)d->lport)) ||
        (e = (*d->chan)->AddListener(d->chan, (IOFireWireLibIsochPortRef)d->rport)) ||
        (e = (*d->chan)->AllocateChannel(d->chan))) {
        if (fwdv_verbose)
            fprintf(stderr, "fwdv: isochronous transmit setup failed: %08x\n", e);
        return FWDV_ERR_IO;
    }
    /* tell the camcorder's input plug which channel to listen to, then start sending */
    if ((e = (*d->avc)->makeP2PInputConnection(d->avc, 0, TXCHAN))) {
        if (fwdv_verbose)
            fprintf(stderr, "fwdv: connecting the input plug failed: %08x\n", e);
        return FWDV_ERR_IO;
    }
    d->tx_connected = 1;
    if ((e = (*d->chan)->Start(d->chan))) {
        if (fwdv_verbose)
            fprintf(stderr, "fwdv: starting the transmit failed: %08x\n", e);
        return FWDV_ERR_IO;
    }
    if (fwdv_verbose)
        fprintf(stderr, "fwdv: sending on channel %d as node %d from cycle %d\n", TXCHAN, d->tx.sid, start);
    return FWDV_OK;
}

static void *tx_thread_main(void *arg)
{
    fwdv_t *d = arg;
    d->loop = CFRunLoopGetCurrent();
    int r = tx_setup(d);
    pthread_mutex_lock(&d->lock);
    d->tx_result = r;
    d->tx_running = r == FWDV_OK;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->lock);
    if (r == FWDV_OK)
        while (d->tx_running)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, false);
    teardown(d);
    return NULL;
}

int fwdv_send_start(fwdv_t *d, int pal)
{
    if (d->thread || d->tx_thread)
        return FWDV_ERR_BUSY; /* receiving: fwdv_stop first */
    if (!d->tx_q && !(d->tx_q = calloc(TXQ, DV_FRAME_625)))
        return FWDV_ERR_IO;
    if (!d->bufs && !(d->bufs = calloc(NPKT, PKTSZ)))
        return FWDV_ERR_IO;
    pthread_mutex_init(&d->tx_lock, NULL);
    pthread_cond_init(&d->tx_cv, NULL);
    d->tx_head = d->tx_count = d->tx_draining = 0;
    d->tx_queued = d->tx_sent = 0;
    d->tx_pal = pal != 0;
    d->tx_result = 1;
    g_rx = d;
    if (pthread_create(&d->tx_thread, NULL, tx_thread_main, d)) {
        d->tx_thread = 0;
        return FWDV_ERR_IO;
    }
    pthread_mutex_lock(&d->lock);
    while (d->tx_result == 1)
        pthread_cond_wait(&d->cv, &d->lock);
    int r = d->tx_result;
    pthread_mutex_unlock(&d->lock);
    if (r) {
        pthread_join(d->tx_thread, NULL);
        d->tx_thread = 0;
        g_rx = NULL;
    }
    return r;
}

int fwdv_send_frame(fwdv_t *d, const uint8_t *frame, size_t len, volatile int *abort)
{
    if (!d->tx_thread || (len != DV_FRAME_525 && len != DV_FRAME_625))
        return FWDV_ERR_UNSUPPORTED;
    if ((len == DV_FRAME_625) != d->tx_pal)
        return FWDV_ERR_UNSUPPORTED; /* the stream's standard cannot change in the middle */
    pthread_mutex_lock(&d->tx_lock);
    while (d->tx_count == TXQ && d->tx_running && !(abort && *abort)) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        long ns = tv.tv_usec * 1000 + 100 * 1000000;
        struct timespec ts = { tv.tv_sec + ns / 1000000000, ns % 1000000000 };
        pthread_cond_timedwait(&d->tx_cv, &d->tx_lock, &ts);
    }
    int ok = d->tx_count < TXQ && d->tx_running;
    if (ok) {
        memcpy(d->tx_q + (size_t)((d->tx_head + d->tx_count) % TXQ) * DV_FRAME_625, frame, len);
        d->tx_count++;
        d->tx_queued++;
    }
    pthread_mutex_unlock(&d->tx_lock);
    return ok ? FWDV_OK : FWDV_ERR_GONE;
}

void fwdv_send_stop(fwdv_t *d, int drain)
{
    if (!d->tx_thread)
        return;
    if (drain) { /* let the queue play out (and the ring: about 0.2 s more) */
        for (int i = 0; i < 300; i++) {
            pthread_mutex_lock(&d->tx_lock);
            int left = d->tx_count;
            pthread_mutex_unlock(&d->tx_lock);
            if (!left)
                break;
            usleep(100000);
        }
        usleep(400000);
    }
    d->tx_running = 0;
    if (d->loop)
        CFRunLoopStop(d->loop);
    pthread_join(d->tx_thread, NULL);
    if (fwdv_verbose)
        fprintf(stderr, "fwdv: sent %llu of %llu frames; %llu transmit callbacks, %d packet switches failed\n",
                (unsigned long long)d->tx_sent, (unsigned long long)d->tx_queued,
                (unsigned long long)d->tx_switched_groups, d->tx_modify_errors);
    d->tx_thread = 0;
    d->loop = NULL;
    g_rx = NULL;
    free(d->bufs);
    d->bufs = NULL;
    free(d->tx_q);
    d->tx_q = NULL;
}

void fwdv_send_stats(fwdv_t *d, uint64_t *queued, uint64_t *sent)
{
    *queued = d->tx_queued;
    *sent = d->tx_sent;
}

#endif /* !LEGACY_MAC */
