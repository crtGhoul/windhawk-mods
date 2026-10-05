// ==WindhawkMod==
// @id              low-spec-mode
// @name            Low Spec Mode
// @description     One toggle applying reversible low-end-PC tweaks via documented SystemParametersInfo APIs; originals restored on unload.
// @version         1.0
// @author          crtGhoul
// @github          https://github.com/crtGhoul
// @license         MIT
// @include         explorer.exe
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Low Spec Mode

One toggle that applies a set of reversible low-end-PC tweaks, using only
documented SystemParametersInfo APIs — no registry hacks. Every original
value is saved before being changed and restored when the mod is unloaded
or the master toggle is switched off.

Tweaks (each individually toggleable, all gated by the master switch):

- Minimize/maximize animation off (SPI_SETANIMATION)
- Menu animation off (SPI_SETMENUANIMATION)
- Tooltip animation off (SPI_SETTOOLTIPANIMATION)
- Combo box animation off (SPI_SETCOMBOBOXANIMATION)
- Menu fade effect off (SPI_SETMENUFADE)
- Selection fade effect off (SPI_SETSELECTIONFADE)
- Tooltip fade effect off (SPI_SETTOOLTIPFADE)
- List box smooth scrolling off (SPI_SETLISTBOXSMOOTHSCROLLING)

## Settings

- **Enabled** — master toggle. OFF by default; nothing changes until you
  turn it on.
- The eight `disable*` settings control individual tweaks.

## Honest limitations

- Some applications read animation settings once at startup and will not
  pick up the change until they are restarted.
- Changes are broadcast with SPIF_SENDCHANGE, so well-behaved apps apply
  them live; Explorer itself applies them immediately.

## Testing status

Type-checked only with x86_64-w64-mingw32-g++ -fsyntax-only against the
real windhawk_api.h (Windhawk v1.7.3); NOT compiled with Windhawk's real
compiler; NOT tested on a real Windows machine.
*/
// ==WindhawkModSettings==
/*
- enabled: false
  $name: Enabled
  $description: Master toggle. OFF by default.
- disableMinMaxAnimation: true
  $name: Disable minimize/maximize animation
- disableMenuAnimation: true
  $name: Disable menu animation
- disableTooltipAnimation: true
  $name: Disable tooltip animation
- disableComboBoxAnimation: true
  $name: Disable combo box animation
- disableMenuFade: true
  $name: Disable menu fade effect
- disableSelectionFade: true
  $name: Disable selection fade effect
- disableTooltipFade: true
  $name: Disable tooltip fade effect
- disableListBoxSmoothScrolling: true
  $name: Disable list box smooth scrolling
*/
// ==/WindhawkModSettings==

#include <windows.h>

