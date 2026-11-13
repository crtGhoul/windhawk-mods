// ==WindhawkMod==
// @id              double-click-debouncer
// @name            Double-Click Debouncer
// @description     Swallow phantom second clicks from worn-out mice (configurable window, default 50ms)
// @version         1.0
// @author          crtGhoul
// @github          https://github.com/crtGhoul
// @license         MIT
// @include         explorer.exe
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Double-Click Debouncer

Worn-out mouse switches "bounce": one physical press registers as two
clicks a few milliseconds apart, causing accidental double-opens,
double-deletes, and phantom drags. This mod swallows the phantom second
click.

How it works: a single low-level mouse hook (`WH_MOUSE_LL`, installed
from explorer.exe so only one instance covers the whole system) tracks,
per button (left/right/middle), the time and position of the last
button-up. If a button-down arrives within the debounce window AND close
to where the last click ended, it is a bounce: the down (and its
matching up) are swallowed. Everything else — wheel, motion, real
clicks — passes through untouched, decided synchronously with zero
added input latency.

Why genuine double-clicks are safe: the default 50 ms window is an
order of magnitude below Windows' double-click time (~500 ms). A human
cannot physically double-click in 50 ms, so intentional double-clicks
always pass.

## Settings

- **Enabled** — master switch.
- **Debounce ms** — bounce window, 10–200 (default 50).
- **Max distance px** — the phantom down must land within this many
  pixels of the last up (default 8).

## Honest limitations

- Only fixes bounce on down/up pairs; a switch that chatters
  *while held* (repeat downs without ups) is a different failure mode
  and is not handled.
- A bounce slower than your window, or a deliberate click faster than
  your window, will be misclassified — tune the window to your mouse.
- `WH_MOUSE_LL` runs on a system hook thread; the handler is O(1) per
  event, but uninstalling is the only way to fully remove it — done
  cleanly on unload.

## Testing status

Type-checked only with x86_64-w64-mingw32-g++ -fsyntax-only against the
real windhawk_api.h (Windhawk v1.7.3); NOT compiled with Windhawk's real
compiler; NOT tested on a real Windows machine.
*/
// ==WindhawkModSettings==
/*
- enabled: true
  $name: Enabled
  $description: Master switch. When off, every click passes through.
- debounceMs: 50
  $name: Debounce ms
  $description: Bounce window in milliseconds (10-200). Genuine double-clicks (~500ms) always pass.
- maxDistancePx: 8
  $name: Max distance px
  $description: Phantom click must land within this many pixels of the previous click.
*/
// ==/WindhawkModSettings==

#include <windows.h>

namespace {

BOOL g_enabled = TRUE;
int g_debounceMs = 50;
int g_maxDistancePx = 8;

HHOOK g_hook = nullptr;

struct ButtonState {
    ULONGLONG lastUpTime = 0;
    POINT lastUpPos = {};
    bool swallowNextUp = false;
};

ButtonState g_buttons[3];  // 0=left, 1=right, 2=middle

LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_enabled) {
        const MSLLHOOKSTRUCT *ms = (const MSLLHOOKSTRUCT *)lParam;
        int button = -1;
        bool isDown = false;
        switch (wParam) {
            case WM_LBUTTONDOWN:
                button = 0;
                isDown = true;
                break;
            case WM_LBUTTONUP:
                button = 0;
                isDown = false;
                break;
            case WM_RBUTTONDOWN:
                button = 1;
                isDown = true;
                break;
            case WM_RBUTTONUP:
                button = 1;
                isDown = false;
                break;
            case WM_MBUTTONDOWN:
                button = 2;
                isDown = true;
                break;
            case WM_MBUTTONUP:
                button = 2;
                isDown = false;
                break;
            default:
                break;
        }

        if (button >= 0) {
            ButtonState &st = g_buttons[button];
            if (isDown) {
                ULONGLONG now = GetTickCount64();
                if (st.lastUpTime != 0 &&
                    now - st.lastUpTime < (ULONGLONG)g_debounceMs) {
                    LONG dx = ms->pt.x - st.lastUpPos.x;
                    LONG dy = ms->pt.y - st.lastUpPos.y;
                    LONG maxD = (LONG)g_maxDistancePx;
                    if (dx * dx + dy * dy <= maxD * maxD) {
                        // Phantom bounce: swallow this down and its up.
                        st.swallowNextUp = true;
                        return 1;
                    }
                }
            } else {
                if (st.swallowNextUp) {
                    st.swallowNextUp = false;
                    return 1;
                }
                st.lastUpTime = GetTickCount64();
                st.lastUpPos = ms->pt;
            }
        }
    }
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

void ResetState() {
    for (int i = 0; i < 3; ++i) {
        g_buttons[i].lastUpTime = 0;
        g_buttons[i].lastUpPos = {};
        g_buttons[i].swallowNextUp = false;
    }
}

void LoadSettings() {
    g_enabled = Wh_GetIntSetting(L"enabled");
    g_debounceMs = Wh_GetIntSetting(L"debounceMs");
    g_maxDistancePx = Wh_GetIntSetting(L"maxDistancePx");
    if (g_debounceMs < 0) {
        g_debounceMs = 0;
    }
    if (g_debounceMs > 1000) {
        g_debounceMs = 1000;
    }
    if (g_maxDistancePx < 0) {
        g_maxDistancePx = 0;
    }
}

}  // namespace

BOOL Wh_ModInit(void) {
    LoadSettings();
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, nullptr, 0);
    if (!g_hook) {
        Wh_Log(L"SetWindowsHookEx(WH_MOUSE_LL) failed");
        return FALSE;
    }
    return TRUE;
}

void Wh_ModUninit(void) {
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    ResetState();
}

void Wh_ModSettingsChanged(void) {
    LoadSettings();
    ResetState();  // Never leave a stuck swallow flag across a retune.
}
