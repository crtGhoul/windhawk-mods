// ==WindhawkMod==
// @id              tray-order-lock
// @name            Tray Order Lock
// @description     Keep system-tray icon order stable via a user priority list, enforced by re-adding icons in order
// @version         1.0
// @author          crtGhoul
// @github          https://github.com/crtGhoul
// @license         MIT
// @include         explorer.exe
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Tray Order Lock

Keeps your system-tray (notification area) icons in a stable order across
reboots and restarts. You define a priority list of exe basenames (first =
leftmost); the mod tracks every tray icon registered through the hooked
`Shell_NotifyIconW` path and re-registers icons in priority order — first on
load, whenever a priority-listed exe adds an icon, and whenever the settings
change. Icons from exes not on the list keep their current relative order and
are appended after the prioritized ones.

Demand: issues #1170 and #2947 (tray icon order not surviving restarts).

How it works: hooks `Shell_NotifyIconW`, stores a copy of each icon's
`NOTIFYICONDATAW` keyed by (`hWnd`, `uID`) along with the owning exe basename,
then enforces the order by calling the original `NIM_DELETE` followed by
`NIM_ADD` for each tracked icon in the desired sequence (re-adding an icon
moves it to the end, so processing leftmost-first yields the wanted order).
`NIM_MODIFY` and genuine `NIM_DELETE` calls pass through (deletes also drop
the icon from tracking). A reentrancy guard keeps the mod's own re-add calls
from being re-tracked.

## Settings

- **Enabled** — master switch.
- **Priority order** — comma-separated exe basenames, first = leftmost.
  Example: `discord.exe,steam.exe`

## Honest limitation

Re-adding an icon is inherently a delete+add cycle, which is why the order
sticks: position changes are persistent (that is the point of the mod). A side
effect is that balloon/toast tips for a re-added icon may re-fire once, and an
icon that is mid-animation when reordered can briefly flicker. Also note the
hook lives in `explorer.exe`, so it tracks icons whose `Shell_NotifyIconW`
registration passes through that path; a fully system-wide variant would need
`@include *` plus cross-process coordination and is out of scope for this
lean v1.0.

## Testing status

