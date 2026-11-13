// ==WindhawkMod==
// @id              taskbar-overflow-space-fix
// @name            Taskbar Overflow Space Fix
// @description     Remove the dead gap left when the taskbar overflow chevron appears
// @version         1.0
// @author          crtGhoul
// @github          https://github.com/crtGhoul
// @license         MIT
// @include         explorer.exe
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Taskbar Overflow Space Fix

When the taskbar notification area overflows, Windows shows the overflow
chevron (`...`) but often leaves a dead empty gap between the last visible
tray icon and the chevron/clock. This mod closes that gap: whenever the
tray container (`TrayNotifyWnd`) repositions, it measures the empty space
between the last visible icon and the chevron/clock and shifts the
chevron and clock leftward to close it.

It is deliberately conservative:

- It only ever moves windows to **smaller** x positions (closing gaps).
  It never enlarges, stretches, or reorders anything.
- It never touches the icon area itself — only the chevron button and the
  clock (anything sitting to the right of the gap) move.
- If the gap is at or below `maxGapPx`, nothing happens.
- A sanity cap (200 px) prevents runaway shifts from a misdetected layout.

## Settings

- **Enabled** — master switch.
- **Max gap (px)** — gaps at or below this width are left alone.

## Honest limitations

- This is a **heuristic layout fix**. The Win11 tray internals vary by
  build (26H2 turned the system-tray container from a Grid into a
  StackPanel), so the child-window shapes this mod looks for may differ
  on your build.
- Explorer owns the tray layout and may reposition children on its own;
  the mod re-applies after every `TrayNotifyWnd` reposition, but a fight
  with Explorer's own layout pass is possible on some builds.
- Unloading only removes the subclass hook; icon/chevron positions are
  left wherever they are until Explorer re-lays out the tray itself.

## Testing status

- Type-checked only with `x86_64-w64-mingw32-g++ -fsyntax-only` against
  the real `windhawk_api.h` (Windhawk v1.7.3).
- NOT compiled with Windhawk's real compiler.
- NOT tested on a real Windows machine — needs real-PC validation on
  24H2 and 26H2 with a full overflow tray.
*/
// ==WindhawkModSettings==
/*
- enabled: true
  $name: Enabled
  $description: Master switch for the gap-closing fix.
- maxGapPx: 8
  $name: Max gap (px)
  $description: Gaps at or below this width are left alone.
*/
// ==/WindhawkModSettings==

#include <windows.h>

