// ==WindhawkMod==
// @id              taskbar-close-all-windows
// @name            Taskbar Close All Windows
// @description     Add "Close all windows" to right-click menus of uncombined taskbar buttons
// @version         1.0
// @author          crtGhoul
// @github          https://github.com/crtGhoul
// @license         MIT
// @include         explorer.exe
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Taskbar Close All Windows

Windows only offers "Close all windows" on *combined* taskbar buttons.
With "Never combine", each button's right-click menu has just "Close
window". This mod adds a **Close all windows** item to those menus; it
posts `WM_CLOSE` to every visible top-level window of the same app.

How it works (heuristic — read the limitations):

1. A `WH_MOUSE_LL` hook records when the user right-clicks on the
   taskbar (`Shell_TrayWnd`).
2. A `TrackPopupMenuEx` hook inspects popup menus owned by the taskbar;
   if one contains a "Close window" item and a taskbar right-click
   happened < 2 s ago, the mod figures out which app the menu belongs
   to and appends **Close all windows**.
3. The menu owner's window proc is subclassed for the lifetime of that
   one modal menu; picking the new item closes the app's windows, then
   everything is unsubclassed immediately.

App identification is the fragile part: the mod reads the menu's first
item (usually the app/window name), matches it against visible window
titles, falling back to exe basenames. `explorer.exe` itself can never
be a target (hardcoded guard), and `excludedExes` lets you protect
other apps.

## Settings

- **Enabled** — master switch.
- **Excluded exes** — comma-separated exe basenames never closed by
  this mod, e.g. `firefox.exe, code.exe`.

## Honest limitations

- The app-matching is a heuristic (menu sniffing). If the menu's first
  item matches nothing — or matches ambiguously — the item is simply
  not added (fail-closed), but on an unusual menu it could in theory
  match the wrong app. The < 2 s freshness gate and the explorer.exe
  guard bound the blast radius.
- Only visible top-level windows get `WM_CLOSE`; apps that ignore it
  (or show "save changes?" prompts) behave as if you closed each
  window by hand.
- Closing is by process: all windows of every matched PID are closed.
- Needs a real-PC check on current builds that the appended item shows
  up in the uncombined-button menu and closes exactly the right app.

## Testing status

Type-checked only with x86_64-w64-mingw32-g++ -fsyntax-only against the
real windhawk_api.h (Windhawk v1.7.3); NOT compiled with Windhawk's real
compiler; NOT tested on a real Windows machine.
*/
// ==WindhawkModSettings==
/*
- enabled: true
  $name: Enabled
  $description: Master switch. When off, no menu items are added.
- excludedExes: ""
  $name: Excluded exes
  $description: Comma-separated exe basenames never closed by this mod (e.g. firefox.exe, code.exe).
*/
// ==/WindhawkModSettings==

#include <windows.h>

#include <cwctype>
#include <string>
#include <vector>

namespace {

BOOL g_enabled = TRUE;
std::vector<std::wstring> g_excluded;  // lowercased, with ".exe"

HHOOK g_mouseHook = nullptr;
HWND g_trayWnd = nullptr;
ULONGLONG g_lastTrayClick = 0;  // 0 = none recorded

constexpr UINT kCloseAllCmd = 0xE000;

using TrackPopupMenuEx_t = decltype(&TrackPopupMenuEx);
TrackPopupMenuEx_t g_tpmOriginal = nullptr;
BOOL g_tpmHooked = FALSE;

WNDPROC g_origMenuProc = nullptr;
HWND g_menuOwner = nullptr;
bool g_subclassActive = false;
std::vector<DWORD> g_targetPids;

std::wstring ToLowerW(const std::wstring &s) {
    std::wstring out = s;
    for (auto &ch : out) {
        ch = std::towlower(ch);
    }
    return out;
}

std::wstring TrimW(const std::wstring &s) {
    size_t b = 0;
    while (b < s.size() && iswspace(s[b])) {
        ++b;
    }
    size_t e = s.size();
    while (e > b && iswspace(s[e - 1])) {
        --e;
    }
    return s.substr(b, e - b);
}

std::wstring StripAmpersands(const std::wstring &s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t ch : s) {
        if (ch != L'&') {
            out.push_back(ch);
        }
    }
    return out;
}

bool ContainsCI(const std::wstring &haystack, const std::wstring &needle) {
    if (needle.empty()) {
        return false;
    }
    std::wstring h = ToLowerW(haystack);
    std::wstring n = ToLowerW(needle);
    return wcsstr(h.c_str(), n.c_str()) != nullptr;
}

std::wstring BaseName(const std::wstring &path) {
    size_t i = path.find_last_of(L"\\/");
    return (i == std::wstring::npos) ? path : path.substr(i + 1);
}

