// ==WindhawkMod==
// @id              freeze-on-minimize
// @name            Freeze on Minimize
// @description     Suspends a process with NtSuspendProcess when its window is minimized; resumes on restore. Opt-in allowlist, hardcoded denylist, resume-all on unload.
// @version         1.0
// @author          crtGhoul
// @github          https://github.com/crtGhoul
// @license         MIT
// @include         explorer.exe
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Freeze on Minimize

Suspends a process (via NtSuspendProcess) when one of its windows is
minimized, and resumes it (via NtResumeProcess) when the window is
restored. Useful for pausing CPU-hungry apps you are not looking at.

The mod runs inside explorer.exe and listens for system-wide minimize /
restore events — no polling.

Safety design:

- **Opt-in allowlist.** The allowlist is empty by default, so the mod does
  nothing until you add exe basenames (e.g. `notepad.exe,spotify.exe`).
- **Hardcoded denylist.** System processes (explorer, dwm, csrss, smss,
  wininit, services, lsass, svchost, SearchHost, StartMenuExperienceHost,
  ShellExperienceHost, ApplicationFrameHost, System) are never suspended,
  even if allowlisted.
- **Resume-all on unload.** Disabling or unloading the mod resumes every
  process it suspended.

## Settings

- **Enabled** — master switch.
- **Allowlist** — comma-separated exe basenames to freeze on minimize.
  Empty = do nothing. Example: `notepad.exe,spotify.exe`.

## Honest limitations

- Suspension is per-process: restoring any one window of a multi-window
  app resumes the whole process, even if its other windows are still
  minimized.
- A suspended app's threads are fully frozen — anything it was doing
  (downloads, timers) pauses until you restore the window.

## Testing status

