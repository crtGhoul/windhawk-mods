// ==WindhawkMod==
// @id              rdp-connection-bar-shrink
// @name            RDP Connection Bar Shrink
// @description     Restore the old small RDP connection bar height (24H2 made it taller)
// @version         1.0
// @author          crtGhoul
// @github          https://github.com/crtGhoul
// @license         MIT
// @include         mstsc.exe
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# RDP Connection Bar Shrink

Windows 11 24H2 made the Remote Desktop connection bar (the strip at the
top of a full-screen RDP session with the minimize/pin buttons) taller,
and it can no longer be shrunk back down. This mod clamps the bar's
height back to a small value.

How it works: `CreateWindowExW` is hooked in `mstsc.exe`. Each new
top-level unowned `WS_POPUP` window is inspected — if it has at least
two `BUTTON`-class children (the minimize/pin buttons) and is taller
than `barHeightPx`, it's treated as the connection bar, subclassed, and
its height is clamped in `WM_WINDOWPOSCHANGING`. The clamp is also
applied once immediately when the bar is detected.

## Settings

- **Enabled** — master switch.
- **Bar height (px)** — the forced bar height, default 30.

## Honest limitations

- **Heuristic detection.** The connection bar is identified by shape
  (popup + >=2 buttons + tall), not by a documented class name or
  window property — the heuristic was informed by the screenshots in
  issue #2116, but mstsc internals vary and this needs confirmation on
  a real Windows machine with real RDP sessions.
- If mstsc changes the bar's structure (different child count, nested
  containers), the mod will silently do nothing rather than break
  anything — check the Windhawk log to see whether the bar was detected.
- Unloading restores the original window procedures; bars already
  shrunk keep their size until mstsc resizes them itself.

## Testing status

- Type-checked only with `x86_64-w64-mingw32-g++ -fsyntax-only` against
  the real `windhawk_api.h` (Windhawk v1.7.3).
- NOT compiled with Windhawk's real compiler.
- NOT tested on a real Windows machine — needs real-Windows
  confirmation: open 2+ RDP sessions, verify the bar shrinks, pin/unpin
  and minimize still work, and unload restores stock behavior.
*/
// ==WindhawkModSettings==
/*
- enabled: true
  $name: Enabled
  $description: Master switch for shrinking the RDP connection bar.
- barHeightPx: 30
  $name: Bar height (px)
  $description: The forced connection bar height in pixels.
*/
// ==/WindhawkModSettings==

#include <windows.h>

#include <unordered_set>

namespace {

using CreateWindowExW_t = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int,
                                        int, int, int, HWND, HMENU,
                                        HINSTANCE, LPVOID);
CreateWindowExW_t CreateWindowExW_Original = nullptr;

bool g_enabled = true;
int g_barHeightPx = 30;
std::unordered_set<HWND> g_subclassedBars;

struct ButtonCount {
    int count = 0;
};

BOOL CALLBACK CountButtonsProc(HWND hWnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<ButtonCount*>(lParam);
    wchar_t className[64] = {};
    GetClassNameW(hWnd, className, ARRAYSIZE(className));
    if (wcscmp(className, L"Button") == 0) {
        ctx->count++;
    }
    return TRUE;
}

bool IsConnectionBarCandidate(HWND hWnd, DWORD dwStyle, HWND hWndParent) {
    if ((dwStyle & WS_POPUP) == 0 || (dwStyle & WS_CHILD) != 0) {
        return false;
    }
    if (hWndParent != nullptr) {
        return false;
    }
    if (GetWindow(hWnd, GW_OWNER) != nullptr) {
        return false;
    }

    ButtonCount ctx{};
    EnumChildWindows(hWnd, CountButtonsProc,
                     reinterpret_cast<LPARAM>(&ctx));
    if (ctx.count < 2) {
        return false;
    }

    RECT rc{};
    if (!GetWindowRect(hWnd, &rc)) {
        return false;
    }
    LONG height = rc.bottom - rc.top;
    return height > g_barHeightPx && height < 400;
}

