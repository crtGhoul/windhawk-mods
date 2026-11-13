// ==WindhawkMod==
// @id              explorer-selection-box-styler
// @name            Explorer Selection Box Styler
// @description     Tint Explorer's rubber-band/selection via SetSysColors (system-wide while enabled, restored on unload)
// @version         1.0
// @author          crtGhoul
// @github          https://github.com/crtGhoul
// @license         MIT
// @include         explorer.exe
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Explorer Selection Box Styler

Recolors the selection highlight (the color behind selected items and
the rubber-band marquee you drag in File Explorer) via the documented
`SetSysColors` API. No hooks, no code patching — just a system color
change that is fully reverted when the mod is unloaded or disabled.

## Settings

- **Enabled** — master switch. Defaults to **off** because the effect
  is system-wide (see below).
- **Selection color** — a COLORREF integer (`0x00BBGGRR`). Default
  `7907476` = `0x0078D4` (Windows blue).

## Important honest limitation

Windows offers **no marquee-only API**: `SetSysColors` changes
`COLOR_HIGHLIGHT`/`COLOR_HOTLIGHT` for the whole session, so while this
mod is enabled **every** selection highlight everywhere (list views,
text selection, other apps) is tinted — not just Explorer's
rubber-band rectangle. If you only want the Explorer marquee recolored,
this mod cannot do that; it is an honest system-wide tint.

Additional caveats:

- Windows 11 File Explorer's newer DirectUI/XAML views may ignore the
  classic system colors, so the effect may only show in classic views.
- Other software calling `SetSysColors` while this mod is enabled can
  fight over the colors; unloading restores whatever this mod saved at
  load time.

## Testing status

- Type-checked only with `x86_64-w64-mingw32-g++ -fsyntax-only` against
  the real `windhawk_api.h` (Windhawk v1.7.3).
- NOT compiled with Windhawk's real compiler.
- NOT tested on a real Windows machine — needs real-PC validation that
  the tint applies, restores cleanly on unload, and behaves across
  light/dark mode switches.
*/
// ==WindhawkModSettings==
/*
- enabled: false
  $name: Enabled
  $description: Master switch. Off by default because the tint is system-wide while enabled.
- selectionColor: 7907476
  $name: Selection color
  $description: "COLORREF integer; default 0x0078D4"
*/
// ==/WindhawkModSettings==

#include <windows.h>

namespace {

bool g_enabled = false;
COLORREF g_selectionColor = 0x0078D4;
COLORREF g_origHighlight = 0;
COLORREF g_origHotlight = 0;
bool g_applied = false;

void ApplyColors() {
    if (g_applied) {
        return;
    }
    g_origHighlight = GetSysColor(COLOR_HIGHLIGHT);
    g_origHotlight = GetSysColor(COLOR_HOTLIGHT);

    int elements[2] = {COLOR_HIGHLIGHT, COLOR_HOTLIGHT};
    COLORREF colors[2] = {g_selectionColor, g_selectionColor};
    if (SetSysColors(ARRAYSIZE(elements), elements, colors)) {
        g_applied = true;
        Wh_Log(L"Applied selection color 0x%06X",
               static_cast<unsigned int>(g_selectionColor & 0xFFFFFF));
    } else {
        Wh_Log(L"SetSysColors failed, error %lu", GetLastError());
    }
}

void RestoreColors() {
    if (!g_applied) {
        return;
    }
    int elements[2] = {COLOR_HIGHLIGHT, COLOR_HOTLIGHT};
    COLORREF colors[2] = {g_origHighlight, g_origHotlight};
    SetSysColors(ARRAYSIZE(elements), elements, colors);
    g_applied = false;
    Wh_Log(L"Restored original selection colors");
}

void LoadSettings() {
    g_enabled = Wh_GetIntSetting(L"enabled") != 0;
    int color = Wh_GetIntSetting(L"selectionColor");
    if (color < 0) {
        color = 0;
    }
    if (color > 0xFFFFFF) {
        color = 0xFFFFFF;
    }
    g_selectionColor = static_cast<COLORREF>(color);
}

void RefreshState() {
    if (g_enabled) {
        // Re-apply so a color change takes effect immediately.
        RestoreColors();
        ApplyColors();
    } else {
        RestoreColors();
    }
}

}  // namespace

BOOL Wh_ModInit(void) {
    Wh_Log(L"Init");
    LoadSettings();
    RefreshState();
    return TRUE;
}

void Wh_ModUninit(void) {
    Wh_Log(L"Uninit");
    RestoreColors();
}

void Wh_ModSettingsChanged(void) {
    Wh_Log(L"Settings changed");
    LoadSettings();
    RefreshState();
}
