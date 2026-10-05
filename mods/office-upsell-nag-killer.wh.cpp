// ==WindhawkMod==
// @id              office-upsell-nag-killer
// @name            Office Upsell Nag Killer
// @description     Hides "Upgrade Your Plan" banners in Microsoft 365 apps via conservative heuristic string matching
// @version         1.0
// @author          crtGhoul
// @github          https://github.com/crtGhoul
// @license         MIT
// @include         winword.exe
// @include         excel.exe
// @include         powerpnt.exe
// @include         outlook.exe
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Office Upsell Nag Killer

Hides "Upgrade Your Plan" / "Go Premium" style banner strips inside the
Microsoft 365 desktop apps (Word, Excel, PowerPoint, Outlook).

How it works: an in-process WinEvent hook watches for created/shown
windows. A candidate is hidden only when ALL of these hold:

- it is not a top-level window (never the main window, ribbon, or document area),
- it is short (below the max banner height) but wide (a strip, not a dialog),
- its parent chain reaches the app's visible main window,
- its window text contains one of the configured match phrases
  (case-insensitive).

A CreateWindowExW hook applies the same test as a backup path for windows
that appear without going through the WinEvent route.

## Settings

- **Enabled** — master switch.
- **Match strings** — semicolon-separated, case-insensitive substrings.
- **Max banner height (px)** — windows taller than this are never touched.

## Honest limitations

- Heuristic string matching can both miss reworded banners and hide
  legitimate UI that happens to contain the same phrases. Keep the match
  list tight.
- The CreateWindowExW backup hook only sees windows created after the mod
  loads; the WinEvent EVENT_OBJECT_SHOW path covers the rest. No polling
  is used anywhere.

## Testing status

Type-checked only with x86_64-w64-mingw32-g++ -fsyntax-only against the
real windhawk_api.h (Windhawk v1.7.3); NOT compiled with Windhawk's real
compiler; NOT tested on a real Windows machine.
*/
// ==WindhawkModSettings==
/*
- enabled: true
  $name: Enabled
  $description: Master switch. When off, nothing is hidden.
- matchStrings: "upgrade your plan;go premium;try microsoft 365;upgrade to premium"
  $name: Match strings
  $description: Semicolon-separated, case-insensitive substrings.
- maxBannerHeightPx: 160
  $name: Max banner height (px)
  $description: Windows taller than this are never touched.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <cwctype>
#include <string>
#include <vector>

namespace {

BOOL g_enabled = TRUE;
int g_maxBannerHeightPx = 160;
std::vector<std::wstring> g_matchStrings;
HWINEVENTHOOK g_winEventHook = nullptr;

using CreateWindowExW_t = decltype(&CreateWindowExW);
CreateWindowExW_t g_createWindowExW_Original = nullptr;

std::wstring ToLower(std::wstring s) {
    for (auto &ch : s) {
        ch = std::towlower(ch);
    }
    return s;
}

std::wstring Trim(const std::wstring &s) {
    size_t begin = 0;
    while (begin < s.size() && std::iswspace(s[begin])) {
        ++begin;
    }
    size_t end = s.size();
    while (end > begin && std::iswspace(s[end - 1])) {
        --end;
    }
    return s.substr(begin, end - begin);
}

void LoadSettings() {
    g_enabled = Wh_GetIntSetting(L"enabled");
    g_maxBannerHeightPx = Wh_GetIntSetting(L"maxBannerHeightPx");

    g_matchStrings.clear();
    PCWSTR raw = Wh_GetStringSetting(L"matchStrings");
    std::wstring all(raw ? raw : L"");
    Wh_FreeStringSetting(raw);

    size_t start = 0;
    while (start <= all.size()) {
        size_t semi = all.find(L';', start);
        std::wstring part = Trim(all.substr(
            start, semi == std::wstring::npos ? std::wstring::npos : semi - start));
        if (!part.empty()) {
            g_matchStrings.push_back(ToLower(part));
        }
        if (semi == std::wstring::npos) {
            break;
        }
        start = semi + 1;
    }
}

// Returns true if the window was hidden.
bool MaybeHideNag(HWND hwnd) {
    if (!g_enabled || g_matchStrings.empty()) {
        return false;
    }
    if (!IsWindow(hwnd)) {
        return false;
    }

    // (a) Never touch a top-level window: not the main window, ribbon,
    // or document area.
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root || root == hwnd) {
        return false;
    }

    // (d) The parent chain must reach the app's visible main window, and
    // the candidate must actually be a child control.
    if (!IsWindowVisible(root)) {
        return false;
    }
    if (!GetParent(hwnd) && !(GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD)) {
        return false;
    }

    // (b)(c) Banner-shaped: a short strip, reasonably wide.
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) {
        return false;
    }
    LONG width = rc.right - rc.left;
    LONG height = rc.bottom - rc.top;
    if (height <= 0 || height >= g_maxBannerHeightPx) {
        return false;
    }
    if (width <= 300) {
        return false;
    }

    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0 || len > 2048) {
        return false;
    }
    std::vector<wchar_t> buf(static_cast<size_t>(len) + 1);
    GetWindowTextW(hwnd, buf.data(), len + 1);
    std::wstring text = ToLower(buf.data());

    for (const auto &m : g_matchStrings) {
        if (wcsstr(text.c_str(), m.c_str())) {
            ShowWindow(hwnd, SW_HIDE);
            Wh_Log(L"Hid upsell banner (matched \"%s\")", m.c_str());
            return true;
        }
    }
    return false;
}

void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                           LONG idObject, LONG idChild, DWORD thread,
                           DWORD time) {
    (void)hook;
    (void)event;
    (void)thread;
    (void)time;
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) {
        return;
    }
    if (!hwnd) {
        return;
    }
    MaybeHideNag(hwnd);
}

HWND WINAPI CreateWindowExW_Hook(DWORD exStyle, LPCWSTR className,
                                 LPCWSTR windowName, DWORD style, int x, int y,
                                 int w, int h, HWND parent, HMENU menu,
                                 HINSTANCE inst, LPVOID param) {
    HWND hwnd = g_createWindowExW_Original(exStyle, className, windowName, style,
                                           x, y, w, h, parent, menu, inst,
                                           param);
    if (hwnd) {
        MaybeHideNag(hwnd);
    }
    return hwnd;
}

} // namespace

BOOL Wh_ModInit(void) {
    LoadSettings();

    g_winEventHook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW,
                                     nullptr, WinEventProc, 0, 0,
                                     WINEVENT_OUTOFCONTEXT);
    if (!g_winEventHook) {
        Wh_Log(L"SetWinEventHook failed, relying on CreateWindowExW hook only");
    }

    if (!Wh_SetFunctionHook((void *)CreateWindowExW,
                            (void *)CreateWindowExW_Hook,
                            (void **)&g_createWindowExW_Original)) {
        Wh_Log(L"CreateWindowExW hook failed");
    }

    return TRUE;
}

void Wh_ModUninit(void) {
    if (g_winEventHook) {
        UnhookWinEvent(g_winEventHook);
        g_winEventHook = nullptr;
    }
    Wh_RemoveFunctionHook((void *)CreateWindowExW);
}

void Wh_ModSettingsChanged(void) {
    LoadSettings();
}
