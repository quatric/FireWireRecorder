/*
 * DV over FireWire on Linux through the kernel's firewire-cdev interface
 * (/dev/fw*, the "juju" stack, Linux 2.6.36 and later):
 *
 *   - the camcorder is the /dev/fw* node whose configuration ROM has an AV/C
 *     unit (specifier 0x00A02D, version 0x010001);
 *   - AV/C commands go out as block writes to its FCP command register
 *     (0xFFFFF0000B00); the answers come back as writes to our FCP response
 *     register (0xFFFFF0000D00), which the kernel shares between programs;
 *   - the DV stream is received with an isochronous receive context on the
 *     channel its output plug (oPCR[0]) says, one 512-byte slot per packet.
 *
 * /dev/fw* is usually root-only or group "video"; FWDV_ERR_ACCESS says so.
 */
#include "fwdv.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/firewire-cdev.h>
#include <linux/firewire-constants.h>

#define FCP_COMMAND 0xfffff0000b00ULL
#define FCP_RESPONSE 0xfffff0000d00ULL
#define OPCR0 0xfffff0000904ULL

#define PKTSZ 2048 /* receive slot per packet (DV: 8 CIP + 480 bytes; HDV: several 192-byte source packets) */
#define NPKTS 1024   /* ring of slots, ~128 ms at 8000 packets/s */
#define IRQ_EVERY 32 /* an interrupt (event) every this many packets */

struct fwdv {
    char path[32], name[128];
    int fd;      /* AV/C and requests */
    uint32_t generation, node_id;
    uint32_t guid_hi, guid_lo;
    pthread_mutex_t lock;

    /* isochronous receive */
    int ifd, channel, running;
    uint32_t handle;
    uint8_t *ring;
    unsigned head; /* next slot the controller fills */
    pthread_t thread;
    dv_assembler_t as;
};

static void logv(int level, const char *fmt, const char *a, int b)
{
    if (fwdv_verbose >= level) {
        fprintf(stderr, "fwdv: ");
        fprintf(stderr, fmt, a, b);
        fprintf(stderr, "\n");
    }
}

static int errno_to_fwdv(int e)
{
    switch (e) {
    case EACCES:
    case EPERM: return FWDV_ERR_ACCESS;
    case EBUSY: return FWDV_ERR_BUSY;
    case ENODEV:
    case ENOENT:
    case ENXIO: return FWDV_ERR_GONE;
    default: return FWDV_ERR_IO;
    }
}

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* GET_INFO also tells the kernel which ABI we speak (4: REQUEST2 events) */
static int get_info(int fd, uint32_t *rom, size_t romlen, struct fw_cdev_event_bus_reset *br)
{
    struct fw_cdev_get_info gi;
    memset(&gi, 0, sizeof(gi));
    gi.version = 4;
    gi.rom = (uintptr_t)rom;
    gi.rom_length = romlen;
    gi.bus_reset = (uintptr_t)br;
    if (ioctl(fd, FW_CDEV_IOC_GET_INFO, &gi) < 0)
        return -1;
    return (int)(gi.rom_length / 4);
}

/* The ROM comes in the CPU's order from the kernel; key 0x12 is the unit's
 * specifier id, 0x13 its software version. */
static int rom_has_avc(const uint32_t *rom, int n)
{
    int spec = 0, ver = 0;
    for (int i = 5; i < n && i < 256; i++) {
        uint32_t q = rom[i];
        if (q == 0x1200a02d)
            spec = 1;
        if (q == 0x13010001)
            ver = 1;
    }
    return spec && ver;
}

static void sysfs_string(const char *dev, const char *attr, char *out, size_t len)
{
    char p[128];
    snprintf(p, sizeof(p), "/sys/bus/firewire/devices/%s/%s", dev, attr);
    out[0] = 0;
    FILE *f = fopen(p, "r");
    if (!f)
        return;
    if (fgets(out, (int)len, f))
        out[strcspn(out, "\r\n")] = 0;
    fclose(f);
}

