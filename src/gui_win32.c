/*
 * FireWireRecorder for Windows: preview, tape control and recording for DV
 * camcorders and decks on FireWire, plain Win32 so it runs on any Windows 7+
 * PC (x64, x86, ARM64) without a runtime. The camcorder comes through
 * Windows' own DV driver (fwdv_win32.c).
 *
 * The bar under the preview runs the tape (rewind, play, pause, stop, fast
 * forward), picks the recording format (raw DV as on the tape, MPEG-2 or
 * x264) and folder, and starts/stops recording.
 */
#define COBJMACROS
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_core.h"

enum { TAPE_REW, TAPE_PLAY, TAPE_PAUSE, TAPE_STOP, TAPE_FF, TAPE_BUTTONS };
static const fwdv_cmd_t tape_cmds[TAPE_BUTTONS] = { FWDV_REWIND, FWDV_PLAY, FWDV_PAUSE, FWDV_STOP, FWDV_FFWD };

enum {
    ID_TAPE = 100, /* ID_TAPE + TAPE_* */
    ID_TIMECODE = 110, ID_TAPESTATE, ID_FORMAT, ID_AUTOSTOP, ID_PREVIEW, ID_FOLDER, ID_RECORD, ID_MONITOR, ID_TIME,
    ID_STATUS, ID_METER_L, ID_METER_R, ID_LBL_FORMAT, ID_LBL_SAVE, ID_LBL_L, ID_LBL_R, ID_LBL_STOP, ID_STOPAFTER,
    IDM_RECORD = 200, IDM_SNAPSHOT, IDM_CHOOSE_FOLDER, IDM_OPEN_FOLDER, IDM_EXIT, IDM_FULLSCREEN, IDM_ESCAPE,
    IDM_PREVIEW,
    IDM_TAPE = 220, /* IDM_TAPE + TAPE_* */
    TIMER_FRAME = 1, TIMER_UI = 2,
    WM_APP_RECDONE = WM_APP + 1,
};
#define NLABELS 5

static struct {
    HINSTANCE inst;
    HWND wnd, preview;
    HWND tape[TAPE_BUTTONS], timecode, tape_state, format, autostop, previewchk;
    HWND folder_btn, record, monitor, time, status, meter[2];
    HWND labels[NLABELS];
    HWND stopafter;
    HFONT font, time_font, badge_font, msg_font;
    HBRUSH bar_brush;
    HMENU menu;
    int dpi;
    app_t *app;
    uint32_t *frame;
    uint64_t seq;
    int w, h;
    char folder[1024];
    int finishing;
    double level[2];
    int fullscreen;
    WINDOWPLACEMENT placement;
    char message[300];   /* "Saved ..." for a few seconds */
    DWORD message_until;
} G;

static int S(int v) { return MulDiv(v, G.dpi, 96); }

static void utf8_to_w(const char *s, wchar_t *w, int len)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, w, len))
        w[0] = 0;
}

static void w_to_utf8(const wchar_t *w, char *s, int len)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, s, len, NULL, NULL))
        s[0] = 0;
}

static void set_text(HWND h, const char *utf8)
{
    wchar_t w[1200], old[1200];
    utf8_to_w(utf8, w, 1200);
    GetWindowTextW(h, old, 1200);
    if (wcscmp(old, w)) /* avoid flicker from the 20 Hz timer */
        SetWindowTextW(h, w);
}

static void show_message(const char *s)
{
    snprintf(G.message, sizeof(G.message), "%s", s);
    G.message_until = GetTickCount() + 6000;
}

static const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/')
            b = p + 1;
    return b;
}

static int sel(HWND combo) { return (int)SendMessageW(combo, CB_GETCURSEL, 0, 0); }
static int checked(HWND b) { return SendMessageW(b, BM_GETCHECK, 0, 0) == BST_CHECKED; }

static void update_folder_button(void)
{
    char label[300];
    snprintf(label, sizeof(label), "\xf0\x9f\x93\x81 %s", base_name(G.folder)[0] ? base_name(G.folder) : G.folder);
    set_text(G.folder_btn, label);
}

/* ---- preview ---- */

static RECT picture_rect(RECT c)
{
    int cw = c.right - c.left, ch = c.bottom - c.top;
    double ar = G.app ? app_aspect(G.app) : 4.0 / 3;
    int w = cw, h = (int)(cw / ar);
    if (h > ch) {
        h = ch;
        w = (int)(ch * ar);
    }
    RECT r = { c.left + (cw - w) / 2, c.top + (ch - h) / 2, 0, 0 };
    r.right = r.left + w;
    r.bottom = r.top + h;
    return r;
}

