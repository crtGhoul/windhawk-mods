// ==WindhawkMod==
// @id              copilot-killer
// @name            Copilot Killer
// @description     Blocklist-driven blocking of Copilot processes and Copilot-hosted WebView2 children via CreateProcessW hooks. No registry changes, clean unload.
// @version         1.0
// @author          crtGhoul
// @github          https://github.com/crtGhoul
// @license         MIT
// @include         *
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Copilot Killer

Blocks Copilot from launching, process-wide, via a CreateProcessW hook
installed in every process:

- **Exe blocklist.** The image file-name portion of the launched
  application (falling back to the first token of the command line) is
  compared case-insensitively against a semicolon-separated blocklist of
  exe basenames. Default: `copilot.exe;M365Copilot.exe;CopilotApp.exe`.
- **Copilot-hosted WebView2 children.** If a command line launches
  `msedgewebview2` and also contains one of the configured web markers
  (default: `copilot.microsoft.com;copilot.cloud.microsoft`), the launch
  is refused.

Blocked launches fail with ERROR_ACCESS_DENIED. No registry changes are
made; unloading the mod removes the hook with no persistent state.

## Settings

- **Enabled** — master switch.
- **Blocklist** — semicolon-separated exe basenames.
- **Webview markers** — blocked only inside msedgewebview2 command lines.

## Honest limitations

- Blocks launch only: it does not terminate Copilot processes that are
  already running when the mod loads.
- Matching is by exe basename, so a renamed Copilot binary slips through
  the blocklist (the WebView2-marker path still catches its web UI).

## Testing status

Type-checked only with x86_64-w64-mingw32-g++ -fsyntax-only against the
real windhawk_api.h (Windhawk v1.7.3); NOT compiled with Windhawk's real
compiler; NOT tested on a real Windows machine.
*/
// ==WindhawkModSettings==
/*
- enabled: true
  $name: Enabled
  $description: Master switch. When off, nothing is blocked.
- blocklist: "copilot.exe;M365Copilot.exe;CopilotApp.exe"
  $name: Blocklist
  $description: Semicolon-separated exe basenames.
- webviewMarkers: "copilot.microsoft.com;copilot.cloud.microsoft"
  $name: Webview markers
  $description: Blocked only inside msedgewebview2 command lines.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <cwctype>
#include <string>
#include <vector>

namespace {

BOOL g_enabled = TRUE;
std::vector<std::wstring> g_blocklist; // lowercased exe basenames
std::vector<std::wstring> g_markers;   // lowercased web markers

using CreateProcessW_t = decltype(&CreateProcessW);
CreateProcessW_t g_createProcessW_Original = nullptr;

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

void SplitSemis(PCWSTR raw, std::vector<std::wstring> &out) {
    out.clear();
    std::wstring all(raw ? raw : L"");
    size_t start = 0;
    while (start <= all.size()) {
        size_t semi = all.find(L';', start);
        std::wstring part = Trim(all.substr(
            start, semi == std::wstring::npos ? std::wstring::npos : semi - start));
        if (!part.empty()) {
            out.push_back(ToLower(part));
        }
        if (semi == std::wstring::npos) {
            break;
        }
        start = semi + 1;
    }
}

void LoadSettings() {
    g_enabled = Wh_GetIntSetting(L"enabled");

    PCWSTR raw = Wh_GetStringSetting(L"blocklist");
    SplitSemis(raw, g_blocklist);
    Wh_FreeStringSetting(raw);

    raw = Wh_GetStringSetting(L"webviewMarkers");
    SplitSemis(raw, g_markers);
    Wh_FreeStringSetting(raw);
}

// Lowercased exe basename of the image being launched: the file-name
// portion of lpApplicationName, falling back to the first command-line
// token (honoring quotes).
std::wstring ImageBasename(PCWSTR appName, PCWSTR cmdLine) {
    std::wstring src;
    if (appName && *appName) {
        src = appName;
    } else if (cmdLine && *cmdLine) {
        PCWSTR p = cmdLine;
        while (*p && std::iswspace(*p)) {
            ++p;
        }
        if (*p == L'"') {
            ++p;
            PCWSTR end = wcschr(p, L'"');
            src.assign(p, end ? end : p + wcslen(p));
        } else {
            PCWSTR end = p;
            while (*end && !std::iswspace(*end)) {
                ++end;
            }
            src.assign(p, end);
        }
    }
    size_t slash = src.find_last_of(L"\\/");
    return ToLower(src.substr(slash == std::wstring::npos ? 0 : slash + 1));
}

BOOL WINAPI CreateProcessW_Hook(LPCWSTR appName, LPWSTR cmdLine,
                                LPSECURITY_ATTRIBUTES procAttr,
                                LPSECURITY_ATTRIBUTES threadAttr,
                                BOOL inheritHandles, DWORD creationFlags,
                                LPVOID environment, LPCWSTR currentDir,
                                LPSTARTUPINFOW startupInfo,
                                LPPROCESS_INFORMATION procInfo) {
    if (g_enabled) {
        std::wstring base = ImageBasename(appName, cmdLine);
        for (const auto &b : g_blocklist) {
            if (base == b) {
                Wh_Log(L"Blocked Copilot launch: %s", base.c_str());
                SetLastError(ERROR_ACCESS_DENIED);
                return FALSE;
            }
        }

        if (cmdLine && *cmdLine && !g_markers.empty()) {
            std::wstring cl = ToLower(cmdLine);
            if (wcsstr(cl.c_str(), L"msedgewebview2")) {
                for (const auto &m : g_markers) {
                    if (wcsstr(cl.c_str(), m.c_str())) {
                        Wh_Log(L"Blocked Copilot WebView2 child");
                        SetLastError(ERROR_ACCESS_DENIED);
                        return FALSE;
                    }
                }
            }
        }
    }

    return g_createProcessW_Original(appName, cmdLine, procAttr, threadAttr,
                                     inheritHandles, creationFlags, environment,
                                     currentDir, startupInfo, procInfo);
}

} // namespace

BOOL Wh_ModInit(void) {
    LoadSettings();

    if (!Wh_SetFunctionHook((void *)CreateProcessW,
                            (void *)CreateProcessW_Hook,
                            (void **)&g_createProcessW_Original)) {
        Wh_Log(L"CreateProcessW hook failed");
    }

    return TRUE;
}

void Wh_ModUninit(void) {
    Wh_RemoveFunctionHook((void *)CreateProcessW);
}

void Wh_ModSettingsChanged(void) {
    LoadSettings();
}