static int fcp_listen(fwdv_t *d)
{
    struct fw_cdev_allocate a;
    memset(&a, 0, sizeof(a));
    a.offset = FCP_RESPONSE;
    a.length = 0x200;
    a.region_end = FCP_RESPONSE + 0x200;
    return ioctl(d->fd, FW_CDEV_IOC_ALLOCATE, &a) < 0 ? errno_to_fwdv(errno) : FWDV_OK;
}

int fwdv_open(fwdv_t **out)
{
    *out = NULL;
    int saw_access = 0;
    for (int i = 0; i < 64; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/fw%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            if (errno == EACCES || errno == EPERM) {
                /* readable for the ROM? then at least say it is a camera we cannot use */
                int ro = open(path, O_RDONLY | O_CLOEXEC);
                if (ro >= 0) {
                    uint32_t rom[256];
                    struct fw_cdev_event_bus_reset br;
                    int n = get_info(ro, rom, sizeof(rom), &br);
                    if (n > 0 && br.node_id != br.local_node_id && rom_has_avc(rom, n))
                        saw_access = 1;
                    close(ro);
                } else {
                    saw_access = 1;
                }
            }
            continue;
        }
        uint32_t rom[256];
        struct fw_cdev_event_bus_reset br;
        int n = get_info(fd, rom, sizeof(rom), &br);
        if (n <= 0 || br.node_id == br.local_node_id || !rom_has_avc(rom, n)) {
            close(fd);
            continue;
        }
        fwdv_t *d = calloc(1, sizeof(*d));
        if (!d) {
            close(fd);
            return FWDV_ERR_IO;
        }
        snprintf(d->path, sizeof(d->path), "%s", path);
        d->fd = fd;
        d->ifd = -1;
        d->generation = br.generation;
        d->node_id = br.node_id;
        d->guid_hi = n > 4 ? rom[3] : 0;
        d->guid_lo = n > 4 ? rom[4] : 0;
        pthread_mutex_init(&d->lock, NULL);

        char vendor[64], model[64];
        sysfs_string(path + 5, "vendor_name", vendor, sizeof(vendor));
        sysfs_string(path + 5, "model_name", model, sizeof(model));
        if (vendor[0] && model[0])
            snprintf(d->name, sizeof(d->name), "%s %s", vendor, model);
        else
            snprintf(d->name, sizeof(d->name), "%s", model[0] ? model : vendor[0] ? vendor : "DV device");

        int e = fcp_listen(d);
        if (e) {
            logv(1, "%s: FCP response register: error %d", path, errno);
            close(fd);
            pthread_mutex_destroy(&d->lock);
            free(d);
            return e;
        }
        logv(1, "found %s on node %d", d->name, (int)(d->node_id & 0x3f));
        *out = d;
        return FWDV_OK;
    }
    return saw_access ? FWDV_ERR_ACCESS : FWDV_ERR_NO_DEVICE;
}

void fwdv_close(fwdv_t *d)
{
    if (!d)
        return;
    fwdv_stop(d);
    close(d->fd);
    pthread_mutex_destroy(&d->lock);
    free(d);
}

const char *fwdv_name(fwdv_t *d) { return d->name; }

int fwdv_alive(fwdv_t *d)
{
    uint32_t rom[8];
    struct fw_cdev_event_bus_reset br;
    pthread_mutex_lock(&d->lock);
    int n = get_info(d->fd, rom, sizeof(rom), &br);
    if (n > 0) {
        d->generation = br.generation;
        d->node_id = br.node_id;
    }
    pthread_mutex_unlock(&d->lock);
    return n > 4 && rom[3] == d->guid_hi && rom[4] == d->guid_lo;
}

/* ---- requests and FCP; called with d->lock held ---- */

static void answer_request(fwdv_t *d, uint32_t handle)
{
    struct fw_cdev_send_response r;
    memset(&r, 0, sizeof(r));
    r.rcode = RCODE_COMPLETE;
    r.handle = handle;
    ioctl(d->fd, FW_CDEV_IOC_SEND_RESPONSE, &r);
}