static LRESULT CALLBACK preview_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDBLCLK:
        SendMessageW(G.wnd, WM_COMMAND, IDM_FULLSCREEN, 0);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT c, r;
        GetClientRect(h, &c);
        r = picture_rect(c);
        HBRUSH black = GetStockObject(BLACK_BRUSH);
        RECT bars[4] = { { c.left, c.top, c.right, r.top }, { c.left, r.bottom, c.right, c.bottom },
                         { c.left, r.top, r.left, r.bottom }, { r.right, r.top, c.right, r.bottom } };
        for (int i = 0; i < 4; i++)
            FillRect(dc, &bars[i], black);
        if (G.frame) {
            BITMAPINFO bi = { 0 };
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = G.w;
            bi.bmiHeader.biHeight = -G.h; /* top-down */
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, NULL);
            StretchDIBits(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, 0, 0, G.w, G.h, G.frame, &bi,
                          DIB_RGB_COLORS, SRCCOPY);
        }
        SetBkMode(dc, TRANSPARENT);
        int streaming = G.app && app_state(G.app) == APP_STREAMING;
        if (G.app && (!streaming || !app_preview(G.app))) {
            char s[600];
            wchar_t w[600];
            if (streaming)
                snprintf(s, sizeof(s), "Preview off");
            else
                app_status(G.app, s, sizeof(s));
            utf8_to_w(s, w, 600);
            HFONT old = SelectObject(dc, G.msg_font);
            SetTextColor(dc, RGB(190, 190, 190));
            DrawTextW(dc, w, -1, &c, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, old);
        }
        if (G.app && app_is_recording(G.app)) {
            wchar_t badge[64];
            int s = (int)app_record_seconds(G.app);
            swprintf(badge, 64, L" \x25CF REC %02d:%02d:%02d ", s / 3600, s / 60 % 60, s % 60);
            HFONT old = SelectObject(dc, G.badge_font);
            RECT tr = { 0, 0, 0, 0 };
            DrawTextW(dc, badge, -1, &tr, DT_CALCRECT | DT_SINGLELINE);
            RECT br = { r.left + S(10), r.top + S(10), r.left + S(10) + tr.right + S(8), r.top + S(10) + S(22) };
            HBRUSH red = CreateSolidBrush(RGB(204, 13, 13));
            FillRect(dc, &br, red);
            DeleteObject(red);
            SetTextColor(dc, RGB(255, 255, 255));
            DrawTextW(dc, badge, -1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, old);
        }
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---- meters (owner drawn statics) ---- */

static void draw_meter(DRAWITEMSTRUCT *d)
{
    int c = d->CtlID == ID_METER_R;
    RECT r = d->rcItem;
    HBRUSH bg = CreateSolidBrush(RGB(60, 60, 60));
    FillRect(d->hDC, &r, bg);
    DeleteObject(bg);
    double v = G.level[c] / 60.0; /* dBFS + 60 */
    int w = (int)((r.right - r.left) * v);
    COLORREF col = G.level[c] > 57 ? RGB(230, 50, 40) : G.level[c] > 48 ? RGB(240, 200, 40) : RGB(60, 200, 80);
    RECT f = { r.left, r.top, r.left + w, r.bottom };
    HBRUSH b = CreateSolidBrush(col);
    FillRect(d->hDC, &f, b);
    DeleteObject(b);
}

/* ---- layout ---- */

#define BAR_H 120

