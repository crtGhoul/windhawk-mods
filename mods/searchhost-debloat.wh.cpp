// ==WindhawkMod==
// @id              searchhost-debloat
// @name            SearchHost Debloat
// @description     Blocks msedgewebview2.exe children spawned by SearchHost.exe; optional offline local-only search toggle (registry reverted on unload). Surgical: never kills SearchHost.
// @version         1.0
// @author          crtGhoul
// @github          https://github.com/crtGhoul
// @license         MIT
// @include         SearchHost.exe
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# SearchHost Debloat

Trims the Windows Search UI (SearchHost.exe) without breaking it:

- **Block WebView2 children.** Hooks CreateProcessW inside SearchHost and
  refuses to launch `msedgewebview2.exe` child processes (the ones that
  render web content / ads in the search flyout). Everything else passes
  through untouched. SearchHost itself is never killed or suspended.
- **Offline local-only search (optional, off by default).** Sets
  `HKCU\Software\Policies\Microsoft\Windows\Explorer\DisableSearchBoxSuggestions`
  to 1 while enabled and restores the previous value (or deletes the value
  if it did not exist) on unload, then broadcasts WM_SETTINGCHANGE.

## Settings

- **Enabled** — master switch.
- **Block WebView2** — refuse msedgewebview2.exe child launches.
- **Offline search** — local-only search; reverted on unload. Off by
  default (conservative).

## Honest limitations

- Blocking happens at CreateProcess time only: an msedgewebview2 child
  that is already running when the mod loads is left alone.
- The offline-search registry value is a policy hint; Windows may ignore
  it on builds where the search UI no longer honors it.

## Testing status

Type-checked only with x86_64-w64-mingw32-g++ -fsyntax-only against the
real windhawk_api.h (Windhawk v1.7.3); NOT compiled with Windhawk's real
compiler; NOT tested on a real Windows machine.
*/
// ==WindhawkModSettings==
/*
- enabled: true
  $name: Enabled
  $description: Master switch. When off, nothing is blocked or changed.
- blockWebView2: true
  $name: Block WebView2
  $description: Refuse msedgewebview2.exe child launches from SearchHost.
- offlineSearch: false
  $name: Offline search
  $description: Local-only search; reverted on unload.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <cwctype>
#include <string>

namespace {

BOOL g_enabled = TRUE;
BOOL g_blockWebView2 = TRUE;
BOOL g_offlineSearch = FALSE;

BOOL g_offlineApplied = FALSE;
BOOL g_hadValue = FALSE;
DWORD g_oldValue = 0;

constexpr const wchar_t *kRegPath = L"Software\\Policies\\Microsoft\\Windows\\Explorer";
constexpr const wchar_t *kRegValue = L"DisableSearchBoxSuggestions";

using CreateProcessW_t = decltype(&CreateProcessW);
CreateProcessW_t g_createProcessW_Original = nullptr;

std::wstring ToLower(std::wstring s) {
    for (auto &ch : s) {
        ch = std::towlower(ch);
    }
    return s;
}

bool ContainsCI(PCWSTR haystack, PCWSTR needle) {
    if (!haystack || !needle || !*needle) {
        return false;
    }
    std::wstring h = ToLower(haystack);
    std::wstring n = ToLower(needle);
    return wcsstr(h.c_str(), n.c_str()) != nullptr;
}

BOOL WINAPI CreateProcessW_Hook(LPCWSTR appName, LPWSTR cmdLine,
                                LPSECURITY_ATTRIBUTES procAttr,
                                LPSECURITY_ATTRIBUTES threadAttr,
                                BOOL inheritHandles, DWORD creationFlags,
                                LPVOID environment, LPCWSTR currentDir,
                                LPSTARTUPINFOW startupInfo,
                                LPPROCESS_INFORMATION procInfo) {
    if (g_enabled && g_blockWebView2) {
        if (ContainsCI(appName, L"msedgewebview2") ||
            ContainsCI(cmdLine, L"msedgewebview2")) {
            Wh_Log(L"Blocked msedgewebview2 child of SearchHost");
            SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
    }
    return g_createProcessW_Original(appName, cmdLine, procAttr, threadAttr,
                                     inheritHandles, creationFlags, environment,
                                     currentDir, startupInfo, procInfo);
}

void BroadcastPolicyChange() {
    DWORD_PTR result = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        (LPARAM)L"Policy", SMTO_ABORTIFHUNG, 5000, &result);
}

void ApplyOfflineSearch() {
    if (g_offlineApplied) {
        return;
    }

    HKEY hKey = nullptr;
    DWORD disposition = 0;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
                              KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &hKey,
                              &disposition);
    if (rc != ERROR_SUCCESS) {
        Wh_Log(L"RegCreateKeyEx failed: %ld", rc);
        return;
    }

    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    if (RegQueryValueExW(hKey, kRegValue, nullptr, &type, (LPBYTE)&value,
                         &size) == ERROR_SUCCESS &&
        type == REG_DWORD) {
        g_hadValue = TRUE;
        g_oldValue = value;
    } else {
        g_hadValue = FALSE;
    }

    DWORD one = 1;
    rc = RegSetValueExW(hKey, kRegValue, 0, REG_DWORD, (const BYTE *)&one,
                        sizeof(one));
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS) {
        Wh_Log(L"RegSetValueEx failed: %ld", rc);
        return;
    }

    g_offlineApplied = TRUE;
    BroadcastPolicyChange();
    Wh_Log(L"Offline search enabled (DisableSearchBoxSuggestions=1)");
}

void RevertOfflineSearch() {
    if (!g_offlineApplied) {
        return;
    }

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_SET_VALUE, &hKey) ==
        ERROR_SUCCESS) {
        if (g_hadValue) {
            RegSetValueExW(hKey, kRegValue, 0, REG_DWORD,
                           (const BYTE *)&g_oldValue, sizeof(g_oldValue));
        } else {
            RegDeleteValueW(hKey, kRegValue);
        }
        RegCloseKey(hKey);
    }

    g_offlineApplied = FALSE;
    BroadcastPolicyChange();
    Wh_Log(L"Offline search setting reverted");
}

void LoadSettings() {
    g_enabled = Wh_GetIntSetting(L"enabled");
    g_blockWebView2 = Wh_GetIntSetting(L"blockWebView2");
    g_offlineSearch = Wh_GetIntSetting(L"offlineSearch");
}

} // namespace

BOOL Wh_ModInit(void) {
    LoadSettings();

    if (!Wh_SetFunctionHook((void *)CreateProcessW,
                            (void *)CreateProcessW_Hook,
                            (void **)&g_createProcessW_Original)) {
        Wh_Log(L"CreateProcessW hook failed");
    }

    if (g_offlineSearch) {
        ApplyOfflineSearch();
    }

    return TRUE;
}

void Wh_ModUninit(void) {
    RevertOfflineSearch();
    Wh_RemoveFunctionHook((void *)CreateProcessW);
}

void Wh_ModSettingsChanged(void) {
    LoadSettings();
    if (g_offlineSearch) {
        ApplyOfflineSearch();
    } else {
        RevertOfflineSearch();
    }
}