/*
 * Reads events until a transaction response (want_resp, returning its rcode and
 * data) or an FCP response for opcode `op` arrives, or until the deadline.
 */
static int wait_event(fwdv_t *d, int64_t deadline, int want_resp, int op, uint8_t *data, int *len)
{
    static uint8_t buf[4096 + 256] __attribute__((aligned(8)));
    for (;;) {
        int left = (int)(deadline - now_ms());
        if (left <= 0)
            return FWDV_ERR_IO;
        struct pollfd p = { d->fd, POLLIN, 0 };
        int r = poll(&p, 1, left);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            continue;
        if (p.revents & (POLLHUP | POLLERR))
            return FWDV_ERR_GONE;
        ssize_t n = read(d->fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return errno_to_fwdv(errno);
        }
        if (n < (ssize_t)sizeof(struct fw_cdev_event_common))
            continue;
        union fw_cdev_event *ev = (union fw_cdev_event *)buf;
        switch (ev->common.type) {
        case FW_CDEV_EVENT_BUS_RESET:
            d->generation = ev->bus_reset.generation;
            d->node_id = ev->bus_reset.node_id;
            break;
        case FW_CDEV_EVENT_RESPONSE:
            if (want_resp) {
                int l = (int)ev->response.length;
                if (data && len) {
                    if (l > *len)
                        l = *len;
                    memcpy(data, ev->response.data, (size_t)l);
                    *len = l;
                }
                return (int)ev->response.rcode;
            }
            break;
        case FW_CDEV_EVENT_REQUEST2: {
            struct fw_cdev_event_request2 *q = &ev->request2;
            answer_request(d, q->handle);
            if (want_resp || q->offset != FCP_RESPONSE || q->length < 3 || !data)
                break;
            if (q->generation == d->generation && q->source_node_id != d->node_id)
                break;
            const uint8_t *b = (const uint8_t *)q->data;
            if (b[2] != (uint8_t)op)
                break; /* a late answer to an earlier command */
            if (b[0] == 0x0f) { /* INTERIM: the final answer follows */
                deadline = now_ms() + 3000;
                break;
            }
            int l = (int)q->length < *len ? (int)q->length : *len;
            memcpy(data, b, (size_t)l);
            *len = l;
            return FWDV_OK;
        }
        case FW_CDEV_EVENT_REQUEST: { /* old kernels */
            struct fw_cdev_event_request *q = &ev->request;
            answer_request(d, q->handle);
            if (want_resp || q->offset != FCP_RESPONSE || q->length < 3 || !data)
                break;
            const uint8_t *b = (const uint8_t *)q->data;
            if (b[2] != (uint8_t)op)
                break;
            if (b[0] == 0x0f) {
                deadline = now_ms() + 3000;
                break;
            }
            int l = (int)q->length < *len ? (int)q->length : *len;
            memcpy(data, b, (size_t)l);
            *len = l;
            return FWDV_OK;
        }
        default:
            break;
        }
    }
}

static int send_request(fwdv_t *d, int tcode, uint64_t offset, const void *data, int len, uint8_t *resp,
                        int *rlen)
{
    struct fw_cdev_send_request s;
    memset(&s, 0, sizeof(s));
    s.tcode = (uint32_t)tcode;
    s.length = (uint32_t)len;
    s.offset = offset;
    s.data = (uintptr_t)data;
    s.generation = d->generation;
    if (ioctl(d->fd, FW_CDEV_IOC_SEND_REQUEST, &s) < 0)
        return errno_to_fwdv(errno);
    int rc = wait_event(d, now_ms() + 1000, 1, 0, resp, rlen);
    if (rc < 0)
        return rc;
    if (rc == RCODE_GENERATION) /* a bus reset in between: the next try uses the new one */
        return FWDV_ERR_IO;
    if (rc == RCODE_NO_ACK || rc == RCODE_CANCELLED)
        return FWDV_ERR_GONE;
    return rc == RCODE_COMPLETE ? FWDV_OK : FWDV_ERR_IO;
}