static void layout(void)
{
    RECT c;
    GetClientRect(G.wnd, &c);
    if (G.fullscreen) {
        MoveWindow(G.preview, 0, 0, c.right, c.bottom, TRUE);
        return;
    }
    int bar = S(BAR_H), y1 = c.bottom - bar + S(8), y2 = c.bottom - bar + S(46), y3 = c.bottom - bar + S(82);
    MoveWindow(G.preview, 0, 0, c.right, c.bottom - bar, TRUE);
    int x = S(12), lh = S(20);
    for (int i = 0; i < TAPE_BUTTONS; i++) {
        MoveWindow(G.tape[i], x, y1, S(44), S(28), TRUE);
        x += S(44 + 4);
    }
    x += S(12);
    MoveWindow(G.timecode, x, y1 + S(4), S(110), lh + S(2), TRUE);
    x += S(110 + 12);
    MoveWindow(G.tape_state, x, y1 + S(6), c.right - x - S(12), lh, TRUE);

    x = S(12);
    struct { HWND h; int w, dy, hh, gap; } row2[] = {
        { G.labels[0], 50, 3, 20, 6 },   { G.format, 110, 0, 200, 16 }, { G.autostop, 150, 2, 20, 12 },
        { G.previewchk, 80, 2, 20, 16 }, { G.labels[1], 50, 3, 20, 6 }, { G.folder_btn, 180, -1, 26, 6 },
    };
    for (size_t i = 0; i < sizeof(row2) / sizeof(row2[0]); i++) {
        MoveWindow(row2[i].h, x, y2 + S(row2[i].dy), S(row2[i].w), S(row2[i].hh), TRUE);
        x += S(row2[i].w) + S(row2[i].gap);
    }
    x = S(12);
    MoveWindow(G.record, x, y3, S(110), S(28), TRUE);
    x += S(110 + 16);
    MoveWindow(G.time, x, y3 + S(3), S(84), lh + S(2), TRUE);
    x += S(84 + 12);
    MoveWindow(G.labels[2], x, y3, S(12), S(14), TRUE);
    MoveWindow(G.meter[0], x + S(14), y3 + S(4), S(100), S(8), TRUE);
    MoveWindow(G.labels[3], x, y3 + S(14), S(12), S(14), TRUE);
    MoveWindow(G.meter[1], x + S(14), y3 + S(18), S(100), S(8), TRUE);
    x += S(14 + 100 + 16);
    MoveWindow(G.monitor, x, y3 + S(4), S(100), lh, TRUE);
    x += S(100 + 12);
    MoveWindow(G.labels[4], x, y3 + S(6), S(62), lh, TRUE);
    x += S(62 + 4);
    MoveWindow(G.stopafter, x, y3 + S(2), S(100), S(200), TRUE);
    x += S(100 + 12);
    MoveWindow(G.status, x, y3 + S(6), c.right - x - S(12), lh, TRUE);
}

static HWND make(const wchar_t *cls, const wchar_t *text, DWORD style, int id)
{
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, G.wnd,
                             (HMENU)(INT_PTR)id, G.inst, NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)G.font, TRUE);
    return h;
}

static HWND make_combo(int id, const char *const *names, int n)
{
    HWND h = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, id);
    for (int i = 0; i < n; i++) {
        wchar_t w[100];
        utf8_to_w(names[i], w, 100);
        SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)w);
    }
    return h;
}

static void build_controls(void)
{
    static const wchar_t *const glyphs[TAPE_BUTTONS] = { L"\x25C0\x25C0", L"\x25B6", L"\x275A\x275A", L"\x25A0",
                                                         L"\x25B6\x25B6" };
    G.preview = CreateWindowExW(0, L"FireWirePreview", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, G.wnd, NULL,
                                G.inst, NULL);
    for (int i = 0; i < TAPE_BUTTONS; i++)
        G.tape[i] = make(L"BUTTON", glyphs[i], BS_PUSHBUTTON | WS_TABSTOP, ID_TAPE + i);
    G.timecode = make(L"STATIC", L"--:--:--.--", 0, ID_TIMECODE);
    SendMessageW(G.timecode, WM_SETFONT, (WPARAM)G.time_font, TRUE);
    G.tape_state = make(L"STATIC", L"", SS_ENDELLIPSIS | SS_NOPREFIX, ID_TAPESTATE);

    G.labels[0] = make(L"STATIC", L"Format", 0, ID_LBL_FORMAT);
    G.format = make_combo(ID_FORMAT, app_format_names, APP_FORMATS);
    G.autostop = make(L"BUTTON", L"Stop at end of tape", BS_AUTOCHECKBOX | WS_TABSTOP, ID_AUTOSTOP);
    G.previewchk = make(L"BUTTON", L"Preview", BS_AUTOCHECKBOX | WS_TABSTOP, ID_PREVIEW);
    G.labels[1] = make(L"STATIC", L"Save to", 0, ID_LBL_SAVE);
    G.folder_btn = make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, ID_FOLDER);

    G.record = make(L"BUTTON", L"\x25CF  Record", BS_PUSHBUTTON | WS_TABSTOP, ID_RECORD);
    G.time = make(L"STATIC", L"00:00:00", 0, ID_TIME);
    SendMessageW(G.time, WM_SETFONT, (WPARAM)G.time_font, TRUE);
    G.labels[2] = make(L"STATIC", L"L", 0, ID_LBL_L);
    G.meter[0] = make(L"STATIC", L"", SS_OWNERDRAW, ID_METER_L);
    G.labels[3] = make(L"STATIC", L"R", 0, ID_LBL_R);
    G.meter[1] = make(L"STATIC", L"", SS_OWNERDRAW, ID_METER_R);
    G.monitor = make(L"BUTTON", L"Play audio", BS_AUTOCHECKBOX | WS_TABSTOP, ID_MONITOR);
    G.labels[4] = make(L"STATIC", L"Stop after", 0, ID_LBL_STOP);
    G.stopafter = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, ID_STOPAFTER);
    for (int i = 0; i < APP_STOP_AFTERS; i++) {
        wchar_t wn[32];
        MultiByteToWideChar(CP_UTF8, 0, app_stop_after_names[i], -1, wn, 32);
        SendMessageW(G.stopafter, CB_ADDSTRING, 0, (LPARAM)wn);
    }
    G.status = make(L"STATIC", L"", SS_ENDELLIPSIS | SS_NOPREFIX, ID_STATUS);
}

