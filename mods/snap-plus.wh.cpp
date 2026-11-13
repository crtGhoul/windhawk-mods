// ==WindhawkMod==
// @id              snap-plus
// @name            Snap Plus
// @description     Alt-drag to move/resize any window plus magnetic edge snapping, without touching normal Aero Snap
// @version         1.0
// @author          crtGhoul
// @github          https://github.com/crtGhoul
// @license         MIT
// @include         explorer.exe
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Snap Plus

Alt-drag any window to move it (Alt+Left) or resize it (Alt+Right), with
optional magnetic snapping of window edges to monitor work-area edges while
Alt-dragging. A lightweight, from-scratch take on the abandoned
`slick-window-arrangement` mod (v1.0.2, last updated 2024-10-01; open issues
#5817, #5473, and explicit fork demand in #2466 and #5813).

Design principle: the mod only engages while the Alt key is physically held
down during the drag. Everything else — Win10/Win11 split-screen, Aero Snap,
and Chromium/Electron's own snapping — is completely untouched because the
hook never activates for those gestures.

- Alt+Left-drag: move the top-level window under the cursor.
- Alt+Right-drag: resize from the nearest edge/corner (cursor position within
  the window's thirds decides which edges move).
- While Alt-moving, window edges snap magnetically to the monitor work-area
  edges within `snapDistance` pixels (0 disables).
- Only the Alt+button-down that starts a handled drag (and the button-up that
  ends it) is swallowed; every other input passes through untouched.
- Windows that are maximized, fullscreen, the taskbar (`Shell_TrayWnd`) or the
  desktop (`Progman`) are never grabbed, nor are exes in `excludedExes`.

## Settings

- **Enabled** — master switch.
- **AltMoveEnabled** — Alt+Left-drag moves windows.
- **AltResizeEnabled** — Alt+Right-drag resizes windows.
- **Snap distance** — pixels for magnetic edge snapping; 0 disables.
- **Excluded exes** — comma-separated exe basenames to ignore (e.g. `game.exe`).

## Honest limitation

This is Alt-drag move/resize plus magnetic edge snapping only. It does not
implement snap animations, snap layouts/grids, or window tiling — those belong
to a different (much heavier) mod. If you only want Aero Snap, you don't need
this mod at all.

## Testing status

Type-checked only with x86_64-w64-mingw32-g++ -fsyntax-only against the real
windhawk_api.h (Windhawk v1.7.3); NOT compiled with Windhawk's real compiler;
NOT tested on a real Windows machine. In particular, low-level mouse-hook
delivery timing, Alt-key state tracking during fast drags, and multi-monitor
work-area snapping all need verification on real Windows.
*/
// ==WindhawkModSettings==
/*
- enabled: true
  $name: Enabled
- altMoveEnabled: true
  $name: Alt+Left-drag moves windows
- altResizeEnabled: true
  $name: Alt+Right-drag resizes windows
- snapDistance: 12
  $name: Snap distance
  $description: Pixels; 0 disables magnetic snapping
- excludedExes: ""
  $name: Excluded exes
  $description: Comma-separated exe basenames to ignore, e.g. game.exe
*/
// ==/WindhawkModSettings==

#include <windows.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr int kDragNone = 0;
constexpr int kDragMove = 1;
constexpr int kDragResize = 2;

constexpr int kEdgeLeft = 1;
constexpr int kEdgeRight = 2;
constexpr int kEdgeTop = 4;
constexpr int kEdgeBottom = 8;

constexpr int kMinWindowSize = 64;

HHOOK g_hMouseHook = nullptr;
HANDLE g_hHookThread = nullptr;
DWORD g_hookThreadId = 0;

bool g_enabled = true;
bool g_altMoveEnabled = true;
bool g_altResizeEnabled = true;
int g_snapDistance = 12;
std::vector<std::wstring> g_excludedExes;

int g_dragMode = kDragNone;
HWND g_dragHwnd = nullptr;
POINT g_downPt{};
RECT g_dragOrigRect{};
int g_grabDX = 0;
int g_grabDY = 0;
int g_resizeEdges = 0;

bool IsAltHeld() {
    return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
}

std::wstring ExeBasenameOfWindow(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) {
        return std::wstring();
    }
    std::wstring result;
    HANDLE hProc =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        wchar_t path[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(hProc, 0, path, &size) && size > 0) {
            const wchar_t* base = wcsrchr(path, L'\\');
            result = base ? (base + 1) : path;
        }
        CloseHandle(hProc);
    }
    return result;
}

