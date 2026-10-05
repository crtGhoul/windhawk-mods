// ==WindhawkMod==
// @id              dark-menus-plus
// @name            Dark Menus Plus
// @description     Force dark titlebars and dark dialog backgrounds on classic dialogs and message boxes, system-wide
// @version         1.0
// @author          crtGhoul
// @github          https://github.com/crtGhoul
// @license         MIT
// @include         *
// @compilerOptions -ldwmapi -luxtheme -lgdi32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Dark Menus Plus

Forces dark titlebars and dark dialog backgrounds on classic Win32 dialogs
and message boxes (window class `#32770`), system-wide. This is an
independent take focused on classic dialogs — `dark-menus` itself is
maintained-but-slow (v1.4.2), and issue #2422 ("Dark mode for win32 classic
message box and Task Dialogs") shows the demand this covers. It does not
touch context menus; that is `dark-menus` territory.

- Hooks `CreateWindowExW` (one hook, event-driven). Non-dialog windows take
  a cheap early-out: a single class-name check before the original call.
- For `#32770` windows: applies `DwmSetWindowAttribute(...,
  DWMWA_USE_IMMERSIVE_DARK_MODE=20, ...)` (-ldwmapi) for the dark titlebar,
  plus `SetWindowTheme(hwnd, L"DarkMode_Explorer", NULL)` (-luxtheme) so
  buttons/scrollbars render dark.
- If **Dark dialog background** is on, the dialog is subclassed (classic
  `SetWindowLongPtr`, user32 only — no extra link deps) and
  `WM_CTLCOLORDLG` / `WM_CTLCOLORSTATIC` / `WM_CTLCOLOREDIT` return a shared
  dark brush with light text. The subclass removes itself on
  `WM_NCDESTROY`.
- `@include *` tradeoff (documented deliberately): dialogs can be created by
  any process, so the hook must be system-wide. The per-process cost is one
  hooked API with a near-zero early-out; own-exe exclusion
  (`excludedExes`, default `cmd.exe,conhost.exe`) disables the mod entirely
  in matching processes at init — no hooks installed there at all.
- Clean unload: hook removed, shared brush deleted.

## Settings

- **Enabled** — master switch.
- **Dark dialog background** — also recolor dialog backgrounds/controls dark
  (titlebar darkening always applies).
- **Excluded exes** — comma-separated basenames; matching processes install
  no hooks at all. Default: `cmd.exe,conhost.exe`.

## Honest limitation

Some applications owner-draw their dialogs or set their own brushes in
`WM_CTLCOLOR*`, which overrides this mod's brush for those controls — the
titlebar still goes dark, but control backgrounds follow the app. Immersive
(XAML/WinUI) dialogs are not `#32770` and are untouched. System-wide API
hooking is inherently heavier than a single-process mod; the early-out keeps
it cheap, but measure if you are sensitive.

## Testing status