Type-checked only with x86_64-w64-mingw32-g++ -fsyntax-only against the real
windhawk_api.h (Windhawk v1.7.3); NOT compiled with Windhawk's real compiler;
NOT tested on a real Windows machine. Real-Windows verification needed: icon
order after reboot, no icon loss when a tracked app exits while a reorder is
in flight, and no duplicate icons from the delete+add cycle.
*/
// ==WindhawkModSettings==
/*
- enabled: true
  $name: Enabled
- priorityOrder: ""
  $name: Priority order
  $description: "Comma-separated exe basenames, first = leftmost. Example: discord.exe,steam.exe"
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using Shell_NotifyIconW_t = BOOL(WINAPI*)(DWORD, PNOTIFYICONDATAW);
Shell_NotifyIconW_t g_pfnShell_NotifyIconW = nullptr;

struct TrackedIcon {
    HWND hWnd = nullptr;
    UINT uID = 0;
    NOTIFYICONDATAW data{};
    std::wstring exe;
    unsigned long long seq = 0;
};

std::vector<TrackedIcon> g_icons;
unsigned long long g_seqCounter = 0;
std::vector<std::wstring> g_priority;
bool g_enabled = true;
bool g_inApplyOrder = false;  // reentrancy guard for our own re-add calls

std::wstring ExeBasenameFromHwnd(HWND hwnd) {
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

int PriorityIndex(const std::wstring& exe) {
    for (size_t i = 0; i < g_priority.size(); ++i) {
        if (!g_priority[i].empty() &&
            _wcsicmp(g_priority[i].c_str(), exe.c_str()) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

TrackedIcon* FindIcon(HWND hWnd, UINT uID) {
    for (auto& icon : g_icons) {
        if (icon.hWnd == hWnd && icon.uID == uID) {
            return &icon;
        }
    }
    return nullptr;
}

// Re-register tracked icons so the tray ends up in priority order.
// Re-adding moves an icon to the end, so processing leftmost-first produces
// the desired final sequence.
void ApplyOrder() {
    if (!g_enabled || g_inApplyOrder || !g_pfnShell_NotifyIconW) {
        return;
    }
    std::vector<TrackedIcon*> ordered;
    ordered.reserve(g_icons.size());
    for (auto& icon : g_icons) {
        ordered.push_back(&icon);
    }
    // Stable: priority-listed icons first in list order, everything else keeps
    // its registration (seq) order appended at the end.
    std::stable_sort(
        ordered.begin(), ordered.end(),
        [](const TrackedIcon* a, const TrackedIcon* b) {
            int pa = PriorityIndex(a->exe);
            int pb = PriorityIndex(b->exe);
            bool aHas = pa >= 0;
            bool bHas = pb >= 0;
            if (aHas != bHas) {
                return aHas;
            }
            if (aHas && bHas && pa != pb) {
                return pa < pb;
            }
            return a->seq < b->seq;
        });

    g_inApplyOrder = true;
    for (TrackedIcon* icon : ordered) {
        if (!IsWindow(icon->hWnd)) {
            continue;
        }
        g_pfnShell_NotifyIconW(NIM_DELETE, &icon->data);
        g_pfnShell_NotifyIconW(NIM_ADD, &icon->data);
    }
    g_inApplyOrder = false;
}

BOOL WINAPI Shell_NotifyIconW_Hook(DWORD dwMessage, PNOTIFYICONDATAW nid) {
    if (g_inApplyOrder || !g_enabled) {
        return g_pfnShell_NotifyIconW(dwMessage, nid);
    }
    if (nid && dwMessage == NIM_ADD) {
        // Track a private copy before forwarding.
        TrackedIcon* existing = FindIcon(nid->hWnd, nid->uID);
        size_t copySize = nid->cbSize < sizeof(NOTIFYICONDATAW)
                              ? nid->cbSize
                              : sizeof(NOTIFYICONDATAW);
        if (existing) {
            memcpy(&existing->data, nid, copySize);
            existing->data.cbSize = static_cast<DWORD>(copySize);
            existing->exe = ExeBasenameFromHwnd(nid->hWnd);
        } else {
            TrackedIcon icon;
            icon.hWnd = nid->hWnd;
            icon.uID = nid->uID;
            memcpy(&icon.data, nid, copySize);
            icon.data.cbSize = static_cast<DWORD>(copySize);
            icon.exe = ExeBasenameFromHwnd(nid->hWnd);
            icon.seq = g_seqCounter++;
            g_icons.push_back(icon);
        }
        BOOL result = g_pfnShell_NotifyIconW(dwMessage, nid);
        // Enforce order when a priority-listed exe registers an icon.
        TrackedIcon* tracked = FindIcon(nid->hWnd, nid->uID);
        if (tracked && PriorityIndex(tracked->exe) >= 0) {
            ApplyOrder();
        }
        return result;
    }
    if (nid && dwMessage == NIM_DELETE) {
        BOOL result = g_pfnShell_NotifyIconW(dwMessage, nid);
        for (auto it = g_icons.begin(); it != g_icons.end(); ++it) {
            if (it->hWnd == nid->hWnd && it->uID == nid->uID) {
                g_icons.erase(it);
                break;
            }
        }
        return result;
    }
    return g_pfnShell_NotifyIconW(dwMessage, nid);
}

void LoadSettings() {
    g_enabled = Wh_GetIntSetting(L"enabled") != 0;
    g_priority.clear();
    PCWSTR raw = Wh_GetStringSetting(L"priorityOrder");
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
            g_priority.emplace_back(s.substr(a, b - a));
        }
        start = end + 1;
    }
}

}  // namespace

BOOL Wh_ModInit() {
    LoadSettings();
    HMODULE hShell32 = GetModuleHandleW(L"shell32.dll");
    if (!hShell32) {
        Wh_Log(L"GetModuleHandleW(shell32.dll) failed");
        return FALSE;
    }
    void* target = reinterpret_cast<void*>(
        GetProcAddress(hShell32, "Shell_NotifyIconW"));
    if (!target) {
        Wh_Log(L"GetProcAddress(Shell_NotifyIconW) failed");
        return FALSE;
    }
    if (!Wh_SetFunctionHook(target,
                            reinterpret_cast<void*>(&Shell_NotifyIconW_Hook),
                            reinterpret_cast<void**>(&g_pfnShell_NotifyIconW))) {
        Wh_Log(L"Wh_SetFunctionHook(Shell_NotifyIconW) failed");
        return FALSE;
    }
    Wh_Log(L"Tray Order Lock initialized");
    ApplyOrder();
    return TRUE;
}

void Wh_ModUninit() {
    if (g_pfnShell_NotifyIconW) {
        HMODULE hShell32 = GetModuleHandleW(L"shell32.dll");
        if (hShell32) {
            void* target = reinterpret_cast<void*>(
                GetProcAddress(hShell32, "Shell_NotifyIconW"));
            if (target) {
                Wh_RemoveFunctionHook(target);
            }
        }
        g_pfnShell_NotifyIconW = nullptr;
    }
    g_icons.clear();
    Wh_Log(L"Tray Order Lock uninitialized");
}

void Wh_ModSettingsChanged() {
    LoadSettings();
    ApplyOrder();
}
