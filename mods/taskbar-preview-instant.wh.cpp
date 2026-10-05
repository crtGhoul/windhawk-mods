// ==WindhawkMod==
// @id              taskbar-preview-instant
// @name            Taskbar Preview Instant
// @description     Zero hover delay before taskbar window previews appear (ExtendedUIHoverTime=0 while enabled, restored on unload)
// @version         1.0
// @author          crtGhoul
// @github          https://github.com/crtGhoul
// @license         MIT
// @include         explorer.exe
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Taskbar Preview Instant

Removes the hover delay before taskbar window previews pop up: while
the mod is enabled it sets
`HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced\ExtendedUIHoverTime`
to 0 and broadcasts `WM_SETTINGCHANGE` so Explorer picks it up.

No hooks, no polling, no timers — one registry value, written once on
load and reverted once on unload. If the value did not exist before,
it is deleted on unload rather than left at 0.

## Settings

- **Enabled** — master switch. Off restores the previous value (or
  deletes the value if it was absent).

## Honest limitations

- The exact effect varies by Windows build: on some builds the
  preview appears truly instantly, on others a small delay remains or
  an Explorer restart is needed. Needs a real-PC check.
- This only covers the hover-to-show delay; the linger delay after
  unhovering is a separate behavior and is not changed.
- If another tool also writes `ExtendedUIHoverTime`, the last writer
  wins and unload restores whatever was there when this mod loaded.

## Testing status

Type-checked only with x86_64-w64-mingw32-g++ -fsyntax-only against the
real windhawk_api.h (Windhawk v1.7.3); NOT compiled with Windhawk's real
compiler; NOT tested on a real Windows machine.
*/
// ==WindhawkModSettings==
/*
- enabled: true
  $name: Enabled
  $description: Master switch. When off, the previous hover-delay value is restored.
*/
// ==/WindhawkModSettings==

#include <windows.h>

namespace {

BOOL g_enabled = TRUE;
BOOL g_applied = FALSE;
BOOL g_hadOriginal = FALSE;
DWORD g_original = 0;

constexpr const wchar_t *kRegPath =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
constexpr const wchar_t *kRegValue = L"ExtendedUIHoverTime";

void BroadcastSettingChange() {
    DWORD_PTR result = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        (LPARAM)L"Policy", SMTO_ABORTIFHUNG, 5000, &result);
}

void ApplyInstantPreview() {
    if (g_applied) {
        return;
    }

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE, &hKey) !=
        ERROR_SUCCESS) {
        Wh_Log(L"RegOpenKeyEx failed");
        return;
    }

    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    if (RegQueryValueExW(hKey, kRegValue, nullptr, &type, (LPBYTE)&value,
                         &size) == ERROR_SUCCESS &&
        type == REG_DWORD) {
        g_hadOriginal = TRUE;
        g_original = value;
    } else {
        g_hadOriginal = FALSE;
    }

    DWORD zero = 0;
    LONG rc = RegSetValueExW(hKey, kRegValue, 0, REG_DWORD,
                             (const BYTE *)&zero, sizeof(zero));
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS) {
        Wh_Log(L"RegSetValueEx failed: %ld", rc);
        return;
    }

    g_applied = TRUE;
    BroadcastSettingChange();
    Wh_Log(L"ExtendedUIHoverTime set to 0");
}

void RevertInstantPreview() {
    if (!g_applied) {
        return;
    }

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_SET_VALUE, &hKey) ==
        ERROR_SUCCESS) {
        if (g_hadOriginal) {
            RegSetValueExW(hKey, kRegValue, 0, REG_DWORD,
                           (const BYTE *)&g_original, sizeof(g_original));
        } else {
            RegDeleteValueW(hKey, kRegValue);
        }
        RegCloseKey(hKey);
    }

    g_applied = FALSE;
    BroadcastSettingChange();
    Wh_Log(L"ExtendedUIHoverTime reverted");
}

}  // namespace

BOOL Wh_ModInit(void) {
    g_enabled = Wh_GetIntSetting(L"enabled");
    if (g_enabled) {
        ApplyInstantPreview();
    }
    return TRUE;
}

void Wh_ModUninit(void) {
    RevertInstantPreview();
}

void Wh_ModSettingsChanged(void) {
    g_enabled = Wh_GetIntSetting(L"enabled");
    if (g_enabled) {
        ApplyInstantPreview();
    } else {
        RevertInstantPreview();
    }
}
