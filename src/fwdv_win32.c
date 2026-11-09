/*
 * Windows backend (see fwdv.h): the DV camcorder through Windows' own DV
 * driver (MSDV, "Microsoft DV Camera and VCR"), which every Windows from XP
 * on installs for a camcorder plugged into a FireWire port. DirectShow
 * delivers its interleaved DV frames to a Sample Grabber, and the driver's
 * IAMExtTransport and IAMTimecodeReader run the tape.
 *
 * All COM objects live on one thread of their own (MTA): the engine and the
 * GUI ask it to do things through a small request queue, so it does not
 * matter which thread or apartment they call from.
 */
#define COBJMACROS
#define CINTERFACE
#include "fwdv.h"

#include <windows.h>
#include <dshow.h>
#include <edevdefs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Sample Grabber (qedit.dll): declared here, as not every SDK still ships qedit.h */
static const GUID CLSID_SampleGrabber_ = { 0xc1f400a0, 0x3f08, 0x11d3, { 0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37 } };
static const GUID CLSID_NullRenderer_ = { 0xc1f400a4, 0x3f08, 0x11d3, { 0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37 } };
static const GUID IID_ISampleGrabber_ = { 0x6b652fff, 0x11fe, 0x4fce, { 0x92, 0xad, 0x02, 0x66, 0xb5, 0xd7, 0xc7, 0x8f } };
static const GUID IID_ISampleGrabberCB_ = { 0x0579154a, 0x2b53, 0x4994, { 0xb0, 0xd0, 0xe7, 0x73, 0x14, 0x8e, 0xff, 0x85 } };
/* 'iavs': interleaved audio and video, as MSDV sends DV */
static const GUID MEDIATYPE_Interleaved_ = { 0x73766169, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

typedef struct SG ISG;
typedef struct SGCB ISGCB;
typedef struct {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(ISGCB *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(ISGCB *);
    ULONG(STDMETHODCALLTYPE *Release)(ISGCB *);
    HRESULT(STDMETHODCALLTYPE *SampleCB)(ISGCB *, double, IMediaSample *);
    HRESULT(STDMETHODCALLTYPE *BufferCB)(ISGCB *, double, BYTE *, LONG);
} ISGCBVtbl;
struct SGCB {
    const ISGCBVtbl *lpVtbl;
};
typedef struct {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(ISG *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(ISG *);
    ULONG(STDMETHODCALLTYPE *Release)(ISG *);
    HRESULT(STDMETHODCALLTYPE *SetOneShot)(ISG *, BOOL);
    HRESULT(STDMETHODCALLTYPE *SetMediaType)(ISG *, const AM_MEDIA_TYPE *);
    HRESULT(STDMETHODCALLTYPE *GetConnectedMediaType)(ISG *, AM_MEDIA_TYPE *);
    HRESULT(STDMETHODCALLTYPE *SetBufferSamples)(ISG *, BOOL);
    HRESULT(STDMETHODCALLTYPE *GetCurrentBuffer)(ISG *, LONG *, LONG *);
    HRESULT(STDMETHODCALLTYPE *GetCurrentSample)(ISG *, IMediaSample **);
    HRESULT(STDMETHODCALLTYPE *SetCallback)(ISG *, ISGCB *, LONG);
} ISGVtbl;
struct SG {
    const ISGVtbl *lpVtbl;
};

enum { REQ_NONE, REQ_OPEN, REQ_START, REQ_STOP, REQ_TRANSPORT, REQ_STATUS, REQ_ALIVE, REQ_QUIT };

struct fwdv {
    /* the COM thread and its requests */
    HANDLE thread;
    CRITICAL_SECTION lock;
    HANDLE req_event, done_event;
    int req, arg, result;
    fwdv_status_t *st;

    char name[128];
    IGraphBuilder *graph;
    IBaseFilter *src, *grab, *null;
    ISG *sg;
    IMediaControl *control;
    IMediaEventEx *events;
    IAMExtTransport *transport;
    IAMExtDevice *device;
    IAMTimecodeReader *tc;
    int running, lost;

    ISGCB cb_obj;
    fwdv_frame_cb cb;
    void *user;
};

/* ---- the Sample Grabber callback: one DV frame per buffer ---- */

static fwdv_t *g_dev;

static HRESULT STDMETHODCALLTYPE cb_qi(ISGCB *self, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_ISampleGrabberCB_)) {
        *out = self;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE cb_addref(ISGCB *self) { (void)self; return 2; }
static ULONG STDMETHODCALLTYPE cb_release(ISGCB *self) { (void)self; return 1; }
static HRESULT STDMETHODCALLTYPE cb_sample(ISGCB *self, double t, IMediaSample *s)
{
    (void)self, (void)t, (void)s;
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE cb_buffer(ISGCB *self, double t, BYTE *buf, LONG len)
{
    (void)self, (void)t;
    fwdv_t *d = g_dev;
    if (d && d->cb && (len == DV_FRAME_525 || len == DV_FRAME_625))
        d->cb(buf, (size_t)len, d->user);
    return S_OK;
}
static const ISGCBVtbl cb_vtbl = { cb_qi, cb_addref, cb_release, cb_sample, cb_buffer };

/* ---- graph helpers ---- */

#define RELEASE(p)                                                                                                     \
    do {                                                                                                               \
        if (p) {                                                                                                       \
            (p)->lpVtbl->Release(p);                                                                                   \
            (p) = NULL;                                                                                                \
        }                                                                                                              \
    } while (0)

static void free_mt(AM_MEDIA_TYPE *mt)
{
    if (mt->cbFormat)
        CoTaskMemFree(mt->pbFormat);
    if (mt->pUnk)
        mt->pUnk->lpVtbl->Release(mt->pUnk);
    CoTaskMemFree(mt);
}

/* the first pin of a direction; for the source, the one that carries interleaved DV */
static IPin *find_pin(IBaseFilter *f, PIN_DIRECTION dir, int want_interleaved)
{
    IEnumPins *e = NULL;
    IPin *p = NULL, *found = NULL;
    if (FAILED(IBaseFilter_EnumPins(f, &e)))
        return NULL;
    while (!found && IEnumPins_Next(e, 1, &p, NULL) == S_OK) {
        PIN_DIRECTION d;
        IPin_QueryDirection(p, &d);
        int ok = d == dir;
        if (ok && want_interleaved) {
            ok = 0;
            IEnumMediaTypes *mts = NULL;
            AM_MEDIA_TYPE *mt;
            if (SUCCEEDED(IPin_EnumMediaTypes(p, &mts))) {
                while (!ok && IEnumMediaTypes_Next(mts, 1, &mt, NULL) == S_OK) {
                    ok = IsEqualGUID(&mt->majortype, &MEDIATYPE_Interleaved_);
                    free_mt(mt);
                }
                IEnumMediaTypes_Release(mts);
            }
        }
        if (ok)
            found = p;
        else
            IPin_Release(p);
    }
    IEnumPins_Release(e);
    return found;
}

static void close_graph(fwdv_t *d)
{
    if (d->control && d->running)
        IMediaControl_Stop(d->control);
    d->running = 0;
    RELEASE(d->tc);
    RELEASE(d->device);
    RELEASE(d->transport);
    RELEASE(d->events);
    RELEASE(d->control);
    RELEASE(d->sg);
    RELEASE(d->null);
    RELEASE(d->grab);
    RELEASE(d->src);
    RELEASE(d->graph);
}

/* finds the first video capture device with a tape transport: MSDV's DV camcorders and decks */
static int do_open(fwdv_t *d)
{
    ICreateDevEnum *de = NULL;
    IEnumMoniker *em = NULL;
    IMoniker *m = NULL;
    int r = FWDV_ERR_NO_DEVICE;
    if (FAILED(CoCreateInstance(&CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER, &IID_ICreateDevEnum, (void **)&de)))
        return FWDV_ERR_IO;
    if (ICreateDevEnum_CreateClassEnumerator(de, &CLSID_VideoInputDeviceCategory, &em, 0) != S_OK) {
        ICreateDevEnum_Release(de);
        return FWDV_ERR_NO_DEVICE;
    }
    while (!d->src && IEnumMoniker_Next(em, 1, &m, NULL) == S_OK) {
        IBaseFilter *f = NULL;
        IAMExtTransport *t = NULL;
        if (SUCCEEDED(IMoniker_BindToObject(m, NULL, NULL, &IID_IBaseFilter, (void **)&f)) &&
            SUCCEEDED(IBaseFilter_QueryInterface(f, &IID_IAMExtTransport, (void **)&t))) {
            d->src = f;
            d->transport = t;
            IPropertyBag *bag = NULL;
            VARIANT v;
            VariantInit(&v);
            snprintf(d->name, sizeof(d->name), "DV device");
            if (SUCCEEDED(IMoniker_BindToStorage(m, NULL, NULL, &IID_IPropertyBag, (void **)&bag)) &&
                SUCCEEDED(IPropertyBag_Read(bag, L"FriendlyName", &v, NULL)) && v.vt == VT_BSTR)
                WideCharToMultiByte(CP_UTF8, 0, v.bstrVal, -1, d->name, sizeof(d->name), NULL, NULL);
            VariantClear(&v);
            RELEASE(bag);
        } else {
            RELEASE(f);
        }
        IMoniker_Release(m);
    }
    IEnumMoniker_Release(em);
    ICreateDevEnum_Release(de);
    if (!d->src)
        return r;
    IBaseFilter_QueryInterface(d->src, &IID_IAMExtDevice, (void **)&d->device);
    IBaseFilter_QueryInterface(d->src, &IID_IAMTimecodeReader, (void **)&d->tc);
    if (fwdv_verbose)
        fprintf(stderr, "fwdv: %s\n", d->name);
    return FWDV_OK;
}

static int do_start(fwdv_t *d)
{
    IPin *out = NULL, *gin = NULL, *gout = NULL, *nin = NULL;
    HRESULT hr;
    int r = FWDV_ERR_IO;
    if (FAILED(CoCreateInstance(&CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER, &IID_IGraphBuilder, (void **)&d->graph)) ||
        FAILED(CoCreateInstance(&CLSID_SampleGrabber_, NULL, CLSCTX_INPROC_SERVER, &IID_IBaseFilter, (void **)&d->grab)) ||
        FAILED(CoCreateInstance(&CLSID_NullRenderer_, NULL, CLSCTX_INPROC_SERVER, &IID_IBaseFilter, (void **)&d->null)) ||
        FAILED(IBaseFilter_QueryInterface(d->grab, &IID_ISampleGrabber_, (void **)&d->sg)))
        goto fail;
    if (FAILED(IGraphBuilder_AddFilter(d->graph, d->src, L"DV")) ||
        FAILED(IGraphBuilder_AddFilter(d->graph, d->grab, L"Grabber")) ||
        FAILED(IGraphBuilder_AddFilter(d->graph, d->null, L"Null")))
        goto fail;
    AM_MEDIA_TYPE mt;
    memset(&mt, 0, sizeof(mt));
    mt.majortype = MEDIATYPE_Interleaved_;
    d->sg->lpVtbl->SetMediaType(d->sg, &mt);
    d->sg->lpVtbl->SetOneShot(d->sg, FALSE);
    d->sg->lpVtbl->SetBufferSamples(d->sg, FALSE);
    d->cb_obj.lpVtbl = &cb_vtbl;
    d->sg->lpVtbl->SetCallback(d->sg, &d->cb_obj, 1);
    out = find_pin(d->src, PINDIR_OUTPUT, 1);
    gin = find_pin(d->grab, PINDIR_INPUT, 0);
    gout = find_pin(d->grab, PINDIR_OUTPUT, 0);
    nin = find_pin(d->null, PINDIR_INPUT, 0);
    if (!out || !gin || !gout || !nin) {
        if (fwdv_verbose)
            fprintf(stderr, "fwdv: no interleaved DV output pin\n");
        goto fail;
    }
    if (FAILED(hr = IGraphBuilder_Connect(d->graph, out, gin)) || FAILED(hr = IGraphBuilder_Connect(d->graph, gout, nin))) {
        if (fwdv_verbose)
            fprintf(stderr, "fwdv: connecting the graph failed: %08lx\n", (unsigned long)hr);
        goto fail;
    }
    IGraphBuilder_QueryInterface(d->graph, &IID_IMediaControl, (void **)&d->control);
    IGraphBuilder_QueryInterface(d->graph, &IID_IMediaEventEx, (void **)&d->events);
    if (!d->control || FAILED(hr = IMediaControl_Run(d->control))) {
        if (fwdv_verbose)
            fprintf(stderr, "fwdv: starting the graph failed: %08lx\n", (unsigned long)hr);
        r = FWDV_ERR_BUSY; /* most often another program has the camcorder */
        goto fail;
    }
    d->running = 1;
    r = FWDV_OK;
fail:
    RELEASE(out);
    RELEASE(gin);
    RELEASE(gout);
    RELEASE(nin);
    if (r) {
        /* keep the source and its transport for the tape commands */
        if (d->graph && d->src)
            IGraphBuilder_RemoveFilter(d->graph, d->src);
        RELEASE(d->events);
        RELEASE(d->control);
        RELEASE(d->sg);
        RELEASE(d->null);
        RELEASE(d->grab);
        RELEASE(d->graph);
    }
    return r;
}

static void do_stop(fwdv_t *d)
{
    if (!d->graph)
        return;
    if (d->control && d->running)
        IMediaControl_Stop(d->control);
    d->running = 0;
    IGraphBuilder_RemoveFilter(d->graph, d->src);
    RELEASE(d->events);
    RELEASE(d->control);
    RELEASE(d->sg);
    RELEASE(d->null);
    RELEASE(d->grab);
    RELEASE(d->graph);
}

static int do_transport(fwdv_t *d, fwdv_cmd_t cmd)
{
    long mode;
    switch (cmd) {
    case FWDV_PLAY: mode = ED_MODE_PLAY; break;
    case FWDV_PAUSE: mode = ED_MODE_FREEZE; break;
    case FWDV_STOP: mode = ED_MODE_STOP; break;
    case FWDV_REWIND: mode = ED_MODE_REW; break;
    case FWDV_FFWD: mode = ED_MODE_FF; break;
    case FWDV_STEP_FWD: mode = ED_MODE_STEP_FWD; break;
    case FWDV_STEP_BACK: mode = ED_MODE_STEP_REV; break;
    default: return FWDV_ERR_UNSUPPORTED;
    }
    if (!d->transport)
        return FWDV_ERR_UNSUPPORTED;
    HRESULT hr = IAMExtTransport_put_Mode(d->transport, mode);
    if (fwdv_verbose > 1)
        fprintf(stderr, "fwdv: put_Mode(%ld) -> %08lx\n", mode, (unsigned long)hr);
    return SUCCEEDED(hr) ? FWDV_OK : FWDV_ERR_IO;
}

static int bcd(unsigned v) { return (int)((v >> 4 & 0x0f) * 10 + (v & 0x0f)); }

static int do_status(fwdv_t *d, fwdv_status_t *st)
{
    memset(st, 0, sizeof(*st));
    st->has_tape = -1;
    long mode = 0;
    if (d->transport && SUCCEEDED(IAMExtTransport_get_Mode(d->transport, &mode))) {
        switch (mode) {
        case ED_MODE_PLAY: st->mode = FWDV_MODE_PLAYING; break;
        case ED_MODE_FREEZE: st->mode = FWDV_MODE_PAUSED; break;
        case ED_MODE_STOP: st->mode = FWDV_MODE_STOPPED; break;
        case ED_MODE_REW: st->mode = FWDV_MODE_REWINDING; break;
        case ED_MODE_FF: st->mode = FWDV_MODE_FFWD; break;
        case ED_MODE_RECORD: st->mode = FWDV_MODE_RECORDING; break;
        default: st->mode = FWDV_MODE_UNKNOWN; break;
        }
    } else if (d->transport) {
        return d->lost ? FWDV_ERR_GONE : FWDV_ERR_IO;
    }
    /* MSDV does not say whether a cassette is in (has_tape stays -1) */
    if (d->tc) {
        TIMECODE_SAMPLE s;
        memset(&s, 0, sizeof(s));
        s.dwFlags = ED_DEVCAP_TIMECODE_READ;
        if (SUCCEEDED(IAMTimecodeReader_GetTimecode(d->tc, &s))) {
            DWORD f = s.timecode.dwFrames; /* BCD hhmmssff */
            st->tc_h = bcd(f >> 24 & 0xff), st->tc_m = bcd(f >> 16 & 0xff), st->tc_s = bcd(f >> 8 & 0xff),
            st->tc_f = bcd(f & 0xff);
            st->tc_valid = f != 0xffffffff && st->tc_h < 24 && st->tc_m < 60 && st->tc_s < 60;
        }
    }
    return FWDV_OK;
}

static int do_alive(fwdv_t *d)
{
    if (d->events) {
        long code;
        LONG_PTR p1, p2;
        while (IMediaEventEx_GetEvent(d->events, &code, &p1, &p2, 0) == S_OK) {
            if (code == EC_DEVICE_LOST && p2 == 0)
                d->lost = 1;
            if (code == EC_ERRORABORT)
                d->lost = 1;
            IMediaEventEx_FreeEventParams(d->events, code, p1, p2);
        }
    }
    return !d->lost;
}

static DWORD WINAPI com_thread(LPVOID arg)
{
    fwdv_t *d = arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    for (;;) {
        WaitForSingleObject(d->req_event, INFINITE);
        int r = 0;
        switch (d->req) {
        case REQ_OPEN: r = do_open(d); break;
        case REQ_START: r = do_start(d); break;
        case REQ_STOP: do_stop(d); break;
        case REQ_TRANSPORT: r = do_transport(d, (fwdv_cmd_t)d->arg); break;
        case REQ_STATUS: r = do_status(d, d->st); break;
        case REQ_ALIVE: r = do_alive(d); break;
        case REQ_QUIT: close_graph(d); break;
        }
        int quit = d->req == REQ_QUIT;
        d->result = r;
        SetEvent(d->done_event);
        if (quit)
            break;
    }
    CoUninitialize();
    return 0;
}

static int call(fwdv_t *d, int req, int arg, fwdv_status_t *st)
{
    EnterCriticalSection(&d->lock);
    d->req = req;
    d->arg = arg;
    d->st = st;
    SetEvent(d->req_event);
    WaitForSingleObject(d->done_event, INFINITE);
    int r = d->result;
    LeaveCriticalSection(&d->lock);
    return r;
}

static void destroy(fwdv_t *d)
{
    call(d, REQ_QUIT, 0, NULL);
    WaitForSingleObject(d->thread, INFINITE);
    CloseHandle(d->thread);
    CloseHandle(d->req_event);
    CloseHandle(d->done_event);
    DeleteCriticalSection(&d->lock);
    free(d);
}

int fwdv_open(fwdv_t **out)
{
    *out = NULL;
    fwdv_t *d = calloc(1, sizeof(*d));
    if (!d)
        return FWDV_ERR_IO;
    InitializeCriticalSection(&d->lock);
    d->req_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    d->done_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    d->thread = CreateThread(NULL, 0, com_thread, d, 0, NULL);
    if (!d->req_event || !d->done_event || !d->thread) {
        free(d);
        return FWDV_ERR_IO;
    }
    int r = call(d, REQ_OPEN, 0, NULL);
    if (r) {
        destroy(d);
        return r;
    }
    *out = d;
    return FWDV_OK;
}

void fwdv_close(fwdv_t *d)
{
    if (!d)
        return;
    fwdv_stop(d);
    destroy(d);
}

const char *fwdv_name(fwdv_t *d) { return d->name; }

int fwdv_start(fwdv_t *d, fwdv_frame_cb cb, void *user)
{
    d->cb = cb;
    d->user = user;
    g_dev = d;
    int r = call(d, REQ_START, 0, NULL);
    if (r)
        g_dev = NULL;
    return r;
}

void fwdv_stop(fwdv_t *d)
{
    call(d, REQ_STOP, 0, NULL);
    g_dev = NULL;
}

int fwdv_alive(fwdv_t *d) { return call(d, REQ_ALIVE, 0, NULL); }
int fwdv_transport(fwdv_t *d, fwdv_cmd_t cmd) { return call(d, REQ_TRANSPORT, (int)cmd, NULL); }
int fwdv_status(fwdv_t *d, fwdv_status_t *st) { return call(d, REQ_STATUS, 0, st); }