bool ExePathFromPid(DWORD pid, std::wstring &pathOut) {
    HANDLE h =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        return false;
    }
    wchar_t buf[MAX_PATH] = {};
    DWORD size = ARRAYSIZE(buf);
    bool ok = QueryFullProcessImageNameW(h, 0, buf, &size) != FALSE;
    CloseHandle(h);
    if (ok) {
        pathOut.assign(buf, size);
    }
    return ok;
}

bool IsExcluded(const std::wstring &exeLower) {
    for (const std::wstring &e : g_excluded) {
        if (exeLower == e) {
            return true;
        }
    }
    return false;
}

bool MenuHasCloseWindowItem(HMENU hMenu) {
    int count = GetMenuItemCount(hMenu);
    for (int i = 0; i < count; ++i) {
        wchar_t buf[256] = {};
        int len = GetMenuStringW(hMenu, i, buf, ARRAYSIZE(buf), MF_BYPOSITION);
        if (len > 0 && ContainsCI(std::wstring(buf, (size_t)len),
                                  L"close window")) {
            return true;
        }
    }
    return false;
}

struct WindowInfo {
    DWORD pid = 0;
    std::wstring exeLower;    // basename, e.g. L"notepad.exe"
    std::wstring titleLower;  // window title
};

BOOL CALLBACK CollectWindowsProc(HWND hwnd, LPARAM lParam) {
    auto *out = (std::vector<WindowInfo> *)lParam;
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    if (wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Progman") == 0) {
        return TRUE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) {
        return TRUE;
    }
    WindowInfo wi;
    wi.pid = pid;
    std::wstring path;
    if (ExePathFromPid(pid, path)) {
        wi.exeLower = ToLowerW(BaseName(path));
    }
    wchar_t title[256] = {};
    GetWindowTextW(hwnd, title, ARRAYSIZE(title));
    wi.titleLower = ToLowerW(title);
    out->push_back(std::move(wi));
    return TRUE;
}

// Heuristic: the menu's first item is usually the app or window name.
// Match it against visible window titles first, then exe basenames.
// Returns false (fail-closed) on no match, ambiguity guard, or a
// protected target (explorer.exe / excluded list).
bool ResolveTargetFromMenu(HMENU hMenu, std::vector<DWORD> &pidsOut) {
    wchar_t raw[256] = {};
    int len = GetMenuStringW(hMenu, 0, raw, ARRAYSIZE(raw), MF_BYPOSITION);
    if (len <= 0) {
        return false;
    }
    std::wstring name =
        TrimW(StripAmpersands(std::wstring(raw, (size_t)len)));
    name = ToLowerW(name);
    if (name.empty()) {
        return false;
    }

    std::vector<WindowInfo> wins;
    EnumWindows(CollectWindowsProc, (LPARAM)&wins);

    // Pass 1: exact window-title match (covers "doc1 - Notepad" style).
    for (const WindowInfo &w : wins) {
        if (!w.titleLower.empty() && w.titleLower == name) {
            if (w.exeLower == L"explorer.exe" || IsExcluded(w.exeLower)) {
                return false;
            }
            pidsOut.push_back(w.pid);
            return true;
        }
    }

    // Pass 2: exe basename match, with or without extension.
    std::vector<DWORD> hit;
    for (const WindowInfo &w : wins) {
        if (w.exeLower.empty()) {
            continue;
        }
        std::wstring noext = w.exeLower;
        size_t dot = noext.rfind(L'.');
        if (dot != std::wstring::npos) {
            noext = noext.substr(0, dot);
        }
        if (w.exeLower == name || noext == name) {
            if (w.exeLower == L"explorer.exe" || IsExcluded(w.exeLower)) {
                return false;
            }
            bool seen = false;
            for (DWORD p : hit) {
                if (p == w.pid) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                hit.push_back(w.pid);
            }
        }
    }
    if (hit.empty()) {
        return false;
    }
    pidsOut = std::move(hit);
    return true;
}

struct CloseContext {
    const std::vector<DWORD> *pids;
};

BOOL CALLBACK CloseWindowsProc(HWND hwnd, LPARAM lParam) {
    const CloseContext *ctx = (const CloseContext *)lParam;
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    if (wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Progman") == 0) {
        return TRUE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    bool wanted = false;
    for (DWORD p : *ctx->pids) {
        if (p == pid) {
            wanted = true;
            break;
        }
    }
    if (!wanted) {
        return TRUE;
    }
    // Re-verify the guard at close time (paranoia: pid reuse).
    std::wstring path;
    if (ExePathFromPid(pid, path)) {
        std::wstring exe = ToLowerW(BaseName(path));
        if (exe == L"explorer.exe" || IsExcluded(exe)) {
            return TRUE;
        }
    }
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}

void CloseAllWindowsOfPids(const std::vector<DWORD> &pids) {
    CloseContext ctx{&pids};
    EnumWindows(CloseWindowsProc, (LPARAM)&ctx);
}

void UnsubclassMenu() {
    if (g_subclassActive && g_menuOwner && g_origMenuProc) {
        SetWindowLongPtrW(g_menuOwner, GWLP_WNDPROC,
                          (LONG_PTR)g_origMenuProc);
    }
    g_origMenuProc = nullptr;
    g_menuOwner = nullptr;
    g_subclassActive = false;
    g_targetPids.clear();
}

LRESULT CALLBACK MenuSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam,
                                  LPARAM lParam) {
    if (uMsg == WM_COMMAND && LOWORD(wParam) == kCloseAllCmd) {
        std::vector<DWORD> pids = std::move(g_targetPids);
        UnsubclassMenu();
        CloseAllWindowsOfPids(pids);
        return 0;
    }
    return CallWindowProcW(g_origMenuProc, hWnd, uMsg, wParam, lParam);
}

