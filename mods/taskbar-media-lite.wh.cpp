// ==WindhawkMod==
// @id              taskbar-media-lite
// @name            Taskbar Media Lite
// @description     Lean now-playing + play/pause/next/prev in the taskbar via raw-COM SystemMediaTransportControls (no WinRT headers)
// @version         1.0
// @author          crtGhoul
// @github          https://github.com/crtGhoul
// @license         MIT
// @include         explorer.exe
// @compilerOptions -lole32 -lruntimeobject -lgdi32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Taskbar Media Lite

A lean, from-scratch now-playing widget drawn natively in the taskbar: track
title plus Prev / Play-Pause / Next buttons, driven by the system's
`SystemMediaTransportControls` session. No WinRT headers are used anywhere —
all COM interfaces are hand-rolled from the documented IDL layout, and the
only hardcoded GUID is the session-manager statics IID transcribed from the
Windows SDK (see "Honest limitation").

This is explicitly NOT an "abandoned software rewrite": `taskbar-fluent-media-
player` is maintained (v1.6.0, active) — this mod is a lean from-scratch
alternative for people who want a tiny fraction of its 10,280 lines. It is
also written to avoid that mod's still-open 26H2 tray bug (#5050, where the
system-tray container changed from Grid to StackPanel): this widget is a plain
Win32 child window of `Shell_TrayWnd`, positioned next to the notification
area, with no dependency on internal XAML structure.

- UI: a GDI-drawn (-lgdi32) child toolbar on the taskbar showing the current
  track title and three buttons. Hidden when no media session exists.
- Control: buttons call `TryTogglePlayPauseAsync` / `TrySkipNextAsync` /
  `TrySkipPreviousAsync` on the current session (async ops are polled to
  completion with a bounded wait — no event-handler plumbing needed for
  clicks).
- Refresh: best-effort WinRT event subscriptions (`CurrentSessionChanged`,
  `PlaybackInfoChanged`, `MediaPropertiesChanged`) trigger an immediate
  refresh; additionally a `WM_TIMER` ticks at `updateIntervalMs` ONLY while
  `PlaybackStatus == Playing` (killed when paused/stopped). Button clicks
  refresh immediately too.
- COM lives on a dedicated worker thread (`CoInitializeEx` STA there,
  `CoUninitialize` on unload); the taskbar subclass that repositions the
  widget only calls `SetWindowPos` and never touches COM.

## Settings

- **Enabled** — master switch.
- **Show buttons** — show the Prev/Play/Next buttons (title-only if off).
- **Max title chars** — title truncation length (10–200).
- **Update interval ms** — timer cadence while playing; minimum 1000,
  enforced in code.

## Honest limitation

Raw-COM WinRT work transcribed without a Windows machine: the statics IID
`94F98E18-BFB6-5E86-A4AD-CE56DEFE7EBE`
(`IGlobalSystemMediaTransportControlsSessionManagerStatics`, per
`windows.media.control.h`) and the interface vtable slot orders (taken from
the SDK IDL declaration order) MUST be validated on real Windows. If the IID
is wrong, `RoGetActivationFactory` fails and the widget simply never appears
(fail-safe, nothing crashes). The event-sink object uses a deliberately
permissive `QueryInterface` (accepts the unknown per-instantiation
`ITypedEventHandler` IID and exposes `Invoke` at the standard slot) — a
contained hack, documented here. When paused, the timer is off by design, so
an external pause/play (e.g. media keys) may leave the button glyph stale
until the next event or click. Theming follows system colors and may not match
a dark taskbar perfectly.

## Testing status