bool IsExcludedExe(const std::wstring& exe) {
    for (const auto& e : g_excludedExes) {
        if (!e.empty() && _wcsicmp(e.c_str(), exe.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

bool IsFullscreenRect(HWND hwnd, const RECT& rc) {
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) {
        return false;
    }
    return rc.left <= mi.rcMonitor.left && rc.top <= mi.rcMonitor.top &&
           rc.right >= mi.rcMonitor.right && rc.bottom >= mi.rcMonitor.bottom;
}

// Returns true and fills the drag state if a drag was started (caller swallows
// the button-down). `move` selects move mode (Alt+Left) vs resize (Alt+Right).
bool TryBeginDrag(POINT pt, bool move) {
    HWND hwnd = WindowFromPoint(pt);
    if (!hwnd) {
        return false;
    }
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root || !IsWindowVisible(root)) {
        return false;
    }
    wchar_t cls[64] = {};
    GetClassNameW(root, cls, static_cast<int>(std::size(cls)));
    if (wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Progman") == 0) {
        return false;
    }
    if (IsZoomed(root)) {
        return false;
    }
    RECT rc{};
    if (!GetWindowRect(root, &rc)) {
        return false;
    }
    if (IsFullscreenRect(root, rc)) {
        return false;
    }
    if (IsExcludedExe(ExeBasenameOfWindow(root))) {
        return false;
    }

    g_dragHwnd = root;
    g_dragOrigRect = rc;
    g_downPt = pt;
    if (move) {
        g_dragMode = kDragMove;
        g_grabDX = pt.x - rc.left;
        g_grabDY = pt.y - rc.top;
    } else {
        g_dragMode = kDragResize;
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        g_resizeEdges = 0;
        if (w > 0) {
            if (pt.x < rc.left + w / 3) {
                g_resizeEdges |= kEdgeLeft;
            } else if (pt.x > rc.right - w / 3) {
                g_resizeEdges |= kEdgeRight;
            }
        }
        if (h > 0) {
            if (pt.y < rc.top + h / 3) {
                g_resizeEdges |= kEdgeTop;
            } else if (pt.y > rc.bottom - h / 3) {
                g_resizeEdges |= kEdgeBottom;
            }
        }
        if (g_resizeEdges == 0) {
            g_resizeEdges = kEdgeRight | kEdgeBottom;
        }
    }
    return true;
}

void EndDrag() {
    g_dragMode = kDragNone;
    g_dragHwnd = nullptr;
    g_resizeEdges = 0;
}

void UpdateDrag(POINT pt) {
    if (!IsWindow(g_dragHwnd) || !IsAltHeld()) {
        EndDrag();
        return;
    }
    if (g_dragMode == kDragMove) {
        int w = g_dragOrigRect.right - g_dragOrigRect.left;
        int h = g_dragOrigRect.bottom - g_dragOrigRect.top;
        int nx = pt.x - g_grabDX;
        int ny = pt.y - g_grabDY;
        if (g_snapDistance > 0) {
            HMONITOR mon =
                MonitorFromWindow(g_dragHwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(mon, &mi)) {
                const RECT& wa = mi.rcWork;
                if (std::abs(nx - wa.left) <= g_snapDistance) {
                    nx = wa.left;
                } else if (std::abs(nx + w - wa.right) <= g_snapDistance) {
                    nx = wa.right - w;
                }
                if (std::abs(ny - wa.top) <= g_snapDistance) {
                    ny = wa.top;
                } else if (std::abs(ny + h - wa.bottom) <= g_snapDistance) {
                    ny = wa.bottom - h;
                }
            }
        }
        SetWindowPos(g_dragHwnd, nullptr, nx, ny, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    } else if (g_dragMode == kDragResize) {
        int dx = pt.x - g_downPt.x;
        int dy = pt.y - g_downPt.y;
        RECT nr = g_dragOrigRect;
        if (g_resizeEdges & kEdgeLeft) {
            nr.left += dx;
        }
        if (g_resizeEdges & kEdgeRight) {
            nr.right += dx;
        }
        if (g_resizeEdges & kEdgeTop) {
            nr.top += dy;
        }
        if (g_resizeEdges & kEdgeBottom) {
            nr.bottom += dy;
        }
        if (nr.right - nr.left < kMinWindowSize) {
            if (g_resizeEdges & kEdgeLeft) {
                nr.left = nr.right - kMinWindowSize;
            } else {
                nr.right = nr.left + kMinWindowSize;
            }
        }
        if (nr.bottom - nr.top < kMinWindowSize) {
            if (g_resizeEdges & kEdgeTop) {
                nr.top = nr.bottom - kMinWindowSize;
            } else {
                nr.bottom = nr.top + kMinWindowSize;
            }
        }
        SetWindowPos(g_dragHwnd, nullptr, nr.left, nr.top,
                     nr.right - nr.left, nr.bottom - nr.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const MSLLHOOKSTRUCT* ms =
            reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        switch (wParam) {
            case WM_LBUTTONDOWN:
                if (g_enabled && g_altMoveEnabled && IsAltHeld() &&
                    TryBeginDrag(ms->pt, true)) {
                    return 1;
                }
                break;
            case WM_RBUTTONDOWN:
                if (g_enabled && g_altResizeEnabled && IsAltHeld() &&
                    TryBeginDrag(ms->pt, false)) {
                    return 1;
                }
                break;
            case WM_MOUSEMOVE:
                if (g_dragMode != kDragNone) {
                    UpdateDrag(ms->pt);
                }
                break;
            case WM_LBUTTONUP:
                if (g_dragMode == kDragMove) {
                    EndDrag();
                    return 1;
                }
                break;
            case WM_RBUTTONUP:
                if (g_dragMode == kDragResize) {
                    EndDrag();
                    return 1;
                }
                break;
            default:
                break;
        }
    }
    return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
}

DWORD WINAPI HookThreadProc([[maybe_unused]] LPVOID param) {
    HMODULE hMod = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<PCWSTR>(LowLevelMouseProc),
        &hMod);
    g_hMouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, hMod, 0);
    if (!g_hMouseHook) {
        Wh_Log(L"SetWindowsHookExW(WH_MOUSE_LL) failed");
    }
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_hMouseHook) {
        UnhookWindowsHookEx(g_hMouseHook);
        g_hMouseHook = nullptr;
    }
    return 0;
}

void LoadSettings() {
    g_enabled = Wh_GetIntSetting(L"enabled") != 0;
    g_altMoveEnabled = Wh_GetIntSetting(L"altMoveEnabled") != 0;
    g_altResizeEnabled = Wh_GetIntSetting(L"altResizeEnabled") != 0;
    g_snapDistance = Wh_GetIntSetting(L"snapDistance");
    if (g_snapDistance < 0) {
        g_snapDistance = 0;
    }

    g_excludedExes.clear();
    PCWSTR raw = Wh_GetStringSetting(L"excludedExes");
    std::wstring s = raw ? raw : L"";
    Wh_FreeStringSetting(raw);
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find(L',', start);
        if (end == std::wstring::npos) {
            end = s.size();
        }
        size_t a = start;
        while (a < end && (s[a] == L' ' || s[a] == L'\t')) {
            ++a;
        }
        size_t b = end;
        while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t')) {
            --b;
        }
        if (b > a) {
            g_excludedExes.emplace_back(s.substr(a, b - a));
        }
        start = end + 1;
    }
}

}  // namespace

BOOL Wh_ModInit() {
    LoadSettings();
    g_hHookThread = CreateThread(nullptr, 0, HookThreadProc, nullptr, 0,
                                 &g_hookThreadId);
    if (!g_hHookThread) {
        Wh_Log(L"CreateThread for mouse hook failed");
        return FALSE;
    }
    Wh_Log(L"Snap Plus initialized");
    return TRUE;
}

void Wh_ModUninit() {
    if (g_hookThreadId != 0) {
        PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    }
    if (g_hHookThread) {
        WaitForSingleObject(g_hHookThread, 5000);
        CloseHandle(g_hHookThread);
        g_hHookThread = nullptr;
        g_hookThreadId = 0;
    }
    Wh_Log(L"Snap Plus uninitialized");
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}