BOOL WINAPI TrackPopupMenuEx_Hook(HMENU hMenu, UINT uFlags, int x, int y,
                                  HWND hWnd, LPTPMPARAMS lptpm) {
    if (g_enabled && hWnd && g_trayWnd && g_lastTrayClick != 0 &&
        GetTickCount64() - g_lastTrayClick < 2000 &&
        GetAncestor(hWnd, GA_ROOT) == g_trayWnd &&
        MenuHasCloseWindowItem(hMenu)) {
        std::vector<DWORD> pids;
        if (ResolveTargetFromMenu(hMenu, pids) && !pids.empty() &&
            AppendMenuW(hMenu, MF_STRING, kCloseAllCmd,
                        L"Close all windows")) {
            WNDPROC prev = (WNDPROC)SetWindowLongPtrW(
                hWnd, GWLP_WNDPROC, (LONG_PTR)MenuSubclassProc);
            if (prev) {
                g_origMenuProc = prev;
                g_menuOwner = hWnd;
                g_subclassActive = true;
                g_targetPids = std::move(pids);
            }
        }
        g_lastTrayClick = 0;  // Consume the recorded click either way.
    }

    BOOL ret = g_tpmOriginal(hMenu, uFlags, x, y, hWnd, lptpm);

    if (g_subclassActive) {
        // Menu dismissed without our item being picked: restore.
        UnsubclassMenu();
    }
    return ret;
}

LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_enabled && g_trayWnd &&
        (wParam == WM_RBUTTONDOWN || wParam == WM_RBUTTONUP)) {
        const MSLLHOOKSTRUCT *ms = (const MSLLHOOKSTRUCT *)lParam;
        HWND hit = WindowFromPoint(ms->pt);
        if (hit && GetAncestor(hit, GA_ROOT) == g_trayWnd) {
            g_lastTrayClick = GetTickCount64();
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

void LoadSettings() {
    g_enabled = Wh_GetIntSetting(L"enabled");
    g_excluded.clear();
    PCWSTR raw = Wh_GetStringSetting(L"excludedExes");
    std::wstring text = raw ? raw : L"";
    Wh_FreeStringSetting(raw);
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == L',') {
            std::wstring item = ToLowerW(TrimW(text.substr(start, i - start)));
            if (!item.empty()) {
                if (item.find(L'.') == std::wstring::npos) {
                    item += L".exe";
                }
                g_excluded.push_back(item);
            }
            start = i + 1;
        }
    }
}

void SetTpmHook(bool on, bool applyNow) {
    if (on && !g_tpmHooked) {
        if (Wh_SetFunctionHook((void *)TrackPopupMenuEx,
                               (void *)TrackPopupMenuEx_Hook,
                               (void **)&g_tpmOriginal)) {
            g_tpmHooked = TRUE;
        } else {
            Wh_Log(L"TrackPopupMenuEx hook failed");
        }
    } else if (!on && g_tpmHooked) {
        Wh_RemoveFunctionHook((void *)TrackPopupMenuEx);
        g_tpmHooked = FALSE;
    }
    if (applyNow) {
        Wh_ApplyHookOperations();
    }
}

}  // namespace

BOOL Wh_ModInit(void) {
    LoadSettings();

    g_trayWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!g_trayWnd) {
        Wh_Log(L"Shell_TrayWnd not found");
        return FALSE;
    }

    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, nullptr, 0);
    if (!g_mouseHook) {
        Wh_Log(L"SetWindowsHookEx(WH_MOUSE_LL) failed");
        return FALSE;
    }

    SetTpmHook(g_enabled != FALSE, false);
    return TRUE;
}

void Wh_ModUninit(void) {
    UnsubclassMenu();
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
    if (g_tpmHooked) {
        Wh_RemoveFunctionHook((void *)TrackPopupMenuEx);
        g_tpmHooked = FALSE;
    }
    g_lastTrayClick = 0;
}

void Wh_ModSettingsChanged(void) {
    LoadSettings();
    SetTpmHook(g_enabled != FALSE, true);
}