namespace {

BOOL g_applied = FALSE;

struct SavedState {
    BOOL gotAnimation = FALSE;
    int minAnimate = 0;
    BOOL gotMenuAnimation = FALSE;
    BOOL menuAnimation = FALSE;
    BOOL gotTooltipAnimation = FALSE;
    BOOL tooltipAnimation = FALSE;
    BOOL gotComboBoxAnimation = FALSE;
    BOOL comboBoxAnimation = FALSE;
    BOOL gotMenuFade = FALSE;
    BOOL menuFade = FALSE;
    BOOL gotSelectionFade = FALSE;
    BOOL selectionFade = FALSE;
    BOOL gotTooltipFade = FALSE;
    BOOL tooltipFade = FALSE;
    BOOL gotListBoxSmooth = FALSE;
    BOOL listBoxSmooth = FALSE;
};

SavedState g_saved;

constexpr DWORD kSpiFlags = SPIF_UPDATEINIFILE | SPIF_SENDCHANGE;

inline PVOID BoolToPVParam(BOOL b) {
    return (PVOID)(INT_PTR)(b ? 1 : 0);
}

inline BOOL Tweak(PCWSTR name) {
    return Wh_GetIntSetting(name);
}

void Apply() {
    if (g_applied) {
        return;
    }

    if (Tweak(L"disableMinMaxAnimation")) {
        ANIMATIONINFO ai{sizeof(ai), 0};
        if (SystemParametersInfoW(SPI_GETANIMATION, sizeof(ai), &ai, 0)) {
            g_saved.minAnimate = ai.iMinAnimate;
            g_saved.gotAnimation = TRUE;
            ai.iMinAnimate = 0;
            SystemParametersInfoW(SPI_SETANIMATION, sizeof(ai), &ai, kSpiFlags);
        }
    }

    if (Tweak(L"disableMenuAnimation")) {
        BOOL v = FALSE;
        if (SystemParametersInfoW(SPI_GETMENUANIMATION, 0, &v, 0)) {
            g_saved.menuAnimation = v;
            g_saved.gotMenuAnimation = TRUE;
            SystemParametersInfoW(SPI_SETMENUANIMATION, 0, BoolToPVParam(FALSE),
                                  kSpiFlags);
        }
    }

    if (Tweak(L"disableTooltipAnimation")) {
        BOOL v = FALSE;
        if (SystemParametersInfoW(SPI_GETTOOLTIPANIMATION, 0, &v, 0)) {
            g_saved.tooltipAnimation = v;
            g_saved.gotTooltipAnimation = TRUE;
            SystemParametersInfoW(SPI_SETTOOLTIPANIMATION, 0,
                                  BoolToPVParam(FALSE), kSpiFlags);
        }
    }

    if (Tweak(L"disableComboBoxAnimation")) {
        BOOL v = FALSE;
        if (SystemParametersInfoW(SPI_GETCOMBOBOXANIMATION, 0, &v, 0)) {
            g_saved.comboBoxAnimation = v;
            g_saved.gotComboBoxAnimation = TRUE;
            SystemParametersInfoW(SPI_SETCOMBOBOXANIMATION, 0,
                                  BoolToPVParam(FALSE), kSpiFlags);
        }
    }

    if (Tweak(L"disableMenuFade")) {
        BOOL v = FALSE;
        if (SystemParametersInfoW(SPI_GETMENUFADE, 0, &v, 0)) {
            g_saved.menuFade = v;
            g_saved.gotMenuFade = TRUE;
            SystemParametersInfoW(SPI_SETMENUFADE, 0, BoolToPVParam(FALSE),
                                  kSpiFlags);
        }
    }

    if (Tweak(L"disableSelectionFade")) {
        BOOL v = FALSE;
        if (SystemParametersInfoW(SPI_GETSELECTIONFADE, 0, &v, 0)) {
            g_saved.selectionFade = v;
            g_saved.gotSelectionFade = TRUE;
            SystemParametersInfoW(SPI_SETSELECTIONFADE, 0, BoolToPVParam(FALSE),
                                  kSpiFlags);
        }
    }

    if (Tweak(L"disableTooltipFade")) {
        BOOL v = FALSE;
        if (SystemParametersInfoW(SPI_GETTOOLTIPFADE, 0, &v, 0)) {
            g_saved.tooltipFade = v;
            g_saved.gotTooltipFade = TRUE;
            SystemParametersInfoW(SPI_SETTOOLTIPFADE, 0, BoolToPVParam(FALSE),
                                  kSpiFlags);
        }
    }

    if (Tweak(L"disableListBoxSmoothScrolling")) {
        BOOL v = FALSE;
        if (SystemParametersInfoW(SPI_GETLISTBOXSMOOTHSCROLLING, 0, &v, 0)) {
            g_saved.listBoxSmooth = v;
            g_saved.gotListBoxSmooth = TRUE;
            SystemParametersInfoW(SPI_SETLISTBOXSMOOTHSCROLLING, 0,
                                  BoolToPVParam(FALSE), kSpiFlags);
        }
    }

    g_applied = TRUE;
    Wh_Log(L"Low Spec Mode tweaks applied");
}

void Restore() {
    if (!g_applied) {
        return;
    }

    if (g_saved.gotAnimation) {
        ANIMATIONINFO ai{sizeof(ai), 0};
        ai.iMinAnimate = g_saved.minAnimate;
        SystemParametersInfoW(SPI_SETANIMATION, sizeof(ai), &ai, kSpiFlags);
    }
    if (g_saved.gotMenuAnimation) {
        SystemParametersInfoW(SPI_SETMENUANIMATION, 0,
                              BoolToPVParam(g_saved.menuAnimation), kSpiFlags);
    }
    if (g_saved.gotTooltipAnimation) {
        SystemParametersInfoW(SPI_SETTOOLTIPANIMATION, 0,
                              BoolToPVParam(g_saved.tooltipAnimation),
                              kSpiFlags);
    }
    if (g_saved.gotComboBoxAnimation) {
        SystemParametersInfoW(SPI_SETCOMBOBOXANIMATION, 0,
                              BoolToPVParam(g_saved.comboBoxAnimation),
                              kSpiFlags);
    }
    if (g_saved.gotMenuFade) {
        SystemParametersInfoW(SPI_SETMENUFADE, 0,
                              BoolToPVParam(g_saved.menuFade), kSpiFlags);
    }
    if (g_saved.gotSelectionFade) {
        SystemParametersInfoW(SPI_SETSELECTIONFADE, 0,
                              BoolToPVParam(g_saved.selectionFade), kSpiFlags);
    }
    if (g_saved.gotTooltipFade) {
        SystemParametersInfoW(SPI_SETTOOLTIPFADE, 0,
                              BoolToPVParam(g_saved.tooltipFade), kSpiFlags);
    }
    if (g_saved.gotListBoxSmooth) {
        SystemParametersInfoW(SPI_SETLISTBOXSMOOTHSCROLLING, 0,
                              BoolToPVParam(g_saved.listBoxSmooth), kSpiFlags);
    }

    g_applied = FALSE;
    g_saved = SavedState{};
    Wh_Log(L"Low Spec Mode tweaks restored");
}

} // namespace

BOOL Wh_ModInit(void) {
    if (Wh_GetIntSetting(L"enabled")) {
        Apply();
    }
    return TRUE;
}

void Wh_ModUninit(void) {
    Restore();
}

void Wh_ModSettingsChanged(void) {
    // Re-apply from a clean slate so individual toggle changes take effect.
    Restore();
    if (Wh_GetIntSetting(L"enabled")) {
        Apply();
    }
}
