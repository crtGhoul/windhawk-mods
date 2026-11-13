// ==WindhawkMod==
// @id              taskbar-order-profiles
// @name            Taskbar Order Profiles
// @description     Save/restore pinned taskbar button order as named profiles
// @version         1.0
// @author          crtGhoul
// @github          https://github.com/crtGhoul
// @license         MIT
// @include         explorer.exe
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Taskbar Order Profiles

Save your pinned taskbar button order as a named profile and restore it
later — handy when Explorer scrambles your pins or you want different
orders for different contexts (e.g. "Work" vs "Gaming").

How it works: the pinned order lives in the registry under
`HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Taskband`
(the `Favorites` and `FavoritesResolve` binary values). This mod copies
those blobs into a per-profile key at
`HKCU\Software\crtGhoul\TaskbarOrderProfiles\<name>` on save, and writes
them back plus broadcasts a `Taskband` settings change on restore.

## Usage

Type one command into the **Profile action** setting and apply:

- `save Work` — save the current pinned order as profile "Work"
- `restore Work` — restore the "Work" profile
- `delete Work` — delete the "Work" profile

The command is **one-shot**: it runs once when the settings are applied,
then is remembered so it won't re-run. Clear the field (or type a new
command) to run another action. Profile names may not contain `\`.

## Honest limitations

- The saved data is an **opaque binary blob** whose format is
  build-sensitive — a profile saved on one Windows build may not apply
  cleanly on another. If a restore misbehaves, delete the profile and
  re-save on the current build.
- Only **pinned** order is captured. The order of unpinned *running*
  apps is managed by Explorer and is NOT captured — a Windows
  limitation, not something this mod can work around.
- On some builds Explorer may need a restart after a restore for the
  new order to show up.
- There is no in-mod profile list UI; profiles live in the registry
  path above and can be inspected with regedit.

## Testing status

- Type-checked only with `x86_64-w64-mingw32-g++ -fsyntax-only` against
  the real `windhawk_api.h` (Windhawk v1.7.3).
- NOT compiled with Windhawk's real compiler.
- NOT tested on a real Windows machine — needs real-PC validation:
  save/restore round-trip on 24H2 and 26H2, restore with missing apps,
  and unload behavior.
*/
// ==WindhawkModSettings==
/*
- enabled: true
  $name: Enabled
  $description: Master switch for profile actions.
- profileAction: ""
  $name: Profile action
  $description: "One-shot commands: 'save Work', 'restore Work', 'delete Work'"
*/
// ==/WindhawkModSettings==

#include <windows.h>

#include <string>
#include <vector>

namespace {

const wchar_t kTaskbandKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Taskband";
const wchar_t kProfilesRoot[] = L"Software\\crtGhoul\\TaskbarOrderProfiles";
const wchar_t* kBlobNames[] = {L"Favorites", L"FavoritesResolve"};

bool g_enabled = true;
std::wstring g_lastAction;  // one-shot dedup: a command runs only once

std::wstring Trim(const std::wstring& s) {
    const wchar_t* ws = L" \t\r\n";
    size_t start = s.find_first_not_of(ws);
    if (start == std::wstring::npos) {
        return L"";
    }
    size_t end = s.find_last_not_of(ws);
    return s.substr(start, end - start + 1);
}

bool ValidProfileName(const std::wstring& name) {
    return !name.empty() && name.size() <= 64 &&
           name.find(L'\\') == std::wstring::npos &&
           name.find(L'/') == std::wstring::npos;
}

bool ReadBinaryValue(const std::wstring& subkey, const wchar_t* valueName,
                     std::vector<BYTE>& out) {
    DWORD size = 0;
    LONG rc = RegGetValueW(HKEY_CURRENT_USER, subkey.c_str(), valueName,
                           RRF_RT_REG_BINARY, nullptr, nullptr, &size);
    if (rc != ERROR_SUCCESS || size == 0 || size > 16 * 1024 * 1024) {
        return false;
    }
    out.resize(size);
    DWORD type = 0;
    rc = RegGetValueW(HKEY_CURRENT_USER, subkey.c_str(), valueName,
                      RRF_RT_REG_BINARY, &type, out.data(), &size);
    if (rc != ERROR_SUCCESS) {
        out.clear();
        return false;
    }
    out.resize(size);
    return true;
}

bool WriteBinaryValue(const std::wstring& subkey, const wchar_t* valueName,
                      const std::vector<BYTE>& data) {
    HKEY hKey = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr,
                              REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                              nullptr, &hKey, nullptr);
    if (rc != ERROR_SUCCESS) {
        return false;
    }
    rc = RegSetValueExW(hKey, valueName, 0, REG_BINARY, data.data(),
                        static_cast<DWORD>(data.size()));
    RegCloseKey(hKey);
    return rc == ERROR_SUCCESS;
}

bool SaveProfile(const std::wstring& name) {
    std::wstring profileKey = std::wstring(kProfilesRoot) + L"\\" + name;
    bool anySaved = false;
    for (const wchar_t* blobName : kBlobNames) {
        std::vector<BYTE> data;
        if (!ReadBinaryValue(kTaskbandKey, blobName, data)) {
            Wh_Log(L"Save: could not read Taskband value %s", blobName);
            continue;
        }
        if (!WriteBinaryValue(profileKey, blobName, data)) {
            Wh_Log(L"Save: could not write profile value %s", blobName);
            return false;
        }
        anySaved = true;
    }
    if (anySaved) {
        Wh_Log(L"Saved profile '%s'", name.c_str());
    } else {
        Wh_Log(L"Save failed: no Taskband data found");
    }
    return anySaved;
}

bool RestoreProfile(const std::wstring& name) {
    std::wstring profileKey = std::wstring(kProfilesRoot) + L"\\" + name;
    bool anyRestored = false;
    for (const wchar_t* blobName : kBlobNames) {
        std::vector<BYTE> data;
        if (!ReadBinaryValue(profileKey, blobName, data)) {
            Wh_Log(L"Restore: profile '%s' has no value %s", name.c_str(),
                   blobName);
            continue;
        }
        if (!WriteBinaryValue(kTaskbandKey, blobName, data)) {
            Wh_Log(L"Restore: could not write Taskband value %s", blobName);
            return false;
        }
        anyRestored = true;
    }
    if (!anyRestored) {
        Wh_Log(L"Restore failed: profile '%s' not found", name.c_str());
        return false;
    }
    SendNotifyMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                       reinterpret_cast<LPARAM>(L"Taskband"));
    Wh_Log(L"Restored profile '%s'", name.c_str());
    return true;
}

bool DeleteProfile(const std::wstring& name) {
    std::wstring profileKey = std::wstring(kProfilesRoot) + L"\\" + name;
    LONG rc = RegDeleteTreeW(HKEY_CURRENT_USER, profileKey.c_str());
    if (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND) {
        Wh_Log(L"Deleted profile '%s'", name.c_str());
        return true;
    }
    Wh_Log(L"Delete failed for profile '%s', error %d", name.c_str(),
           static_cast<int>(rc));
    return false;
}

bool StartsWith(const std::wstring& s, const wchar_t* prefix,
                std::wstring& restOut) {
    size_t len = wcslen(prefix);
    if (s.size() <= len || s.compare(0, len, prefix) != 0) {
        return false;
    }
    restOut = Trim(s.substr(len));
    return true;
}

void RunProfileAction(const std::wstring& action) {
    std::wstring rest;
    std::wstring name;
    if (StartsWith(action, L"save ", rest)) {
        name = rest;
        if (!ValidProfileName(name)) {
            Wh_Log(L"Invalid profile name in '%s'", action.c_str());
            return;
        }
        SaveProfile(name);
    } else if (StartsWith(action, L"restore ", rest)) {
        name = rest;
        if (!ValidProfileName(name)) {
            Wh_Log(L"Invalid profile name in '%s'", action.c_str());
            return;
        }
        RestoreProfile(name);
    } else if (StartsWith(action, L"delete ", rest)) {
        name = rest;
        if (!ValidProfileName(name)) {
            Wh_Log(L"Invalid profile name in '%s'", action.c_str());
            return;
        }
        DeleteProfile(name);
    } else {
        Wh_Log(L"Unknown profile action '%s' (use 'save <name>', "
               L"'restore <name>', 'delete <name>')",
               action.c_str());
    }
}

void LoadSettings() {
    g_enabled = Wh_GetIntSetting(L"enabled") != 0;
}

void HandleSettingsChanged() {
    LoadSettings();
    if (!g_enabled) {
        return;
    }
    PCWSTR raw = Wh_GetStringSetting(L"profileAction");
    std::wstring action = Trim(raw ? raw : L"");
    Wh_FreeStringSetting(raw);

    if (action.empty() || action == g_lastAction) {
        return;  // one-shot: already ran (or nothing to do)
    }
    g_lastAction = action;
    RunProfileAction(action);
}

}  // namespace

BOOL Wh_ModInit(void) {
    Wh_Log(L"Init");
    LoadSettings();
    return TRUE;
}

void Wh_ModUninit(void) {
    Wh_Log(L"Uninit");
    // Nothing to undo: registry writes are the user's explicit action.
}

void Wh_ModSettingsChanged(void) {
    Wh_Log(L"Settings changed");
    HandleSettingsChanged();
}