Type-checked only with x86_64-w64-mingw32-g++ -fsyntax-only against the
real windhawk_api.h (Windhawk v1.7.3); NOT compiled with Windhawk's real
compiler; NOT tested on a real Windows machine.
*/
// ==WindhawkModSettings==
/*
- enabled: true
  $name: Enabled
  $description: Master switch. When off, nothing is suspended.
- allowlist: ""
  $name: Allowlist
  $description: "Comma-separated exe basenames to freeze on minimize. Empty = do nothing. Example: notepad.exe,spotify.exe"
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <cwctype>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

BOOL g_enabled = TRUE;
std::vector<std::wstring> g_allowlist; // lowercased exe basenames
std::unordered_set<DWORD> g_frozen;    // PIDs currently suspended by us
HWINEVENTHOOK g_hook = nullptr;

using NtSuspendProcess_t = LONG(NTAPI *)(HANDLE);
using NtResumeProcess_t = LONG(NTAPI *)(HANDLE);
NtSuspendProcess_t g_ntSuspend = nullptr;
NtResumeProcess_t g_ntResume = nullptr;

// Hardcoded denylist (lowercase). Never suspended, even if allowlisted.
constexpr const wchar_t *kDenylist[] = {
    L"explorer.exe",
    L"dwm.exe",
    L"csrss.exe",
    L"smss.exe",
    L"wininit.exe",
    L"services.exe",
    L"lsass.exe",
    L"svchost.exe",
    L"searchhost.exe",
    L"startmenuexperiencehost.exe",
    L"shellexperiencehost.exe",
    L"applicationframehost.exe",
    L"system",
};

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

    g_allowlist.clear();
    PCWSTR raw = Wh_GetStringSetting(L"allowlist");
    std::wstring all(raw ? raw : L"");
    Wh_FreeStringSetting(raw);

    size_t start = 0;
    while (start <= all.size()) {
        size_t comma = all.find(L',', start);
        std::wstring part = Trim(all.substr(
            start, comma == std::wstring::npos ? std::wstring::npos : comma - start));
        if (!part.empty()) {
            g_allowlist.push_back(ToLower(part));
        }
        if (comma == std::wstring::npos) {
            break;
        }
        start = comma + 1;
    }
}

std::wstring ExeBasename(DWORD pid) {
    std::wstring result;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        return result;
    }
    wchar_t path[MAX_PATH];
    DWORD size = ARRAYSIZE(path);
    if (QueryFullProcessImageNameW(h, 0, path, &size) && size > 0) {
        std::wstring full(path, size);
        size_t slash = full.find_last_of(L"\\/");
        result = ToLower(full.substr(slash == std::wstring::npos ? 0 : slash + 1));
    }
    CloseHandle(h);
    return result;
}

bool IsDenied(const std::wstring &base) {
    for (const wchar_t *d : kDenylist) {
        if (base == d) {
            return true;
        }
    }
    return false;
}

bool IsAllowed(const std::wstring &base) {
    for (const auto &a : g_allowlist) {
        if (base == a) {
            return true;
        }
    }
    return false;
}

bool SuspendPid(DWORD pid) {
    if (!g_ntSuspend) {
        return false;
    }
    HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid);
    if (!h) {
        return false;
    }
    LONG status = g_ntSuspend(h);
    CloseHandle(h);
    return status == 0; // STATUS_SUCCESS
}

bool ResumePid(DWORD pid) {
    if (!g_ntResume) {
        return false;
    }
    HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid);
    if (!h) {
        return false;
    }
    LONG status = g_ntResume(h);
    CloseHandle(h);
    return status == 0; // STATUS_SUCCESS
}

void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                           LONG idObject, LONG idChild, DWORD thread,
                           DWORD time) {
    (void)hook;
    (void)idObject;
    (void)idChild;
    (void)thread;
    (void)time;

    if (!g_enabled || !hwnd) {
        return;
    }
    // Only top-level windows can be minimized.
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) {
        return;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == 4 /* System */ || pid == GetCurrentProcessId()) {
        return;
    }

    // Skip shell windows.
    wchar_t cls[64];
    if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls))) {
        if (_wcsicmp(cls, L"Progman") == 0 ||
            _wcsicmp(cls, L"Shell_TrayWnd") == 0) {
            return;
        }
    }

    if (event == EVENT_SYSTEM_MINIMIZESTART) {
        if (g_frozen.find(pid) != g_frozen.end()) {
            return;
        }
        std::wstring base = ExeBasename(pid);
        if (base.empty() || IsDenied(base) || !IsAllowed(base)) {
            return;
        }
        if (SuspendPid(pid)) {
            g_frozen.insert(pid);
            Wh_Log(L"Suspended %s (pid %u)", base.c_str(), pid);
        } else {
            Wh_Log(L"Failed to suspend %s (pid %u)", base.c_str(), pid);
        }
    } else if (event == EVENT_SYSTEM_MINIMIZEEND) {
        auto it = g_frozen.find(pid);
        if (it == g_frozen.end()) {
            return;
        }
        if (ResumePid(pid)) {
            Wh_Log(L"Resumed pid %u", pid);
        } else {
            Wh_Log(L"Failed to resume pid %u", pid);
        }
        g_frozen.erase(it);
    }
}

} // namespace

BOOL Wh_ModInit(void) {
    LoadSettings();

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        g_ntSuspend =
            (NtSuspendProcess_t)(void *)GetProcAddress(ntdll, "NtSuspendProcess");
        g_ntResume =
            (NtResumeProcess_t)(void *)GetProcAddress(ntdll, "NtResumeProcess");
    }
    if (!g_ntSuspend || !g_ntResume) {
        Wh_Log(L"Could not resolve NtSuspendProcess/NtResumeProcess");
    }

    g_hook = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART,
                             EVENT_SYSTEM_MINIMIZEEND, nullptr, WinEventProc, 0,
                             0, WINEVENT_OUTOFCONTEXT);
    if (!g_hook) {
        Wh_Log(L"SetWinEventHook failed");
    }

    return TRUE;
}

void Wh_ModUninit(void) {
    if (g_hook) {
        UnhookWinEvent(g_hook);
        g_hook = nullptr;
    }
    // Resume everything we suspended — never leave a process frozen.
    for (DWORD pid : g_frozen) {
        if (ResumePid(pid)) {
            Wh_Log(L"Resumed pid %u on unload", pid);
        } else {
            Wh_Log(L"Failed to resume pid %u on unload", pid);
        }
    }
    g_frozen.clear();
}

void Wh_ModSettingsChanged(void) {
    LoadSettings();
}