namespace {

bool g_enabled = true;
int g_maxGapPx = 8;
HWND g_trayNotifyWnd = nullptr;
WNDPROC g_origTrayNotifyProc = nullptr;

struct GapScan {
    HWND chevron = nullptr;   // overflow "..." button
    HWND clock = nullptr;     // TrayClockWClass
    LONG lastIconRight = 0;   // right edge of the rightmost icon (client coords)
    bool haveIcon = false;
};

BOOL CALLBACK EnumTrayChildrenProc(HWND hWnd, LPARAM lParam) {
    GapScan* scan = reinterpret_cast<GapScan*>(lParam);

    if (!IsWindowVisible(hWnd)) {
        return TRUE;
    }

    wchar_t className[64] = {};
    GetClassNameW(hWnd, className, ARRAYSIZE(className));

    // Clock: never treated as an icon, and a shift candidate.
    if (wcscmp(className, L"TrayClockWClass") == 0) {
        scan->clock = hWnd;
        return TRUE;
    }

    // Overflow chevron: a Button whose text is "..." or the single ellipsis char.
    if (wcscmp(className, L"Button") == 0) {
        wchar_t text[16] = {};
        GetWindowTextW(hWnd, text, ARRAYSIZE(text));
        if (wcscmp(text, L"...") == 0 || wcscmp(text, L"\u2026") == 0) {
            scan->chevron = hWnd;
            return TRUE;
        }
    }

    // Anything else visible counts as an icon for gap measurement.
    RECT rc{};
    if (GetWindowRect(hWnd, &rc)) {
        POINT pt{rc.right, rc.top};
        ScreenToClient(g_trayNotifyWnd, &pt);
        if (!scan->haveIcon || pt.x > scan->lastIconRight) {
            scan->lastIconRight = pt.x;
            scan->haveIcon = true;
        }
    }
    return TRUE;
}

struct ShiftCtx {
    LONG anchorLeft;  // client x: everything at/after this moves left
    int delta;
};

BOOL CALLBACK ShiftRightSideProc(HWND hWnd, LPARAM lParam) {
    const ShiftCtx* ctx = reinterpret_cast<const ShiftCtx*>(lParam);
    if (!IsWindowVisible(hWnd)) {
        return TRUE;
    }
    RECT rc{};
    if (!GetWindowRect(hWnd, &rc)) {
        return TRUE;
    }
    POINT pt{rc.left, rc.top};
    ScreenToClient(g_trayNotifyWnd, &pt);
    if (pt.x >= ctx->anchorLeft) {
        SetWindowPos(hWnd, nullptr, pt.x - ctx->delta, pt.y, 0, 0,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
    }
    return TRUE;
}

void CloseTrayGap() {
    if (!g_enabled || !g_trayNotifyWnd || !IsWindow(g_trayNotifyWnd)) {
        return;
    }

    GapScan scan{};
    EnumChildWindows(g_trayNotifyWnd, EnumTrayChildrenProc,
                     reinterpret_cast<LPARAM>(&scan));

    if (!scan.haveIcon || !scan.chevron) {
        return;  // no icons or no chevron: nothing to fix
    }

    // Anchor = left edge of the chevron (or clock if it sits further left).
    RECT rcChevron{};
    if (!GetWindowRect(scan.chevron, &rcChevron)) {
        return;
    }
    POINT ptChevron{rcChevron.left, rcChevron.top};
    ScreenToClient(g_trayNotifyWnd, &ptChevron);
    LONG anchorLeft = ptChevron.x;

    if (scan.clock) {
        RECT rcClock{};
        if (GetWindowRect(scan.clock, &rcClock)) {
            POINT ptClock{rcClock.left, rcClock.top};
            ScreenToClient(g_trayNotifyWnd, &ptClock);
            if (ptClock.x < anchorLeft) {
                anchorLeft = ptClock.x;
            }
        }
    }

    LONG gap = anchorLeft - scan.lastIconRight;
    if (gap <= g_maxGapPx) {
        return;
    }

    // Leave a 2px breathing room; cap the shift so a misdetected layout
    // can never throw windows across the screen.
    int delta = static_cast<int>(gap) - 2;
    if (delta <= 0 || delta > 200) {
        return;
    }

    Wh_Log(L"Closing tray gap of %d px (delta %d)", static_cast<int>(gap), delta);

    ShiftCtx ctx{anchorLeft, delta};
    EnumChildWindows(g_trayNotifyWnd, ShiftRightSideProc,
                     reinterpret_cast<LPARAM>(&ctx));
}

LRESULT CALLBACK TrayNotifyWndSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam,
                                           LPARAM lParam) {
    LRESULT result =
        CallWindowProcW(g_origTrayNotifyProc, hWnd, uMsg, wParam, lParam);
    if (uMsg == WM_WINDOWPOSCHANGED || uMsg == WM_SIZE) {
        CloseTrayGap();
    }
    return result;
}

void HookTray() {
    HWND shellTray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!shellTray) {
        Wh_Log(L"Shell_TrayWnd not found");
        return;
    }
    HWND trayNotify =
        FindWindowExW(shellTray, nullptr, L"TrayNotifyWnd", nullptr);
    if (!trayNotify) {
        Wh_Log(L"TrayNotifyWnd not found");
        return;
    }
    g_trayNotifyWnd = trayNotify;
    g_origTrayNotifyProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        trayNotify, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(TrayNotifyWndSubclassProc)));
    if (!g_origTrayNotifyProc) {
        Wh_Log(L"Failed to subclass TrayNotifyWnd, error %lu", GetLastError());
        g_trayNotifyWnd = nullptr;
        return;
    }
    Wh_Log(L"Subclassed TrayNotifyWnd");
    CloseTrayGap();
}

void UnhookTray() {
    if (g_trayNotifyWnd && g_origTrayNotifyProc &&
        IsWindow(g_trayNotifyWnd)) {
        SetWindowLongPtrW(g_trayNotifyWnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(g_origTrayNotifyProc));
    }
    g_trayNotifyWnd = nullptr;
    g_origTrayNotifyProc = nullptr;
}

void LoadSettings() {
    g_enabled = Wh_GetIntSetting(L"enabled") != 0;
    int maxGap = Wh_GetIntSetting(L"maxGapPx");
    if (maxGap < 0) {
        maxGap = 0;
    }
    if (maxGap > 500) {
        maxGap = 500;
    }
    g_maxGapPx = maxGap;
}

}  // namespace

BOOL Wh_ModInit(void) {
    Wh_Log(L"Init");
    LoadSettings();
    HookTray();
    return TRUE;
}

void Wh_ModUninit(void) {
    Wh_Log(L"Uninit");
    UnhookTray();
}

void Wh_ModSettingsChanged(void) {
    Wh_Log(L"Settings changed");
    bool wasEnabled = g_enabled;
    LoadSettings();
    if (g_enabled && !wasEnabled) {
        HookTray();
    } else if (!g_enabled && wasEnabled) {
        UnhookTray();
    } else if (g_enabled) {
        CloseTrayGap();
    }
}