static int avc_raw(fwdv_t *d, const uint8_t *cmd, int n, uint8_t *resp, int *rlen)
{
    uint8_t c[64] __attribute__((aligned(4)));
    int padded = (n + 3) & ~3;
    memset(c, 0, sizeof(c));
    memcpy(c, cmd, (size_t)n);
    pthread_mutex_lock(&d->lock);
    int e = FWDV_ERR_IO;
    for (int attempt = 0; attempt < 2 && e; attempt++) {
        e = send_request(d, TCODE_WRITE_BLOCK_REQUEST, FCP_COMMAND, c, padded, NULL, NULL);
        if (!e)
            e = wait_event(d, now_ms() + 1000, 0, cmd[2], resp, rlen);
        if (e == FWDV_ERR_GONE)
            break;
    }
    pthread_mutex_unlock(&d->lock);
    if (fwdv_verbose >= 2 && !e) {
        fprintf(stderr, "fwdv: avc");
        for (int i = 0; i < n; i++)
            fprintf(stderr, " %02x", cmd[i]);
        fprintf(stderr, " ->");
        for (int i = 0; i < *rlen; i++)
            fprintf(stderr, " %02x", resp[i]);
        fprintf(stderr, "\n");
    }
    return e;
}

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

/* the channel the camera sends on: its output plug says, or broadcast channel 63 */
static int find_channel(fwdv_t *d)
{
    uint8_t q[4];
    int ql = 4, chan = 63;
    pthread_mutex_lock(&d->lock);
    int e = send_request(d, TCODE_READ_QUADLET_REQUEST, OPCR0, NULL, 0, q, &ql);
    pthread_mutex_unlock(&d->lock);
    if (!e && ql == 4) {
        uint32_t v = (uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3];
        logv(1, "oPCR %s%08x", "", (int)v);
        if (v >> 31 && (v >> 24 & 0x3f || v >> 30 & 1)) /* online, connected */
            chan = (int)(v >> 16 & 0x3f);
    }
    return chan;
}

/* hands the slots [first, first+count) back to the controller */
static int queue_slots(fwdv_t *d, unsigned first, unsigned count)
{
    while (count) {
        unsigned n = count;
        if (first + n > NPKTS)
            n = NPKTS - first;
        uint32_t ctl[NPKTS];
        for (unsigned i = 0; i < n; i++) {
            ctl[i] = FW_CDEV_ISO_PAYLOAD_LENGTH(PKTSZ) | FW_CDEV_ISO_HEADER_LENGTH(4);
            if ((first + i + 1) % IRQ_EVERY == 0)
                ctl[i] |= FW_CDEV_ISO_INTERRUPT;
        }
        struct fw_cdev_queue_iso q;
        memset(&q, 0, sizeof(q));
        q.packets = (uintptr_t)ctl;
        q.data = (uintptr_t)(d->ring + (size_t)first * PKTSZ);
        q.size = (uint32_t)(n * sizeof(uint32_t));
        q.handle = d->handle;
        while (q.size) {
            int r = ioctl(d->ifd, FW_CDEV_IOC_QUEUE_ISO, &q);
            if (r < 0)
                return errno_to_fwdv(errno);
            if (r == 0 && q.size)
                break; /* the controller's queue is full; the rest goes next time */
        }
        first = (first + n) % NPKTS;
        count -= n;
    }
    return FWDV_OK;
}

