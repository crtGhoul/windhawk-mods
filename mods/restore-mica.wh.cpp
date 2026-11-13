// ==WindhawkMod==
// @id              restore-mica
// @name            Restore Mica
// @description     Force Mica (or Acrylic/Tabbed) backdrop on Explorer windows where themes leave it black/broken
// @version         1.0
// @author          crtGhoul
// @github          https://github.com/crtGhoul
// @license         MIT
// @include         explorer.exe
// @compilerOptions -ldwmapi
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Restore Mica

Forces a DWM system backdrop (Mica by default) on File Explorer windows
(`CabinetWClass`) where visual themes or skinning tools leave the
background black or broken. Uses `DwmSetWindowAttribute` with
`DWMWA_SYSTEMBACKDROP_TYPE` — the documented, theme-friendly way to get
Mica back, instead of the fragile frame-extension hacks.

- Watches for new Explorer windows with a WinEvent hook
  (`EVENT_OBJECT_CREATE`..`EVENT_OBJECT_SHOW`) and applies the chosen
  backdrop immediately.
- Also applies to already-open Explorer windows when the mod loads.
- Optionally forces immersive dark mode on the window frame as well.
- On unload (or when disabled), every touched window is set back to
  `DWMSBT_AUTO`.

Setting values map to the real DWM enum (not 1:1): 0 = auto
(`DWMSBT_AUTO`), 1 = Mica (`DWMSBT_MAINWINDOW`), 2 = Acrylic
(`DWMSBT_TRANSIENTWINDOW`), 3 = Tabbed (`DWMSBT_TABBEDWINDOW`).

## Settings

- **Enabled** — master switch. Off restores all touched windows to auto.
- **Backdrop type** — 0 auto, 1 mica, 2 acrylic, 3 tabbed.
- **Dark backdrop** — also force immersive dark mode on the frame.

## Honest limitations

- Backdrop availability varies by Windows build (Tabbed needs a recent
  11 build); an unsupported type silently falls back to whatever DWM
  decides.
- Immersive dark mode is deliberately NOT reverted on unload: Explorer
  sets it itself from the system theme, and forcing it off could break
  the stock dark frame.
- If another backdrop mod is active, the two will fight over the same
  attribute — disable one of them.
- Effect confirmed only by reading the DWM docs; needs a real-PC check
  that black-background Explorer windows actually render Mica.

## Testing status

Type-checked only with x86_64-w64-mingw32-g++ -fsyntax-only against the
real windhawk_api.h (Windhawk v1.7.3); NOT compiled with Windhawk's real
compiler; NOT tested on a real Windows machine.
*/
// ==WindhawkModSettings==
/*
- enabled: true
  $name: Enabled
  $description: Master switch. When off, touched windows are restored to auto backdrop.
- backdropType: 1
  $name: Backdrop type
  $description: 0 auto, 1 mica, 2 acrylic, 3 tabbed
- darkBackdrop: true
  $name: Dark backdrop
  $description: Also force immersive dark mode on the window frame.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <dwmapi.h>

#include <string>
#include <unordered_set>

// MinGW's dwmapi.h may not define these yet; values are from the SDK.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMSBT_AUTO
#define DWMSBT_AUTO 0
#define DWMSBT_NONE 1
#define DWMSBT_MAINWINDOW 2
#define DWMSBT_TRANSIENTWINDOW 3
#define DWMSBT_TABBEDWINDOW 4
#endif

namespace {

BOOL g_enabled = TRUE;
int g_backdropType = 1;
BOOL g_darkBackdrop = TRUE;

HWINEVENTHOOK g_hook = nullptr;
std::unordered_set<HWND> g_handled;
CRITICAL_SECTION g_cs;
BOOL g_csInit = FALSE;

// Settings use 0=auto, 1=mica, 2=acrylic, 3=tabbed; the DWM enum is
// 0=auto, 1=none, 2=mica(main), 3=acrylic(transient), 4=tabbed.
int MapBackdropType(int v) {
    switch (v) {
        case 0:
            return DWMSBT_AUTO;
        case 1:
            return DWMSBT_MAINWINDOW;
        case 2:
            return DWMSBT_TRANSIENTWINDOW;
        case 3:
            return DWMSBT_TABBEDWINDOW;
        default:
            return DWMSBT_MAINWINDOW;
    }
}

bool IsExplorerWindow(HWND hwnd) {
    wchar_t cls[64] = {};
    if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) == 0) {
        return false;
    }
    return wcscmp(cls, L"CabinetWClass") == 0;
}

void ApplyBackdrop(HWND hwnd) {
    int type = MapBackdropType(g_backdropType);
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &type, sizeof(type));
    if (g_darkBackdrop) {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                              sizeof(dark));
    }
    EnterCriticalSection(&g_cs);
    g_handled.insert(hwnd);
    LeaveCriticalSection(&g_cs);
}

void RevertBackdrop(HWND hwnd) {
    int type = DWMSBT_AUTO;
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &type, sizeof(type));
    // Immersive dark mode intentionally left alone (see readme).
}

void CALLBACK WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD dwEvent,
                           HWND hwnd, LONG idObject, LONG idChild,
                           DWORD dwEventThread, DWORD dwmsEventTime) {
    (void)hWinEventHook;
    (void)dwEvent;
    (void)dwEventThread;
    (void)dwmsEventTime;
    if (!g_enabled) {
        return;
    }
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) {
        return;
    }
    if (!IsExplorerWindow(hwnd)) {
        return;
    }
    ApplyBackdrop(hwnd);
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    (void)lParam;
    if (g_enabled && IsExplorerWindow(hwnd)) {
        ApplyBackdrop(hwnd);
    }
    return TRUE;
}

void ApplyToExisting() {
    EnumWindows(EnumWindowsProc, 0);
}

void RevertAll() {
    EnterCriticalSection(&g_cs);
    std::unordered_set<HWND> copy = g_handled;
    g_handled.clear();
    LeaveCriticalSection(&g_cs);
    for (HWND hwnd : copy) {
        if (IsWindow(hwnd)) {
            RevertBackdrop(hwnd);
        }
    }
}

void LoadSettings() {
    g_enabled = Wh_GetIntSetting(L"enabled");
    g_backdropType = Wh_GetIntSetting(L"backdropType");
    g_darkBackdrop = Wh_GetIntSetting(L"darkBackdrop");
}

}  // namespace

BOOL Wh_ModInit(void) {
    InitializeCriticalSection(&g_cs);
    g_csInit = TRUE;
    LoadSettings();

    if (g_enabled) {
        ApplyToExisting();
    }

    g_hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, nullptr,
                             WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (!g_hook) {
        Wh_Log(L"SetWinEventHook failed");
        return FALSE;
    }
    return TRUE;
}

void Wh_ModUninit(void) {
    if (g_hook) {
        UnhookWinEvent(g_hook);
        g_hook = nullptr;
    }
    RevertAll();
    if (g_csInit) {
        DeleteCriticalSection(&g_cs);
        g_csInit = FALSE;
    }
}

void Wh_ModSettingsChanged(void) {
    BOOL wasEnabled = g_enabled;
    LoadSettings();
    if (g_enabled && !wasEnabled) {
        ApplyToExisting();
    } else if (!g_enabled && wasEnabled) {
        RevertAll();
    } else if (g_enabled) {
        ApplyToExisting();  // Re-apply in case the backdrop type changed.
    }
}
