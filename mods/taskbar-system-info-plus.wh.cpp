// ==WindhawkMod==
// @id              taskbar-system-info-plus
// @name            Taskbar System Info Plus
// @description     CPU/RAM/network readout rendered natively in the taskbar, slow-cadence sampling, no overlay window
// @version         1.0
// @author          crtGhoul
// @github          https://github.com/crtGhoul
// @license         MIT
// @include         explorer.exe
// @compilerOptions -lgdi32 -liphlpapi
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Taskbar System Info Plus

Renders a live `CPU 12%  MEM 45%  ▼1.2M ▲0.3M` style readout natively in the
taskbar — a real Win32 child window of `Shell_TrayWnd`, GDI-drawn, no overlay
window and no always-on-top hacks. Prior art `taskbar-system-info` exists;
this is the lean "plus" take requested in issues #5835 ("Add Taskbar System
Info Plus") and #5921.

- CPU via `GetSystemTimes` deltas, RAM via `GlobalMemoryStatusEx`, network
  up/down rates via `GetIfTable` (-liphlpapi) summed over operational
  non-loopback interfaces.
- Sampling is a single `WM_TIMER` at `updateIntervalMs` (default 2000,
  minimum 1000 enforced in code) — slow cadence, no polling loops.
- Repositioned event-driven: `Shell_TrayWnd` is subclassed (classic
  `SetWindowLongPtr`, user32 only) and the widget follows layout changes on
  `WM_WINDOWPOSCHANGED`/`WM_SIZE`. No timers are used for positioning.
- Clean unload: timer killed, child window destroyed, subclass removed.

## Settings

- **Enabled** — master switch.
- **Update interval ms** — sampling cadence; minimum 1000, enforced in code.
- **Show CPU / Show RAM / Show net** — toggle each readout segment.

## Honest limitation

Placement is heuristic: the widget anchors just left of the notification area
(`TrayNotifyWnd`) and falls back to the taskbar's left edge if that window
isn't found — on crowded or heavily customized taskbars it may need
repositioning logic tuned on real Windows. Colors follow system colors and
DPI scaling is not handled (the widget is fixed-height relative to the
taskbar). First paint shows `...` until the second sample establishes deltas.

## Testing status