static void build_menu(void)
{
    HMENU file = CreatePopupMenu(), tape = CreatePopupMenu(), view = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_RECORD, L"&Start Recording\tCtrl+R");
    AppendMenuW(file, MF_STRING, IDM_SNAPSHOT, L"Save &Snapshot\tCtrl+S");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_CHOOSE_FOLDER, L"Choose Recording &Folder\x2026");
    AppendMenuW(file, MF_STRING, IDM_OPEN_FOLDER, L"&Open Recordings Folder");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");
    AppendMenuW(tape, MF_STRING, IDM_TAPE + TAPE_PLAY, L"&Play\tCtrl+Enter");
    AppendMenuW(tape, MF_STRING, IDM_TAPE + TAPE_PAUSE, L"P&ause\tCtrl+P");
    AppendMenuW(tape, MF_STRING, IDM_TAPE + TAPE_STOP, L"&Stop\tCtrl+.");
    AppendMenuW(tape, MF_STRING, IDM_TAPE + TAPE_REW, L"&Rewind\tCtrl+Left");
    AppendMenuW(tape, MF_STRING, IDM_TAPE + TAPE_FF, L"&Fast Forward\tCtrl+Right");
    AppendMenuW(view, MF_STRING, IDM_PREVIEW, L"Show &Preview\tCtrl+E");
    AppendMenuW(view, MF_STRING, IDM_FULLSCREEN, L"&Full Screen\tF11");
    G.menu = CreateMenu();
    AppendMenuW(G.menu, MF_POPUP, (UINT_PTR)file, L"&File");
    AppendMenuW(G.menu, MF_POPUP, (UINT_PTR)tape, L"&Tape");
    AppendMenuW(G.menu, MF_POPUP, (UINT_PTR)view, L"&View");
    SetMenu(G.wnd, G.menu);
}

/* ---- actions ---- */

static void choose_folder(void)
{
    static const GUID clsid = { 0xdc1c5a9c, 0xe88a, 0x4dde, { 0xa5, 0xa1, 0x60, 0xf8, 0x2a, 0x20, 0xae, 0xf7 } };
    static const GUID iid = { 0xd57c7288, 0xd4ad, 0x4768, { 0xbe, 0x02, 0x9d, 0x96, 0x95, 0x32, 0xd9, 0x60 } };
    static const GUID iid_item = { 0x43826d1e, 0xe718, 0x42ee, { 0xbc, 0x55, 0xa1, 0xe2, 0x61, 0xc3, 0x7b, 0xfe } };
    IFileOpenDialog *dlg = NULL;
    if (FAILED(CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &iid, (void **)&dlg)))
        return;
    DWORD opts = 0;
    IFileOpenDialog_GetOptions(dlg, &opts);
    IFileOpenDialog_SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    IFileOpenDialog_SetTitle(dlg, L"Choose the recording folder");
    wchar_t wf[1024];
    IShellItem *start = NULL;
    utf8_to_w(G.folder, wf, 1024);
    if (SUCCEEDED(SHCreateItemFromParsingName(wf, NULL, &iid_item, (void **)&start))) {
        IFileOpenDialog_SetFolder(dlg, start);
        IShellItem_Release(start);
    }
    if (SUCCEEDED(IFileOpenDialog_Show(dlg, G.wnd))) {
        IShellItem *item = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResult(dlg, &item))) {
            PWSTR p = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &p))) {
                w_to_utf8(p, G.folder, sizeof(G.folder));
                CoTaskMemFree(p);
                app_set_setting_str("folder", G.folder);
                update_folder_button();
            }
            IShellItem_Release(item);
        }
    }
    IFileOpenDialog_Release(dlg);
}