LRESULT CALLBACK ConnectionBarSubclassProc(HWND hWnd, UINT uMsg,
                                            WPARAM wParam, LPARAM lParam) {
    WNDPROC origProc = reinterpret_cast<WNDPROC>(
        GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    if (uMsg == WM_WINDOWPOSCHANGING && g_enabled) {
        auto* wp = reinterpret_cast<WINDOWPOS*>(lParam);
        if ((wp->flags & SWP_NOSIZE) == 0 && wp->cy > g_barHeightPx) {
            wp->cy = g_barHeightPx;
        }
    }
    if (uMsg == WM_NCDESTROY) {
        g_subclassedBars.erase(hWnd);
        WNDPROC proc = origProc;
        SetWindowLongPtrW(hWnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(proc));
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        return CallWindowProcW(proc, hWnd, uMsg, wParam, lParam);
    }
    if (origProc) {
        return CallWindowProcW(origProc, hWnd, uMsg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

void SubclassConnectionBar(HWND hWnd) {
    if (g_subclassedBars.count(hWnd) != 0) {
        return;
    }

    WNDPROC origProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        hWnd, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(ConnectionBarSubclassProc)));
    if (!origProc) {
        Wh_Log(L"Failed to subclass candidate bar %p", hWnd);
        return;
    }
    SetWindowLongPtrW(hWnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(origProc));
    g_subclassedBars.insert(hWnd);
    Wh_Log(L"Connection bar detected and subclassed: %p", hWnd);

    // Apply the clamp once immediately.
    RECT rc{};
    if (GetWindowRect(hWnd, &rc)) {
        SetWindowPos(hWnd, nullptr, 0, 0, rc.right - rc.left, g_barHeightPx,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

HWND WINAPI CreateWindowExW_Hook(DWORD dwExStyle, LPCWSTR lpClassName,
                                  LPCWSTR lpWindowName, DWORD dwStyle, int x,
                                  int y, int nWidth, int nHeight,
                                  HWND hWndParent, HMENU hMenu,
                                  HINSTANCE hInstance, LPVOID lpParam) {
    HWND hWnd = CreateWindowExW_Original(
        dwExStyle, lpClassName, lpWindowName, dwStyle, x, y, nWidth,
        nHeight, hWndParent, hMenu, hInstance, lpParam);
    if (hWnd && g_enabled &&
        IsConnectionBarCandidate(hWnd, dwStyle, hWndParent)) {
        SubclassConnectionBar(hWnd);
    }
    return hWnd;
}

void LoadSettings() {
    g_enabled = Wh_GetIntSetting(L"enabled") != 0;
    int height = Wh_GetIntSetting(L"barHeightPx");
    if (height < 16) {
        height = 16;
    }
    if (height > 200) {
        height = 200;
    }
    g_barHeightPx = height;
}

}  // namespace

BOOL Wh_ModInit(void) {
    Wh_Log(L"Init");
    LoadSettings();
    if (!Wh_SetFunctionHook(reinterpret_cast<void*>(CreateWindowExW),
                            reinterpret_cast<void*>(CreateWindowExW_Hook),
                            reinterpret_cast<void**>(
                                &CreateWindowExW_Original))) {
        Wh_Log(L"Failed to hook CreateWindowExW");
        return FALSE;
    }
    return TRUE;
}

void Wh_ModUninit(void) {
    Wh_Log(L"Uninit");
    Wh_RemoveFunctionHook(reinterpret_cast<void*>(CreateWindowExW));
    // Restore original procs on any bars we subclassed.
    for (HWND hWnd : g_subclassedBars) {
        if (IsWindow(hWnd)) {
            WNDPROC origProc = reinterpret_cast<WNDPROC>(
                GetWindowLongPtrW(hWnd, GWLP_USERDATA));
            if (origProc) {
                SetWindowLongPtrW(hWnd, GWLP_WNDPROC,
                                  reinterpret_cast<LONG_PTR>(origProc));
                SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
            }
        }
    }
    g_subclassedBars.clear();
}

void Wh_ModSettingsChanged(void) {
    Wh_Log(L"Settings changed");
    LoadSettings();
    // Re-apply the (possibly new) height to already-detected bars.
    for (HWND hWnd : g_subclassedBars) {
        if (IsWindow(hWnd) && g_enabled) {
            RECT rc{};
            if (GetWindowRect(hWnd, &rc)) {
                SetWindowPos(hWnd, nullptr, 0, 0, rc.right - rc.left,
                             g_barHeightPx,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }
}