Type-checked only with x86_64-w64-mingw32-g++ -fsyntax-only against the real
windhawk_api.h (Windhawk v1.7.3); NOT compiled with Windhawk's real compiler;
NOT tested on a real Windows machine. Real-Windows verification needed: the
hardcoded IID, every vtable slot, event delivery, button actions against a
real player (e.g. browser media), and widget positioning across taskbar
layouts.
*/
// ==WindhawkModSettings==
/*
- enabled: true
  $name: Enabled
- showButtons: true
  $name: Show buttons
- maxTitleChars: 40
  $name: Max title chars
  $description: Title truncation length, 10-200
- updateIntervalMs: 1000
  $name: Update interval ms
  $description: Min 1000; only ticks while playing
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <windowsx.h>
#include <roapi.h>
#include <winstring.h>

#include <string>

namespace {

// ---------------------------------------------------------------------------
// Hand-rolled raw-COM WinRT surface for
// Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager.
// Slot orders follow the Windows SDK windows.media.control.idl declaration
// order. The ONE hardcoded IID is IGlobalSystemMediaTransportControlsSession-
// ManagerStatics, transcribed from the SDK (C++/WinRT
// guid<IGlobalSystemMediaTransportControlsSessionManagerStatics>):
// {94F98E18-BFB6-5E86-A4AD-CE56DEFE7EBE}. Validate on real Windows.
// ---------------------------------------------------------------------------

enum class AsyncStatus : int { Started = 0, Completed = 1, Canceled = 2, Error = 3 };
enum class TrustLevel : int { BaseTrust = 0, PartialTrust = 1, FullTrust = 2 };

struct EventToken {
    __int64 value;
};

struct AsyncOpBool;
struct AsyncOpObj;
struct GsmtcStatics;
struct GsmtcManager;
struct GsmtcSession;
struct GsmtcPlaybackInfo;
struct GsmtcMediaProps;
struct EventSink;

#define IUNKNOWN_METHODS(T)                                                    \
    HRESULT(STDMETHODCALLTYPE* QueryInterface)(T*, REFIID, void**);             \
    ULONG(STDMETHODCALLTYPE* AddRef)(T*);                                       \
    ULONG(STDMETHODCALLTYPE* Release)(T*)

#define IINSPECTABLE_METHODS(T)                                                \
    IUNKNOWN_METHODS(T);                                                       \
    HRESULT(STDMETHODCALLTYPE* GetIids)(T*, ULONG*, IID**);                     \
    HRESULT(STDMETHODCALLTYPE* GetRuntimeClassName)(T*, HSTRING*);              \
    HRESULT(STDMETHODCALLTYPE* GetTrustLevel)(T*, TrustLevel*)

struct AsyncOpBoolVtbl {
    IINSPECTABLE_METHODS(AsyncOpBool);
    HRESULT(STDMETHODCALLTYPE* put_Completed)(AsyncOpBool*, void*);
    HRESULT(STDMETHODCALLTYPE* get_Completed)(AsyncOpBool*, void**);
    HRESULT(STDMETHODCALLTYPE* GetResults)(AsyncOpBool*, boolean*);
    HRESULT(STDMETHODCALLTYPE* get_Id)(AsyncOpBool*, unsigned*);
    HRESULT(STDMETHODCALLTYPE* get_Status)(AsyncOpBool*, AsyncStatus*);
    HRESULT(STDMETHODCALLTYPE* get_ErrorCode)(AsyncOpBool*, HRESULT*);
    HRESULT(STDMETHODCALLTYPE* Cancel)(AsyncOpBool*);
    HRESULT(STDMETHODCALLTYPE* Close)(AsyncOpBool*);
};
struct AsyncOpBool {
    AsyncOpBoolVtbl* lpVtbl;
};

struct AsyncOpObjVtbl {
    IINSPECTABLE_METHODS(AsyncOpObj);
    HRESULT(STDMETHODCALLTYPE* put_Completed)(AsyncOpObj*, void*);
    HRESULT(STDMETHODCALLTYPE* get_Completed)(AsyncOpObj*, void**);
    HRESULT(STDMETHODCALLTYPE* GetResults)(AsyncOpObj*, void**);
    HRESULT(STDMETHODCALLTYPE* get_Id)(AsyncOpObj*, unsigned*);
    HRESULT(STDMETHODCALLTYPE* get_Status)(AsyncOpObj*, AsyncStatus*);
    HRESULT(STDMETHODCALLTYPE* get_ErrorCode)(AsyncOpObj*, HRESULT*);
    HRESULT(STDMETHODCALLTYPE* Cancel)(AsyncOpObj*);
    HRESULT(STDMETHODCALLTYPE* Close)(AsyncOpObj*);
};
struct AsyncOpObj {
    AsyncOpObjVtbl* lpVtbl;
};

struct GsmtcStaticsVtbl {
    IINSPECTABLE_METHODS(GsmtcStatics);
    HRESULT(STDMETHODCALLTYPE* RequestAsync)(GsmtcStatics*, AsyncOpObj**);
};
struct GsmtcStatics {
    GsmtcStaticsVtbl* lpVtbl;
};

struct GsmtcManagerVtbl {
    IINSPECTABLE_METHODS(GsmtcManager);
    HRESULT(STDMETHODCALLTYPE* GetCurrentSession)(GsmtcManager*, GsmtcSession**);
    HRESULT(STDMETHODCALLTYPE* GetSessions)(GsmtcManager*, void**);
    HRESULT(STDMETHODCALLTYPE* add_CurrentSessionChanged)(GsmtcManager*, EventSink*, EventToken*);
    HRESULT(STDMETHODCALLTYPE* remove_CurrentSessionChanged)(GsmtcManager*, EventToken);
    HRESULT(STDMETHODCALLTYPE* add_SessionsChanged)(GsmtcManager*, EventSink*, EventToken*);
    HRESULT(STDMETHODCALLTYPE* remove_SessionsChanged)(GsmtcManager*, EventToken);
};
struct GsmtcManager {
    GsmtcManagerVtbl* lpVtbl;
};

struct GsmtcSessionVtbl {
    IINSPECTABLE_METHODS(GsmtcSession);
    HRESULT(STDMETHODCALLTYPE* get_SourceAppId)(GsmtcSession*, HSTRING*);
    HRESULT(STDMETHODCALLTYPE* add_MediaPropertiesChanged)(GsmtcSession*, EventSink*, EventToken*);
    HRESULT(STDMETHODCALLTYPE* remove_MediaPropertiesChanged)(GsmtcSession*, EventToken);
    HRESULT(STDMETHODCALLTYPE* add_PlaybackInfoChanged)(GsmtcSession*, EventSink*, EventToken*);
    HRESULT(STDMETHODCALLTYPE* remove_PlaybackInfoChanged)(GsmtcSession*, EventToken);
    HRESULT(STDMETHODCALLTYPE* add_TimelinePropertiesChanged)(GsmtcSession*, EventSink*, EventToken*);
    HRESULT(STDMETHODCALLTYPE* remove_TimelinePropertiesChanged)(GsmtcSession*, EventToken);
    HRESULT(STDMETHODCALLTYPE* GetMediaPropertiesAsync)(GsmtcSession*, AsyncOpObj**);
    HRESULT(STDMETHODCALLTYPE* GetTimelineProperties)(GsmtcSession*, void**);
    HRESULT(STDMETHODCALLTYPE* GetPlaybackInfo)(GsmtcSession*, GsmtcPlaybackInfo**);
    HRESULT(STDMETHODCALLTYPE* TryTogglePlayPauseAsync)(GsmtcSession*, AsyncOpBool**);
    HRESULT(STDMETHODCALLTYPE* TryStopAsync)(GsmtcSession*, AsyncOpBool**);
    HRESULT(STDMETHODCALLTYPE* TryPlayAsync)(GsmtcSession*, AsyncOpBool**);
    HRESULT(STDMETHODCALLTYPE* TryPauseAsync)(GsmtcSession*, AsyncOpBool**);
    HRESULT(STDMETHODCALLTYPE* TryFastForwardAsync)(GsmtcSession*, AsyncOpBool**);
    HRESULT(STDMETHODCALLTYPE* TryRewindAsync)(GsmtcSession*, AsyncOpBool**);
    HRESULT(STDMETHODCALLTYPE* TrySkipNextAsync)(GsmtcSession*, AsyncOpBool**);
    HRESULT(STDMETHODCALLTYPE* TrySkipPreviousAsync)(GsmtcSession*, AsyncOpBool**);
};
struct GsmtcSession {
    GsmtcSessionVtbl* lpVtbl;
};

struct GsmtcPlaybackInfoVtbl {
    IINSPECTABLE_METHODS(GsmtcPlaybackInfo);
    // PlaybackStatus enum: 0 Closed, 1 Changing, 2 Stopped, 3 Playing, 4 Paused.
    HRESULT(STDMETHODCALLTYPE* get_PlaybackStatus)(GsmtcPlaybackInfo*, int*);
};
struct GsmtcPlaybackInfo {
    GsmtcPlaybackInfoVtbl* lpVtbl;
};

struct GsmtcMediaPropsVtbl {
    IINSPECTABLE_METHODS(GsmtcMediaProps);
    HRESULT(STDMETHODCALLTYPE* get_Title)(GsmtcMediaProps*, HSTRING*);
};
struct GsmtcMediaProps {
    GsmtcMediaPropsVtbl* lpVtbl;
};

static const GUID IID_GSMTC_STATICS = {
    0x94F98E18, 0xBFB6, 0x5E86, {0xA4, 0xAD, 0xCE, 0x56, 0xDE, 0xFE, 0x7E, 0xBE}};

// ---------------------------------------------------------------------------
// Event sink with a deliberately permissive QueryInterface (see readme).
// ---------------------------------------------------------------------------

struct EventSink {
    struct Vtbl {
        HRESULT(STDMETHODCALLTYPE* QueryInterface)(EventSink*, REFIID, void**);
        ULONG(STDMETHODCALLTYPE* AddRef)(EventSink*);
        ULONG(STDMETHODCALLTYPE* Release)(EventSink*);
        HRESULT(STDMETHODCALLTYPE* GetIids)(EventSink*, ULONG*, IID**);
        HRESULT(STDMETHODCALLTYPE* GetRuntimeClassName)(EventSink*, HSTRING*);
        HRESULT(STDMETHODCALLTYPE* GetTrustLevel)(EventSink*, TrustLevel*);
        HRESULT(STDMETHODCALLTYPE* Invoke)(EventSink*, void*, void*);
    };
    Vtbl* lpVtbl;
    LONG ref;
    HWND hwndNotify;
};

constexpr UINT WM_APP_MEDIA_EVENT = WM_APP + 101;
constexpr UINT WM_APP_SHUTDOWN = WM_APP + 102;

HRESULT STDMETHODCALLTYPE Sink_QueryInterface(EventSink* self, REFIID riid,
                                              void** ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    *ppv = nullptr;
    // Accept IUnknown/IInspectable normally; accept ANY other IID too: the
    // WinRT runtime QIs event sinks for the concrete per-instantiation
    // ITypedEventHandler IID (not hardcoded here), then only ever calls
    // Invoke at the standard vtable slot. Contained hack, documented.
    (void)riid;
    self->lpVtbl->AddRef(self);
    *ppv = self;
    return S_OK;
}

ULONG STDMETHODCALLTYPE Sink_AddRef(EventSink* self) {
    return static_cast<ULONG>(InterlockedIncrement(&self->ref));
}

ULONG STDMETHODCALLTYPE Sink_Release(EventSink* self) {
    LONG r = InterlockedDecrement(&self->ref);
    if (r == 0) {
        delete self;
    }
    return static_cast<ULONG>(r);
}

HRESULT STDMETHODCALLTYPE Sink_GetIids(EventSink*, ULONG* count, IID** iids) {
    if (count) {
        *count = 0;
    }
    if (iids) {
        *iids = nullptr;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Sink_GetRuntimeClassName(EventSink*, HSTRING* name) {
    if (name) {
        *name = nullptr;
    }
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE Sink_GetTrustLevel(EventSink* self, TrustLevel* level) {
    (void)self;
    if (level) {
        *level = TrustLevel::BaseTrust;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Sink_Invoke(EventSink* self, void*, void*) {
    if (self->hwndNotify) {
        PostMessageW(self->hwndNotify, WM_APP_MEDIA_EVENT, 0, 0);
    }
    return S_OK;
}

EventSink::Vtbl g_sinkVtbl = {
    &Sink_QueryInterface, &Sink_AddRef,        &Sink_Release,
    &Sink_GetIids,        &Sink_GetRuntimeClassName, &Sink_GetTrustLevel,
    &Sink_Invoke,
};

// ---------------------------------------------------------------------------
// Mod state (all COM objects live on the worker thread).
// ---------------------------------------------------------------------------

constexpr UINT_PTR kTimerId = 1;
constexpr int kPlaybackPlaying = 3;

HANDLE g_hThread = nullptr;
DWORD g_threadId = 0;
HWND g_hwndBar = nullptr;
HWND g_hwndTray = nullptr;
WNDPROC g_trayOldProc = nullptr;
bool g_shuttingDown = false;

GsmtcManager* g_pManager = nullptr;
GsmtcSession* g_pSession = nullptr;
EventSink* g_pSink = nullptr;
EventToken g_tokSessionChanged{};
bool g_tokSessionChangedValid = false;
EventToken g_tokMediaProps{};
bool g_tokMediaPropsValid = false;
EventToken g_tokPlayback{};
bool g_tokPlaybackValid = false;

bool g_enabled = true;
bool g_showButtons = true;
int g_maxTitleChars = 40;
int g_updateIntervalMs = 1000;

bool g_hasSession = false;
int g_playbackStatus = 0;
std::wstring g_title;
bool g_timerOn = false;
RECT g_btnRects[3]{};

bool WaitForAsyncObj(AsyncOpObj* op, DWORD timeoutMs) {
    DWORD waited = 0;
    while (waited < timeoutMs) {
        AsyncStatus st = AsyncStatus::Started;
        if (FAILED(op->lpVtbl->get_Status(op, &st))) {
            return false;
        }
        if (st == AsyncStatus::Completed) {
            return true;
        }
        if (st == AsyncStatus::Canceled || st == AsyncStatus::Error) {
            return false;
        }
        Sleep(10);
        waited += 10;
    }
    return false;
}

bool WaitForAsyncBool(AsyncOpBool* op, DWORD timeoutMs) {
    DWORD waited = 0;
    while (waited < timeoutMs) {
        AsyncStatus st = AsyncStatus::Started;
        if (FAILED(op->lpVtbl->get_Status(op, &st))) {
            return false;
        }
        if (st == AsyncStatus::Completed) {
            return true;
        }
        if (st == AsyncStatus::Canceled || st == AsyncStatus::Error) {
            return false;
        }
        Sleep(10);
        waited += 10;
    }
    return false;
}

void UnsubscribeSessionEvents() {
    if (g_pSession) {
        if (g_tokMediaPropsValid) {
            g_pSession->lpVtbl->remove_MediaPropertiesChanged(g_pSession,
                                                             g_tokMediaProps);
            g_tokMediaPropsValid = false;
        }
        if (g_tokPlaybackValid) {
            g_pSession->lpVtbl->remove_PlaybackInfoChanged(g_pSession,
                                                          g_tokPlayback);
            g_tokPlaybackValid = false;
        }
    }
}

void SubscribeSessionEvents() {
    if (!g_pSession || !g_pSink || g_shuttingDown) {
        return;
    }
    EventToken tok{};
    if (SUCCEEDED(g_pSession->lpVtbl->add_MediaPropertiesChanged(
            g_pSession, g_pSink, &tok))) {
        g_tokMediaProps = tok;
        g_tokMediaPropsValid = true;
    }
    if (SUCCEEDED(g_pSession->lpVtbl->add_PlaybackInfoChanged(g_pSession,
                                                             g_pSink, &tok))) {
        g_tokPlayback = tok;
        g_tokPlaybackValid = true;
    }
}

void EnsureManager() {
    if (g_pManager || g_shuttingDown) {
        return;
    }
    const wchar_t* kClass =
        L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
    HSTRING hClass = nullptr;
    if (FAILED(WindowsCreateString(kClass,
                                   static_cast<UINT32>(wcslen(kClass)),
                                   &hClass))) {
        return;
    }
    void* factory = nullptr;
    HRESULT hr = RoGetActivationFactory(hClass, IID_GSMTC_STATICS, &factory);
    WindowsDeleteString(hClass);
    if (FAILED(hr) || !factory) {
        Wh_Log(L"RoGetActivationFactory failed: hr=0x%08X", hr);
        return;
    }
    GsmtcStatics* statics = static_cast<GsmtcStatics*>(factory);
    AsyncOpObj* op = nullptr;
    hr = statics->lpVtbl->RequestAsync(statics, &op);
    statics->lpVtbl->Release(statics);
    if (FAILED(hr) || !op) {
        return;
    }
    bool done = WaitForAsyncObj(op, 2000);
    GsmtcManager* mgr = nullptr;
    if (done) {
        op->lpVtbl->GetResults(op, reinterpret_cast<void**>(&mgr));
    }
    op->lpVtbl->Release(op);
    if (!mgr) {
        return;
    }
    g_pManager = mgr;
    if (g_pSink) {
        EventToken tok{};
        if (SUCCEEDED(mgr->lpVtbl->add_CurrentSessionChanged(mgr, g_pSink,
                                                            &tok))) {
            g_tokSessionChanged = tok;
            g_tokSessionChangedValid = true;
        }
    }
}

void RefreshMediaState() {
    if (g_shuttingDown) {
        return;
    }
    EnsureManager();
    GsmtcSession* fresh = nullptr;
    if (g_pManager) {
        if (FAILED(g_pManager->lpVtbl->GetCurrentSession(g_pManager, &fresh))) {
            fresh = nullptr;
        }
    }
    if (fresh != g_pSession) {
        UnsubscribeSessionEvents();
        if (g_pSession) {
            g_pSession->lpVtbl->Release(g_pSession);
        }
        g_pSession = fresh;
        SubscribeSessionEvents();
    } else if (fresh) {
        fresh->lpVtbl->Release(fresh);  // keep the cached reference
    }

    g_hasSession = (g_pSession != nullptr);
    g_playbackStatus = 0;
    g_title.clear();

    if (g_pSession) {
        GsmtcPlaybackInfo* pi = nullptr;
        if (SUCCEEDED(g_pSession->lpVtbl->GetPlaybackInfo(g_pSession, &pi)) &&
            pi) {
            pi->lpVtbl->get_PlaybackStatus(pi, &g_playbackStatus);
            pi->lpVtbl->Release(pi);
        }
        AsyncOpObj* op = nullptr;
        if (SUCCEEDED(g_pSession->lpVtbl->GetMediaPropertiesAsync(g_pSession,
                                                                 &op)) &&
            op) {
            if (WaitForAsyncObj(op, 1500)) {
                GsmtcMediaProps* mp = nullptr;
                if (SUCCEEDED(op->lpVtbl->GetResults(
                        op, reinterpret_cast<void**>(&mp))) &&
                    mp) {
                    HSTRING ht = nullptr;
                    if (SUCCEEDED(mp->lpVtbl->get_Title(mp, &ht)) && ht) {
                        UINT32 len = 0;
                        PCWSTR raw = WindowsGetStringRawBuffer(ht, &len);
                        if (raw && len > 0) {
                            UINT32 take = len >
                                            static_cast<UINT32>(g_maxTitleChars)
                                              ? static_cast<UINT32>(
                                                    g_maxTitleChars)
                                              : len;
                            g_title.assign(raw, take);
                            if (len >
                                static_cast<UINT32>(g_maxTitleChars)) {
                                g_title += L"...";
                            }
                        }
                        WindowsDeleteString(ht);
                    }
                    mp->lpVtbl->Release(mp);
                }
            }
            op->lpVtbl->Release(op);
        }
    }
}

void FireTransport(int which) {
    // 0 = toggle play/pause, 1 = next, 2 = previous.
    if (!g_pSession || g_shuttingDown) {
        return;
    }
    AsyncOpBool* op = nullptr;
    HRESULT hr = E_FAIL;
    switch (which) {
        case 0:
            hr = g_pSession->lpVtbl->TryTogglePlayPauseAsync(g_pSession, &op);
            break;
        case 1:
            hr = g_pSession->lpVtbl->TrySkipNextAsync(g_pSession, &op);
            break;
        case 2:
            hr = g_pSession->lpVtbl->TrySkipPreviousAsync(g_pSession, &op);
            break;
        default:
            break;
    }
    if (SUCCEEDED(hr) && op) {
        if (WaitForAsyncBool(op, 2000)) {
            boolean r = false;
            op->lpVtbl->GetResults(op, &r);
        }
        op->lpVtbl->Release(op);
    }
    RefreshMediaState();
}

// ---------------------------------------------------------------------------
// Taskbar widget UI (GDI).
// ---------------------------------------------------------------------------

enum MediaGlyph { GlyphPrev, GlyphPlay, GlyphPause, GlyphNext };

void DrawMediaGlyph(HDC hdc, int x, int y, int w, int h, COLORREF fg,
                    MediaGlyph glyph) {
    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HBRUSH brush = CreateSolidBrush(fg);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    int cx = x + w / 2;
    int cy = y + h / 2;
    int s = (w < h ? w : h) / 4;
    if (s < 3) {
        s = 3;
    }
    if (glyph == GlyphPrev || glyph == GlyphNext) {
        int dir = (glyph == GlyphPrev) ? -1 : 1;
        // Bar on the outer side.
        int barX = (dir < 0) ? cx - s - 4 : cx + s;
        Rectangle(hdc, barX, cy - s, barX + 4, cy + s);
        POINT tri[3] = {{cx - dir * s, cy - s},
                        {cx - dir * s, cy + s},
                        {cx + dir * s, cy}};
        Polygon(hdc, tri, 3);
    } else if (glyph == GlyphPlay) {
        POINT tri[3] = {
            {cx - s, cy - s}, {cx - s, cy + s}, {cx + s, cy}};
        Polygon(hdc, tri, 3);
    } else {  // GlyphPause
        Rectangle(hdc, cx - s - 1, cy - s, cx - 1, cy + s);
        Rectangle(hdc, cx + 1, cy - s, cx + s + 1, cy + s);
    }
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void PaintBar(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;
    if (W <= 0 || H <= 0) {
        EndPaint(hwnd, &ps);
        return;
    }

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    COLORREF fgCol = GetSysColor(COLOR_BTNTEXT);
    HBRUSH bgBrush = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
    FillRect(mem, &rc, bgBrush);
    DeleteObject(bgBrush);

    HFONT hFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(mem, hFont);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, fgCol);

    const int kBtnW = 44;
    int btnCount = g_showButtons ? 3 : 0;
    int textW = W - btnCount * kBtnW - 16;
    if (textW < 40) {
        textW = 40;
    }
    RECT trc{8, 0, 8 + textW, H};
    std::wstring title =
        g_title.empty() ? L"No media playing" : g_title;
    DrawTextW(mem, title.c_str(), -1, &trc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (g_showButtons) {
        int bx = W - 3 * kBtnW;
        MediaGlyph middle =
            (g_playbackStatus == kPlaybackPlaying) ? GlyphPause : GlyphPlay;
        DrawMediaGlyph(mem, bx, 0, kBtnW, H, fgCol, GlyphPrev);
        DrawMediaGlyph(mem, bx + kBtnW, 0, kBtnW, H, fgCol, middle);
        DrawMediaGlyph(mem, bx + 2 * kBtnW, 0, kBtnW, H, fgCol, GlyphNext);
        g_btnRects[0] = {bx, 0, bx + kBtnW, H};
        g_btnRects[1] = {bx + kBtnW, 0, bx + 2 * kBtnW, H};
        g_btnRects[2] = {bx + 2 * kBtnW, 0, bx + 3 * kBtnW, H};
    }

    BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    DeleteObject(hFont);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

void UpdateTimerAndVisibility() {
    if (!g_hwndBar || g_shuttingDown) {
        return;
    }
    bool playing = (g_playbackStatus == kPlaybackPlaying);
    ShowWindow(g_hwndBar, (g_hasSession && g_enabled) ? SW_SHOW : SW_HIDE);
    if (playing && !g_timerOn) {
        SetTimer(g_hwndBar, kTimerId,
                 static_cast<UINT>(g_updateIntervalMs), nullptr);
        g_timerOn = true;
    } else if (!playing && g_timerOn) {
        KillTimer(g_hwndBar, kTimerId);
        g_timerOn = false;
    }
    InvalidateRect(g_hwndBar, nullptr, TRUE);
}

void RepositionBar() {
    if (!g_hwndBar || !g_hwndTray || g_shuttingDown) {
        return;
    }
    RECT trc{};
    GetClientRect(g_hwndTray, &trc);
    int trayH = trc.bottom - trc.top;
    int barH = trayH - 8;
    if (barH < 24) {
        barH = 24;
    }
    const int barW = 380;
    int x = 8;
    HWND trayNotify =
        FindWindowExW(g_hwndTray, nullptr, L"TrayNotifyWnd", nullptr);
    if (trayNotify) {
        RECT nrc{};
        GetWindowRect(trayNotify, &nrc);
        MapWindowPoints(HWND_DESKTOP, g_hwndTray,
                        reinterpret_cast<LPPOINT>(&nrc), 2);
        x = nrc.left - barW - 8;
        if (x < 8) {
            x = 8;
        }
    }
    int y = (trayH - barH) / 2;
    SetWindowPos(g_hwndBar, nullptr, x, y, barW, barH,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void OnBarClick(int x, int y) {
    if (!g_showButtons) {
        return;
    }
    POINT pt{x, y};
    for (int i = 0; i < 3; ++i) {
        if (PtInRect(&g_btnRects[i], pt)) {
            // Rect order: 0 = prev, 1 = play/pause, 2 = next.
            // FireTransport order: 0 = toggle, 1 = next, 2 = previous.
            int which = (i == 0) ? 2 : (i == 1) ? 0 : 1;
            FireTransport(which);
            UpdateTimerAndVisibility();
            return;
        }
    }
}

LRESULT CALLBACK BarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            RefreshMediaState();
            UpdateTimerAndVisibility();
            return 0;
        case WM_TIMER:
            if (wParam == kTimerId) {
                RefreshMediaState();
                UpdateTimerAndVisibility();
            }
            return 0;
        case WM_APP_MEDIA_EVENT:
            RefreshMediaState();
            UpdateTimerAndVisibility();
            return 0;
        case WM_PAINT:
            PaintBar(hwnd);
            return 0;
        case WM_LBUTTONDOWN:
            OnBarClick(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_APP_SHUTDOWN: {
            g_shuttingDown = true;
            if (g_timerOn) {
                KillTimer(hwnd, kTimerId);
                g_timerOn = false;
            }
            UnsubscribeSessionEvents();
            if (g_pSession) {
                g_pSession->lpVtbl->Release(g_pSession);
                g_pSession = nullptr;
            }
            if (g_pManager) {
                if (g_tokSessionChangedValid) {
                    g_pManager->lpVtbl->remove_CurrentSessionChanged(
                        g_pManager, g_tokSessionChanged);
                    g_tokSessionChangedValid = false;
                }
                g_pManager->lpVtbl->Release(g_pManager);
                g_pManager = nullptr;
            }
            if (g_pSink) {
                g_pSink->lpVtbl->Release(g_pSink);
                g_pSink = nullptr;
            }
            DestroyWindow(hwnd);
            PostQuitMessage(0);
            return 0;
        }
        case WM_NCDESTROY:
            g_hwndBar = nullptr;
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK TraySubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                  LPARAM lParam) {
    LRESULT result = CallWindowProcW(g_trayOldProc, hwnd, msg, wParam, lParam);
    if (msg == WM_WINDOWPOSCHANGED || msg == WM_SIZE) {
        RepositionBar();
    }
    return result;
}

DWORD WINAPI MediaThreadProc([[maybe_unused]] LPVOID param) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        Wh_Log(L"CoInitializeEx failed");
        return 1;
    }

    HMODULE hMod = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<PCWSTR>(&MediaThreadProc), &hMod);

    g_pSink = new (std::nothrow) EventSink();
    if (g_pSink) {
        g_pSink->lpVtbl = &g_sinkVtbl;
        g_pSink->ref = 1;
        g_pSink->hwndNotify = nullptr;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &BarWndProc;
    wc.hInstance = hMod;
    wc.lpszClassName = L"WindhawkTaskbarMediaLite";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    g_hwndTray = FindWindowW(L"Shell_TrayWnd", nullptr);
    g_hwndBar = CreateWindowExW(0, L"WindhawkTaskbarMediaLite", L"",
                                WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 380, 32,
                                g_hwndTray, nullptr, hMod, nullptr);
    if (g_hwndBar) {
        if (g_pSink) {
            g_pSink->hwndNotify = g_hwndBar;
        }
        if (g_hwndTray) {
            g_trayOldProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                g_hwndTray, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(&TraySubclassProc)));
            RepositionBar();
        }
        // WM_CREATE handler does the first refresh.
        ShowWindow(g_hwndBar, SW_HIDE);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return 0;
}

void StartMediaThread() {
    if (g_hThread) {
        return;
    }
    g_shuttingDown = false;
    g_hThread = CreateThread(nullptr, 0, &MediaThreadProc, nullptr, 0,
                             &g_threadId);
    if (!g_hThread) {
        Wh_Log(L"CreateThread for media worker failed");
    }
}

void StopMediaThread() {
    if (g_hwndBar) {
        PostMessageW(g_hwndBar, WM_APP_SHUTDOWN, 0, 0);
    } else if (g_threadId != 0) {
        PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
    }
    if (g_hThread) {
        WaitForSingleObject(g_hThread, 8000);
        CloseHandle(g_hThread);
        g_hThread = nullptr;
        g_threadId = 0;
    }
    if (g_hwndTray && g_trayOldProc) {
        SetWindowLongPtrW(g_hwndTray, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(g_trayOldProc));
        g_trayOldProc = nullptr;
    }
    g_hwndBar = nullptr;
    g_hwndTray = nullptr;
}

void LoadSettings() {
    g_enabled = Wh_GetIntSetting(L"enabled") != 0;
    g_showButtons = Wh_GetIntSetting(L"showButtons") != 0;
    g_maxTitleChars = Wh_GetIntSetting(L"maxTitleChars");
    if (g_maxTitleChars < 10) {
        g_maxTitleChars = 10;
    }
    if (g_maxTitleChars > 200) {
        g_maxTitleChars = 200;
    }
    g_updateIntervalMs = Wh_GetIntSetting(L"updateIntervalMs");
    if (g_updateIntervalMs < 1000) {
        g_updateIntervalMs = 1000;
    }
}

}  // namespace

BOOL Wh_ModInit() {
    LoadSettings();
    if (g_enabled) {
        StartMediaThread();
    }
    Wh_Log(L"Taskbar Media Lite initialized");
    return TRUE;
}

void Wh_ModUninit() {
    StopMediaThread();
    Wh_Log(L"Taskbar Media Lite uninitialized");
}

void Wh_ModSettingsChanged() {
    LoadSettings();
    if (g_enabled && !g_hThread) {
        StartMediaThread();
    } else if (!g_enabled && g_hThread) {
        StopMediaThread();
    } else if (g_hwndBar) {
        PostMessageW(g_hwndBar, WM_APP_MEDIA_EVENT, 0, 0);
    }
}