Type-checked only with x86_64-w64-mingw32-g++ -fsyntax-only against the real
windhawk_api.h (Windhawk v1.7.3); NOT compiled with Windhawk's real compiler;
NOT tested on a real Windows machine. Real-Windows verification needed:
`GetIfTable` interface enumeration on the user's adapters, rate math across
counter wraps, widget placement on 24H2/26H2 taskbars, and readability over
light/dark taskbar themes.
*/
// ==WindhawkModSettings==
/*
- enabled: true
  $name: Enabled
- updateIntervalMs: 2000
  $name: Update interval ms
  $description: Minimum 1000
- showCpu: true
  $name: Show CPU
- showRam: true
  $name: Show RAM
- showNet: true
  $name: Show net
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <iphlpapi.h>

#include <string>
#include <vector>

namespace {

constexpr UINT_PTR kTimerId = 1;

HANDLE g_hThread = nullptr;
DWORD g_threadId = 0;
HWND g_hwndBar = nullptr;
HWND g_hwndTray = nullptr;
WNDPROC g_trayOldProc = nullptr;
bool g_shuttingDown = false;

bool g_enabled = true;
int g_updateIntervalMs = 2000;
bool g_showCpu = true;
bool g_showRam = true;
bool g_showNet = true;

bool g_haveCpuBase = false;
ULARGE_INTEGER g_prevIdle{};
ULARGE_INTEGER g_prevKernel{};
ULARGE_INTEGER g_prevUser{};
int g_cpuPct = 0;
int g_memPct = 0;
bool g_haveNetBase = false;
ULONGLONG g_prevInOctets = 0;
ULONGLONG g_prevOutOctets = 0;
double g_downBps = 0.0;
double g_upBps = 0.0;
std::wstring g_text = L"...";

constexpr UINT WM_APP_SHUTDOWN = WM_APP + 111;

ULARGE_INTEGER FileTimeToUL(const FILETIME& ft) {
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u;
}

void SampleCpu() {
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) {
        return;
    }
    ULARGE_INTEGER i = FileTimeToUL(idle);
    ULARGE_INTEGER k = FileTimeToUL(kernel);
    ULARGE_INTEGER u = FileTimeToUL(user);
    if (g_haveCpuBase) {
        ULONGLONG idleDelta = i.QuadPart - g_prevIdle.QuadPart;
        ULONGLONG totalDelta = (k.QuadPart + u.QuadPart) -
                               (g_prevKernel.QuadPart + g_prevUser.QuadPart);
        if (totalDelta > 0) {
            int pct = static_cast<int>(
                (1.0 - static_cast<double>(idleDelta) /
                           static_cast<double>(totalDelta)) *
                100.0);
            if (pct < 0) {
                pct = 0;
            }
            if (pct > 100) {
                pct = 100;
            }
            g_cpuPct = pct;
        }
    }
    g_prevIdle = i;
    g_prevKernel = k;
    g_prevUser = u;
    g_haveCpuBase = true;
}

void SampleMem() {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        g_memPct = static_cast<int>(ms.dwMemoryLoad);
    }
}

void SampleNet() {
    ULONG size = 0;
    // First call with nullptr returns the required size.
    if (GetIfTable(nullptr, &size, FALSE) != ERROR_INSUFFICIENT_BUFFER) {
        return;
    }
    std::vector<BYTE> buf(size);
    MIB_IFTABLE* table = reinterpret_cast<MIB_IFTABLE*>(buf.data());
    if (GetIfTable(table, &size, FALSE) != NO_ERROR) {
        return;
    }
    ULONGLONG inOctets = 0;
    ULONGLONG outOctets = 0;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const MIB_IFROW& row = table->table[i];
        if (row.dwOperStatus == IF_OPER_STATUS_OPERATIONAL &&
            row.dwType != IF_TYPE_SOFTWARE_LOOPBACK) {
            inOctets += row.dwInOctets;
            outOctets += row.dwOutOctets;
        }
    }
    if (g_haveNetBase) {
        double secs = static_cast<double>(g_updateIntervalMs) / 1000.0;
        if (secs < 1.0) {
            secs = 1.0;
        }
        ULONGLONG dIn = (inOctets >= g_prevInOctets)
                            ? (inOctets - g_prevInOctets)
                            : (0x100000000ULL - g_prevInOctets + inOctets);
        ULONGLONG dOut = (outOctets >= g_prevOutOctets)
                             ? (outOctets - g_prevOutOctets)
                             : (0x100000000ULL - g_prevOutOctets + outOctets);
        g_downBps = static_cast<double>(dIn) / secs;
        g_upBps = static_cast<double>(dOut) / secs;
    }
    g_prevInOctets = inOctets;
    g_prevOutOctets = outOctets;
    g_haveNetBase = true;
}

void FormatRate(double bps, wchar_t* out, size_t outChars) {
    const wchar_t* unit = L"B";
    double v = bps;
    if (v >= 1024.0 * 1024.0) {
        v /= 1024.0 * 1024.0;
        unit = L"M";
    } else if (v >= 1024.0) {
        v /= 1024.0;
        unit = L"K";
    }
    swprintf(out, outChars, L"%.1f%s", v, unit);
}

void RebuildText() {
    std::wstring t;
    if (g_showCpu) {
        wchar_t b[32];
        swprintf(b, std::size(b), L"CPU %d%%", g_cpuPct);
        t += b;
    }
    if (g_showRam) {
        if (!t.empty()) {
            t += L"  ";
        }
        wchar_t b[32];
        swprintf(b, std::size(b), L"MEM %d%%", g_memPct);
        t += b;
    }
    if (g_showNet) {
        if (!t.empty()) {
            t += L"  ";
        }
        wchar_t down[32], up[32];
        FormatRate(g_downBps, down, std::size(down));
        FormatRate(g_upBps, up, std::size(up));
        wchar_t b[96];
        // \x25BC BLACK DOWN-POINTING TRIANGLE, \x25B2 BLACK UP-POINTING.
        swprintf(b, std::size(b), L"▼%s ▲%s", down, up);
        t += b;
    }
    if (t.empty()) {
        t = L"...";
    }
    g_text = t;
}

void PaintBar(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;
    if (W > 0 && H > 0) {
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);

        HBRUSH bgBrush = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
        FillRect(mem, &rc, bgBrush);
        DeleteObject(bgBrush);

        HFONT hFont = CreateFontW(
            -14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(mem, hFont);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, GetSysColor(COLOR_BTNTEXT));
        RECT trc{8, 0, W - 8, H};
        DrawTextW(mem, g_text.c_str(), -1, &trc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldFont);
        SelectObject(mem, oldBmp);
        DeleteObject(hFont);
        DeleteObject(bmp);
        DeleteDC(mem);
    }
    EndPaint(hwnd, &ps);
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
    // Measure the text so the widget only takes what it needs.
    HDC hdc = GetDC(g_hwndBar);
    HFONT hFont = CreateFontW(
        -14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(hdc, hFont);
    RECT mrc{0, 0, 4096, barH};
    DrawTextW(hdc, g_text.c_str(), -1, &mrc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_CALCRECT);
    SelectObject(hdc, oldFont);
    DeleteObject(hFont);
    ReleaseDC(g_hwndBar, hdc);
    int barW = (mrc.right - mrc.left) + 24;
    if (barW < 120) {
        barW = 120;
    }

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

LRESULT CALLBACK BarWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                            [[maybe_unused]] LPARAM lParam) {
    switch (msg) {
        case WM_TIMER:
            if (wParam == kTimerId) {
                SampleCpu();
                SampleMem();
                SampleNet();
                RebuildText();
                RepositionBar();  // width follows the text
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        case WM_PAINT:
            PaintBar(hwnd);
            return 0;
        case WM_APP_SHUTDOWN:
            g_shuttingDown = true;
            KillTimer(hwnd, kTimerId);
            DestroyWindow(hwnd);
            PostQuitMessage(0);
            return 0;
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

DWORD WINAPI InfoThreadProc([[maybe_unused]] LPVOID param) {
    HMODULE hMod = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<PCWSTR>(&InfoThreadProc), &hMod);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &BarWndProc;
    wc.hInstance = hMod;
    wc.lpszClassName = L"WindhawkTaskbarSysInfoPlus";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    g_hwndTray = FindWindowW(L"Shell_TrayWnd", nullptr);
    g_hwndBar = CreateWindowExW(0, L"WindhawkTaskbarSysInfoPlus", L"",
                                WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 200, 32,
                                g_hwndTray, nullptr, hMod, nullptr);
    if (g_hwndBar && g_hwndTray) {
        g_trayOldProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            g_hwndTray, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(&TraySubclassProc)));
        // Prime the first sample so the widget isn't blank.
        SampleCpu();
        SampleMem();
        SampleNet();
        RebuildText();
        RepositionBar();
        ShowWindow(g_hwndBar, SW_SHOW);
        SetTimer(g_hwndBar, kTimerId,
                 static_cast<UINT>(g_updateIntervalMs), nullptr);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

void StartInfoThread() {
    if (g_hThread) {
        return;
    }
    g_shuttingDown = false;
    g_hThread = CreateThread(nullptr, 0, &InfoThreadProc, nullptr, 0,
                             &g_threadId);
    if (!g_hThread) {
        Wh_Log(L"CreateThread for sysinfo worker failed");
    }
}

void StopInfoThread() {
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
    g_updateIntervalMs = Wh_GetIntSetting(L"updateIntervalMs");
    if (g_updateIntervalMs < 1000) {
        g_updateIntervalMs = 1000;
    }
    g_showCpu = Wh_GetIntSetting(L"showCpu") != 0;
    g_showRam = Wh_GetIntSetting(L"showRam") != 0;
    g_showNet = Wh_GetIntSetting(L"showNet") != 0;
}

}  // namespace

BOOL Wh_ModInit() {
    LoadSettings();
    if (g_enabled) {
        StartInfoThread();
    }
    Wh_Log(L"Taskbar System Info Plus initialized");
    return TRUE;
}

void Wh_ModUninit() {
    StopInfoThread();
    Wh_Log(L"Taskbar System Info Plus uninitialized");
}

void Wh_ModSettingsChanged() {
    bool wasEnabled = (g_hThread != nullptr);
    LoadSettings();
    if (g_enabled && !wasEnabled) {
        StartInfoThread();
    } else if (!g_enabled && wasEnabled) {
        StopInfoThread();
    } else if (g_hwndBar) {
        // Apply new cadence/segments live.
        KillTimer(g_hwndBar, kTimerId);
        SetTimer(g_hwndBar, kTimerId,
                 static_cast<UINT>(g_updateIntervalMs), nullptr);
        RebuildText();
        InvalidateRect(g_hwndBar, nullptr, TRUE);
    }
}