static void open_folder(void)
{
    wchar_t w[1024];
    utf8_to_w(G.folder, w, 1024);
    CreateDirectoryW(w, NULL);
    ShellExecuteW(G.wnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}

static void reveal_file(const char *path)
{
    wchar_t w[1024];
    utf8_to_w(path, w, 1024);
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(w);
    if (pidl) {
        SHOpenFolderAndSelectItems(pidl, 0, NULL, 0);
        ILFree(pidl);
    }
}

static void update_enabled(void)
{
    int recording = app_is_recording(G.app) || G.finishing;
    EnableWindow(G.format, !recording);
    EnableWindow(G.folder_btn, !recording);
}

static void set_recording_ui(int recording)
{
    SetWindowTextW(G.record, recording ? L"\x25A0  Stop" : L"\x25CF  Record");
    ModifyMenuW(G.menu, IDM_RECORD, MF_BYCOMMAND | MF_STRING, IDM_RECORD,
                recording ? L"&Stop Recording\tCtrl+R" : L"&Start Recording\tCtrl+R");
    update_enabled();
}

static void rec_done(void *user, int ok, const char *path)
{
    (void)user;
    size_t n = strlen(path) + 1;
    char *copy = malloc(n);
    if (copy)
        memcpy(copy, path, n);
    PostMessageW(G.wnd, WM_APP_RECDONE, (WPARAM)ok, (LPARAM)copy);
}

static void stop_recording(void)
{
    G.finishing = 1;
    EnableWindow(G.record, FALSE);
    SetWindowTextW(G.record, L"Finishing\x2026");
    app_record_stop(G.app, rec_done, NULL);
}

static void toggle_record(void)
{
    if (G.finishing)
        return;
    if (app_is_recording(G.app)) {
        stop_recording();
        return;
    }
    char path[1100], err[300];
    if (app_record_start(G.app, G.folder, sel(G.format), path, sizeof(path), err, sizeof(err))) {
        wchar_t w[300];
        utf8_to_w(err, w, 300);
        MessageBoxW(G.wnd, w, L"Cannot start recording", MB_ICONWARNING);
        return;
    }
    set_recording_ui(1);
}

static void snapshot(void)
{
    char path[1100], err[300], msg[1300];
    if (app_snapshot(G.app, G.folder, path, sizeof(path), err, sizeof(err)))
        show_message(err);
    else {
        snprintf(msg, sizeof(msg), "Saved %s", base_name(path));
        show_message(msg);
    }
}

/* AV/C commands take a moment on the bus: sent from a thread of their own */
static DWORD WINAPI tape_thread(LPVOID arg)
{
    app_tape(G.app, (fwdv_cmd_t)(INT_PTR)arg);
    return 0;
}

static void tape(int i)
{
    if (i < 0 || i >= TAPE_BUTTONS)
        return;
    fwdv_cmd_t cmd = tape_cmds[i];
    fwdv_status_t st;
    if (cmd == FWDV_PLAY && app_tape_status(G.app, &st) == 0 && st.mode == FWDV_MODE_PLAYING)
        cmd = FWDV_PAUSE; /* Play while playing pauses, as on the camcorder */
    HANDLE t = CreateThread(NULL, 0, tape_thread, (LPVOID)(INT_PTR)cmd, 0, NULL);
    if (t)
        CloseHandle(t);
}

static void set_preview(int on)
{
    SendMessageW(G.previewchk, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    CheckMenuItem(G.menu, IDM_PREVIEW, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
    app_set_preview(G.app, on);
    app_set_setting_int("preview", on);
    if (!on && G.frame)
        memset(G.frame, 0, (size_t)G.w * G.h * 4);
    InvalidateRect(G.preview, NULL, FALSE);
}

static void toggle_fullscreen(void)
{
    DWORD style = (DWORD)GetWindowLongW(G.wnd, GWL_STYLE);
    HWND bar[] = { G.tape[0], G.tape[1], G.tape[2], G.tape[3], G.tape[4], G.timecode, G.tape_state, G.format,
                   G.autostop, G.previewchk, G.folder_btn, G.record, G.monitor, G.time, G.status, G.meter[0],
                   G.meter[1], G.labels[0], G.labels[1], G.labels[2], G.labels[3], G.labels[4], G.stopafter };
    G.fullscreen = !G.fullscreen;
    for (size_t i = 0; i < sizeof(bar) / sizeof(bar[0]); i++)
        ShowWindow(bar[i], G.fullscreen ? SW_HIDE : SW_SHOW);
    if (G.fullscreen) {
        MONITORINFO mi = { sizeof(mi) };
        G.placement.length = sizeof(G.placement);
        GetWindowPlacement(G.wnd, &G.placement);
        GetMonitorInfoW(MonitorFromWindow(G.wnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetMenu(G.wnd, NULL);
        SetWindowLongW(G.wnd, GWL_STYLE, (LONG)(style & ~WS_OVERLAPPEDWINDOW));
        SetWindowPos(G.wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(G.wnd, GWL_STYLE, (LONG)(style | WS_OVERLAPPEDWINDOW));
        SetMenu(G.wnd, G.menu);
        SetWindowPlacement(G.wnd, &G.placement);
        SetWindowPos(G.wnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    layout();
}

static void tick_ui(void)
{
    double db[2];
    app_peaks(G.app, db);
    for (int c = 0; c < 2; c++) {
        double v = db[c] + 60, decayed = G.level[c] - 1.5;
        G.level[c] = v > decayed ? v : decayed < 0 ? 0 : decayed;
        InvalidateRect(G.meter[c], NULL, FALSE);
    }
    app_state_t st = app_state(G.app);
    int rec = app_is_recording(G.app);
    EnableWindow(G.record, !G.finishing && (rec || st == APP_STREAMING));
    int connected = st == APP_STREAMING || st == APP_NO_SIGNAL;
    for (int i = 0; i < TAPE_BUTTONS; i++)
        EnableWindow(G.tape[i], connected);

    fwdv_status_t ts;
    char tc[32] = "--:--:--.--";
    const char *mode = "";
    if (connected && app_tape_status(G.app, &ts) == 0) {
        static const char *const modes[] = { "", "Stopped", "Playing", "Paused", "Rewinding",
                                             "Fast forwarding", "Reverse", "Recording", "Camera" };
        if (ts.tc_valid)
            snprintf(tc, sizeof(tc), "%02d:%02d:%02d.%02d", ts.tc_h, ts.tc_m, ts.tc_s, ts.tc_f);
        if ((unsigned)ts.mode < sizeof(modes) / sizeof(modes[0]))
            mode = modes[ts.mode];
        if (ts.has_tape == 0 && ts.mode != FWDV_MODE_CAMERA)
            mode = "No tape";
    }
    set_text(G.timecode, tc);
    set_text(G.tape_state, mode);
    if (rec && !G.finishing && (app_auto_stop_due(G.app) || app_stop_after_due(G.app)))
        stop_recording(); /* "Stop at end of tape" or "Stop after" */

    char status[700], t[64];
    app_status(G.app, status, sizeof(status));
    if (rec) {
        int s = (int)app_record_seconds(G.app);
        snprintf(t, sizeof(t), "%02d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
        set_text(G.time, t);
        char full[800];
        snprintf(full, sizeof(full), "Recording %s %.1f MB \xe2\x80\x94 %s", app_format_names[sel(G.format)],
                 app_record_bytes(G.app) / 1e6, status);
        set_text(G.status, full);
    } else {
        if (!G.finishing)
            set_text(G.time, "00:00:00");
        set_text(G.status, G.finishing ? "Finishing the file\xe2\x80\xa6"
                           : G.message[0] && (int)(G.message_until - GetTickCount()) > 0 ? G.message
                                                                                       : status);
    }

    char title[200], name[128];
    app_device_name(G.app, name, sizeof(name));
    if (name[0])
        snprintf(title, sizeof(title), APP_NAME " \xe2\x80\x94 %s", name);
    else
        snprintf(title, sizeof(title), APP_NAME);
    set_text(G.wnd, title);

    static app_state_t last = (app_state_t)-1;
    if (st != APP_STREAMING || st != last)
        InvalidateRect(G.preview, NULL, FALSE); /* the message on the black picture */
    last = st;

}

/* ---- main window ---- */

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        layout();
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)(void *)lp;
        mm->ptMinTrackSize.x = S(720);
        mm->ptMinTrackSize.y = S(420);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        for (int i = 0; i < NLABELS; i++)
            if ((HWND)lp == G.labels[i])
                SetTextColor((HDC)wp, GetSysColor(COLOR_GRAYTEXT));
        if ((HWND)lp == G.status)
            SetTextColor((HDC)wp, GetSysColor(COLOR_GRAYTEXT));
        SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)G.bar_brush;
    case WM_DRAWITEM:
        draw_meter((DRAWITEMSTRUCT *)(void *)lp);
        return TRUE;
    case WM_TIMER:
        if (wp == TIMER_FRAME) {
            int w, hh;
            app_frame_size(G.app, &w, &hh);
            if (w != G.w || hh != G.h) {
                free(G.frame);
                G.frame = calloc((size_t)w * hh, 4);
                G.w = w;
                G.h = hh;
                G.seq = 0;
            }
            if (G.frame && app_get_frame(G.app, G.frame, G.w, G.h, &G.seq))
                InvalidateRect(G.preview, NULL, FALSE);
        } else {
            tick_ui();
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_FORMAT:
            if (HIWORD(wp) == CBN_SELCHANGE)
                app_set_setting_int("format", sel(G.format));
            return 0;
        case ID_AUTOSTOP: {
            int on = checked(G.autostop);
            app_set_auto_stop(G.app, on);
            app_set_setting_int("autostop", on);
            return 0;
        }
        case ID_PREVIEW:
            set_preview(checked(G.previewchk));
            return 0;
        case IDM_PREVIEW:
            set_preview(!app_preview(G.app));
            return 0;
        case ID_STOPAFTER:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int i = (int)SendMessageW(G.stopafter, CB_GETCURSEL, 0, 0);
                app_set_stop_after(G.app, i);
                app_set_setting_int("stop_after", i);
            }
            return 0;
        case ID_MONITOR: {
            int on = checked(G.monitor);
            app_set_monitor(G.app, on);
            app_set_setting_int("monitor", on);
            return 0;
        }
        case ID_FOLDER:
        case IDM_CHOOSE_FOLDER:
            if (!app_is_recording(G.app))
                choose_folder();
            return 0;
        case ID_RECORD:
        case IDM_RECORD:
            toggle_record();
            return 0;
        case IDM_SNAPSHOT:
            snapshot();
            return 0;
        case IDM_OPEN_FOLDER:
            open_folder();
            return 0;
        case IDM_FULLSCREEN:
            toggle_fullscreen();
            return 0;
        case IDM_ESCAPE:
            if (G.fullscreen)
                toggle_fullscreen();
            return 0;
        case IDM_EXIT:
            PostMessageW(h, WM_CLOSE, 0, 0);
            return 0;
        }
        if (LOWORD(wp) >= ID_TAPE && LOWORD(wp) < ID_TAPE + TAPE_BUTTONS) {
            tape(LOWORD(wp) - ID_TAPE);
            return 0;
        }
        if (LOWORD(wp) >= IDM_TAPE && LOWORD(wp) < IDM_TAPE + TAPE_BUTTONS) {
            tape(LOWORD(wp) - IDM_TAPE);
            return 0;
        }
        break;
    case WM_APP_RECDONE: {
        char *path = (char *)lp, msg2[1300];
        G.finishing = 0;
        set_recording_ui(0);
        if (wp && path) {
            snprintf(msg2, sizeof(msg2), "Saved %s", base_name(path));
            show_message(msg2);
            reveal_file(path);
        } else {
            show_message("Recording failed: the file could not be written");
        }
        free(path);
        InvalidateRect(G.preview, NULL, FALSE);
        return 0;
    }
    case WM_CLOSE:
        if (app_is_recording(G.app) &&
            MessageBoxW(h, L"Stop the recording and quit?", L"A recording is in progress",
                        MB_ICONQUESTION | MB_OKCANCEL) != IDOK)
            return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    (void)prev, (void)cmdline;
    G.inst = inst;
    SetProcessDPIAware();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    HDC sdc = GetDC(NULL);
    G.dpi = GetDeviceCaps(sdc, LOGPIXELSY);
    ReleaseDC(NULL, sdc);

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) {
        if (!wcscmp(argv[i], L"-v")) fwdv_set_verbose(1);
        if (!wcscmp(argv[i], L"-vv")) fwdv_set_verbose(2);
    }
    LocalFree(argv);

    char def[1024];
    app_default_folder(def, sizeof(def));
    app_setting_str("folder", G.folder, sizeof(G.folder), def);

    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    G.font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = -S(17);
    lf.lfWeight = FW_MEDIUM;
    wcscpy(lf.lfFaceName, L"Consolas");
    G.time_font = CreateFontIndirectW(&lf);
    lf = ncm.lfMessageFont;
    lf.lfHeight = -S(14);
    lf.lfWeight = FW_SEMIBOLD;
    G.badge_font = CreateFontIndirectW(&lf);
    lf = ncm.lfMessageFont;
    lf.lfHeight = -S(16);
    G.msg_font = CreateFontIndirectW(&lf);
    G.bar_brush = GetSysColorBrush(COLOR_BTNFACE);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = preview_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.style = CS_DBLCLKS;
    wc.lpszClassName = L"FireWirePreview";
    RegisterClassExW(&wc);

    wc.lpfnWndProc = wnd_proc;
    wc.style = 0;
    wc.hbrBackground = G.bar_brush;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = L"FireWireRecorder";
    RegisterClassExW(&wc);

    RECT r = { 0, 0, S(800), S(600 + BAR_H) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, TRUE);
    {
        /* short screens: stay inside the work area, clear of the taskbar */
        RECT wa;
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
            if (r.bottom - r.top > wa.bottom - wa.top)
                r.bottom = r.top + (wa.bottom - wa.top);
            if (r.right - r.left > wa.right - wa.left)
                r.right = r.left + (wa.right - wa.left);
        }
    }
    G.wnd = CreateWindowExW(0, L"FireWireRecorder", L"FireWireRecorder", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst,
                            NULL);
    build_menu();
    build_controls();

    int format = app_setting_int("format", 0), autostop = app_setting_int("autostop", 1);
    int preview = app_setting_int("preview", 1), monitor = app_setting_int("monitor", 1);
    format = format < 0 || format >= APP_FORMATS ? 0 : format;
    SendMessageW(G.format, CB_SETCURSEL, format, 0);
    SendMessageW(G.autostop, BM_SETCHECK, autostop ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(G.monitor, BM_SETCHECK, monitor ? BST_CHECKED : BST_UNCHECKED, 0);

    G.app = app_start();
    if (!G.app) {
        MessageBoxW(NULL, L"Out of memory", L"FireWireRecorder", MB_ICONERROR);
        return 1;
    }
    {
        int sa = app_setting_int("stop_after", 0);
        sa = sa < 0 || sa >= APP_STOP_AFTERS ? 0 : sa;
        SendMessageW(G.stopafter, CB_SETCURSEL, sa, 0);
        app_set_stop_after(G.app, sa);
    }
    app_set_monitor(G.app, monitor);
    app_set_auto_stop(G.app, autostop);
    set_preview(preview);
    update_folder_button();
    update_enabled();
    layout();

    ACCEL acc[] = { { FCONTROL | FVIRTKEY, 'R', IDM_RECORD },
                    { FCONTROL | FVIRTKEY, 'S', IDM_SNAPSHOT },
                    { FCONTROL | FVIRTKEY, 'E', IDM_PREVIEW },
                    { FCONTROL | FVIRTKEY, VK_RETURN, IDM_TAPE + TAPE_PLAY },
                    { FCONTROL | FVIRTKEY, 'P', IDM_TAPE + TAPE_PAUSE },
                    { FCONTROL | FVIRTKEY, VK_OEM_PERIOD, IDM_TAPE + TAPE_STOP },
                    { FCONTROL | FVIRTKEY, VK_LEFT, IDM_TAPE + TAPE_REW },
                    { FCONTROL | FVIRTKEY, VK_RIGHT, IDM_TAPE + TAPE_FF },
                    { FVIRTKEY, VK_F11, IDM_FULLSCREEN },
                    { FVIRTKEY, VK_ESCAPE, IDM_ESCAPE } };
    HACCEL accel = CreateAcceleratorTableW(acc, sizeof(acc) / sizeof(acc[0]));

    ShowWindow(G.wnd, show);
    SetTimer(G.wnd, TIMER_FRAME, 15, NULL);
    SetTimer(G.wnd, TIMER_UI, 50, NULL);
    tick_ui();

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!TranslateAcceleratorW(G.wnd, accel, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    app_close(G.app); /* finishes a recording still in progress */
    CoUninitialize();
    return 0;
}