Type-checked only with x86_64-w64-mingw32-g++ -fsyntax-only against the real
windhawk_api.h (Windhawk v1.7.3); NOT compiled with Windhawk's real compiler;
NOT tested on a real Windows machine. Real-Windows verification needed: dark
rendering of real message boxes (e.g. Notepad's About box), no interference
with owner-drawn dialogs, excluded exes genuinely hook-free, and clean unload
with dialogs open.
*/
// ==WindhawkModSettings==
/*
- enabled: true
  $name: Enabled
- darkDialogBackground: true
  $name: Dark dialog background
  $description: Also recolor dialog backgrounds and controls dark
- excludedExes: "cmd.exe,conhost.exe"
  $name: Excluded exes
  $description: Comma-separated basenames; matching processes install no hooks
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <string>
#include <vector>

// DWMWA_USE_IMMERSIVE_DARK_MODE = 20 on Windows 10 1809+.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

namespace {

using CreateWindowExW_t = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int,
                                        int, int, int, HWND, HMENU, HINSTANCE,
                                        LPVOID);
CreateWindowExW_t g_pfnCreateWindowExW = nullptr;

HBRUSH g_hDarkBrush = nullptr;
bool g_enabled = true;
bool g_darkDialogBackground = true;
std::vector<std::wstring> g_excludedExes;

bool ExeIsExcluded() {
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return false;
    }
    const wchar_t* base = wcsrchr(path, L'\\');
    std::wstring exe = base ? (base + 1) : path;
    for (const auto& e : g_excludedExes) {
        if (!e.empty() && _wcsicmp(e.c_str(), exe.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

LRESULT CALLBACK DialogSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam) {
    WNDPROC oldProc = reinterpret_cast<WNDPROC>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkColor(hdc, RGB(0x1F, 0x1F, 0x1F));
            SetTextColor(hdc, RGB(0xE6, 0xE6, 0xE6));
            return reinterpret_cast<LRESULT>(g_hDarkBrush);
        }
        case WM_NCDESTROY:
            if (oldProc) {
                SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                  reinterpret_cast<LONG_PTR>(oldProc));
            }
            break;
        default:
            break;
    }
    return oldProc ? CallWindowProcW(oldProc, hwnd, msg, wParam, lParam)
                   : DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ApplyDarkDialog(HWND hwnd) {
    BOOL useDark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark,
                          sizeof(useDark));
    SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);

    if (g_darkDialogBackground && g_hDarkBrush) {
        WNDPROC oldProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            hwnd, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(&DialogSubclassProc)));
        if (oldProc && oldProc != &DialogSubclassProc) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(oldProc));
        }
    }
}

HWND WINAPI CreateWindowExW_Hook(DWORD dwExStyle, LPCWSTR lpClassName,
                                 LPCWSTR lpWindowName, DWORD dwStyle, int x,
                                 int y, int nWidth, int nHeight,
                                 HWND hWndParent, HMENU hMenu,
                                 HINSTANCE hInstance, LPVOID lpParam) {
    // Cheap early-out for everything that isn't a classic dialog/message box.
    bool isDialog = (lpClassName != nullptr) &&
                    !IS_INTRESOURCE(lpClassName) &&
                    (wcscmp(lpClassName, L"#32770") == 0);
    HWND hwnd = g_pfnCreateWindowExW(dwExStyle, lpClassName, lpWindowName,
                                     dwStyle, x, y, nWidth, nHeight,
                                     hWndParent, hMenu, hInstance, lpParam);
    if (isDialog && hwnd && g_enabled) {
        ApplyDarkDialog(hwnd);
    }
    return hwnd;
}

void LoadSettings() {
    g_enabled = Wh_GetIntSetting(L"enabled") != 0;
    g_darkDialogBackground = Wh_GetIntSetting(L"darkDialogBackground") != 0;
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
    if (ExeIsExcluded()) {
        Wh_Log(L"Dark Menus Plus: own exe excluded, no hooks installed");
        return TRUE;
    }
    g_hDarkBrush = CreateSolidBrush(RGB(0x1F, 0x1F, 0x1F));
    if (!g_hDarkBrush) {
        Wh_Log(L"CreateSolidBrush failed");
        return FALSE;
    }
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (!hUser32) {
        Wh_Log(L"GetModuleHandleW(user32.dll) failed");
        return FALSE;
    }
    void* target =
        reinterpret_cast<void*>(GetProcAddress(hUser32, "CreateWindowExW"));
    if (!target) {
        Wh_Log(L"GetProcAddress(CreateWindowExW) failed");
        return FALSE;
    }
    if (!Wh_SetFunctionHook(target,
                            reinterpret_cast<void*>(&CreateWindowExW_Hook),
                            reinterpret_cast<void**>(&g_pfnCreateWindowExW))) {
        Wh_Log(L"Wh_SetFunctionHook(CreateWindowExW) failed");
        return FALSE;
    }
    Wh_Log(L"Dark Menus Plus initialized");
    return TRUE;
}

void Wh_ModUninit() {
    if (g_pfnCreateWindowExW) {
        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        if (hUser32) {
            void* target = reinterpret_cast<void*>(
                GetProcAddress(hUser32, "CreateWindowExW"));
            if (target) {
                Wh_RemoveFunctionHook(target);
            }
        }
        g_pfnCreateWindowExW = nullptr;
    }
    if (g_hDarkBrush) {
        DeleteObject(g_hDarkBrush);
        g_hDarkBrush = nullptr;
    }
    Wh_Log(L"Dark Menus Plus uninitialized");
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}