static void *rx_thread(void *arg)
{
    fwdv_t *d = arg;
    static uint8_t buf[sizeof(struct fw_cdev_event_iso_interrupt) + 4096] __attribute__((aligned(8)));
    while (__atomic_load_n(&d->running, __ATOMIC_ACQUIRE)) {
        struct pollfd p = { d->ifd, POLLIN, 0 };
        int r = poll(&p, 1, 100);
        if (r <= 0)
            continue;
        if (p.revents & (POLLHUP | POLLERR))
            break;
        ssize_t n = read(d->ifd, buf, sizeof(buf));
        if (n < (ssize_t)sizeof(struct fw_cdev_event_common))
            continue;
        union fw_cdev_event *ev = (union fw_cdev_event *)buf;
        if (ev->common.type != FW_CDEV_EVENT_ISO_INTERRUPT)
            continue;
        unsigned count = ev->iso_interrupt.header_length / 4, first = d->head;
        for (unsigned i = 0; i < count; i++) {
            uint32_t h = ev->iso_interrupt.header[i];
            const uint8_t *hb = (const uint8_t *)&h; /* the packet header, big-endian */
            unsigned len = (unsigned)hb[0] << 8 | hb[1];
            if (len >= 8 && len <= PKTSZ - 4)
                dv_assemble(&d->as, d->ring + (size_t)d->head * PKTSZ, len);
            d->head = (d->head + 1) % NPKTS;
        }
        if (queue_slots(d, first, count))
            break;
    }
    return NULL;
}

int fwdv_start(fwdv_t *d, fwdv_frame_cb cb, void *user)
{
    if (d->running)
        return FWDV_OK;
    memset(&d->as, 0, sizeof(d->as));
    d->as.cb = cb;
    d->as.user = user;
    d->channel = find_channel(d);

    d->ifd = open(d->path, O_RDWR | O_CLOEXEC);
    if (d->ifd < 0)
        return errno_to_fwdv(errno);
    uint32_t rom[8];
    struct fw_cdev_event_bus_reset br;
    get_info(d->ifd, rom, sizeof(rom), &br);

    struct fw_cdev_create_iso_context c;
    memset(&c, 0, sizeof(c));
    c.type = FW_CDEV_ISO_CONTEXT_RECEIVE;
    c.header_size = 4;
    c.channel = (uint32_t)d->channel;
    c.speed = SCODE_100;
    int e = FWDV_ERR_IO;
    if (ioctl(d->ifd, FW_CDEV_IOC_CREATE_ISO_CONTEXT, &c) < 0) {
        e = errno == EBUSY ? FWDV_ERR_BUSY : errno_to_fwdv(errno);
        goto fail;
    }
    d->handle = c.handle;
    d->ring = mmap(NULL, (size_t)NPKTS * PKTSZ, PROT_READ, MAP_SHARED, d->ifd, 0);
    if (d->ring == MAP_FAILED) {
        d->ring = NULL;
        e = errno_to_fwdv(errno);
        goto fail;
    }
    d->head = 0;
    if ((e = queue_slots(d, 0, NPKTS)))
        goto fail;
    struct fw_cdev_start_iso s;
    memset(&s, 0, sizeof(s));
    s.cycle = -1;
    s.tags = FW_CDEV_ISO_CONTEXT_MATCH_ALL_TAGS;
    s.handle = d->handle;
    if (ioctl(d->ifd, FW_CDEV_IOC_START_ISO, &s) < 0) {
        e = errno_to_fwdv(errno);
        goto fail;
    }
    d->running = 1;
    if (pthread_create(&d->thread, NULL, rx_thread, d)) {
        d->running = 0;
        e = FWDV_ERR_IO;
        goto fail;
    }
    logv(1, "receiving on channel %s%d", "", d->channel);
    return FWDV_OK;
fail:
    logv(1, "isochronous receive: %s%d", "error ", errno);
    if (d->ring)
        munmap(d->ring, (size_t)NPKTS * PKTSZ);
    d->ring = NULL;
    close(d->ifd);
    d->ifd = -1;
    return e;
}

void fwdv_stop(fwdv_t *d)
{
    if (!d->running)
        return;
    __atomic_store_n(&d->running, 0, __ATOMIC_RELEASE);
    pthread_join(d->thread, NULL);
    struct fw_cdev_stop_iso s = { d->handle };
    ioctl(d->ifd, FW_CDEV_IOC_STOP_ISO, &s);
    munmap(d->ring, (size_t)NPKTS * PKTSZ);
    d->ring = NULL;
    close(d->ifd); /* closing frees the context */
    d->ifd = -1;
}
