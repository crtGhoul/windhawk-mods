// ==WindhawkMod==
// @id              taskbar-quick-launch
// @name            Taskbar Quick Launch
// @description     Classic Quick Launch: a small always-visible toolbar docked on the taskbar with one-click app launchers (a toolbar window, not a COM deskband — no registration needed)
// @version         1.0
// @author          crtGhoul
// @github          https://github.com/crtGhoul
// @license         MIT
// @include         explorer.exe
// @compilerOptions -lshell32 -lgdi32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Taskbar Quick Launch

Brings back a classic Quick Launch-style strip on the Windows 11 taskbar:
a small row of app icons docked just left of the notification area.
Click an icon to launch the app; hover for its file-name tooltip.

Deliberate tradeoff: this is **not** a true COM deskband. A real deskband
needs COM registration and deep taskbar-band hosting (ExplorerPatcher /
StartAllBack territory). Instead this mod creates one plain `WS_CHILD`
toolbar window on `Shell_TrayWnd` plus one child window per launcher
button — zero registration, zero COM, and it uninstalls by simply
destroying the windows. The cost: the strip sits at a fixed offset left
of the tray (it can't be dragged to reorder like a real deskband), and
it only appears on the primary taskbar.

How it works:

- `appsList` is split on `;` (max 16 entries). Each entry is a full path
  or an exe name resolvable via `PATH`.
- Icons come from `ExtractIconW`; if an entry has no icon, the generic
  application icon is used.
- The toolbar repositions itself whenever `Shell_TrayWnd` is resized or
  moved (subclassed `WM_WINDOWPOSCHANGED` — fully event-driven, no timers).
- The toolbar background is sampled from the taskbar pixel just left of
  it, so it blends in on solid-color taskbars (an approximation on
  acrylic/transparent taskbars).

## Settings

- **Enabled** — master switch.
- **Apps** — semicolon-separated list of paths or exe names, max 16.
- **Icon size (px)** — 16–64, default 24.

## Testing status

- Type-checked only with `x86_64-w64-mingw32-g++ -fsyntax-only` against
  the real `windhawk_api.h` (Windhawk v1.7.3).
- NOT compiled with Windhawk's real compiler.
- NOT tested on a real Windows machine — needs real-PC validation:
  icons render, clicks launch, strip survives taskbar resizes and
  explorer restarts, unload leaves no windows behind.
*/
// ==WindhawkModSettings==
/*
- enabled: true
  $name: Enabled
  $description: Master switch for the Quick Launch toolbar.
- appsList: "notepad.exe;mspaint.exe;calc.exe"
  $name: Apps
  $description: Semicolon-separated paths or exe names, max 16.
- iconSizePx: 24
  $name: Icon size (px)
  $description: Icon size in pixels (16-64).
*/
// ==/WindhawkModSettings==

#include <commctrl.h>
#include <windows.h>
#include <windowsx.h>

#include <string>
#include <vector>

namespace {

constexpr int kMaxApps = 16;
constexpr int kPad = 4;

const wchar_t kBarClass[] = L"CrtGhoulQuickLaunchBar";
const wchar_t kBtnClass[] = L"CrtGhoulQuickLaunchBtn";

struct Button {
    HWND hwnd = nullptr;
    HICON icon = nullptr;
    bool ownIcon = false;  // false for the shared IDI_APPLICATION fallback
    std::wstring path;
    std::wstring tip;
    bool hover = false;
    bool pressed = false;
};

bool g_enabled = true;
int g_iconSize = 24;
std::vector<std::wstring> g_apps;

HWND g_shellTray = nullptr;
HWND g_trayNotify = nullptr;
HWND g_toolbar = nullptr;
HWND g_tooltip = nullptr;
WNDPROC g_origTrayProc = nullptr;
std::vector<Button> g_buttons;
COLORREF g_bgColor = RGB(0, 0, 0);
bool g_classesRegistered = false;

Button* ButtonFromHwnd(HWND hWnd) {
    return reinterpret_cast<Button*>(
        GetWindowLongPtrW(hWnd, GWLP_USERDATA));
}

std::wstring FileNameOf(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

HICON ExtractAppIcon(const std::wstring& path, bool& ownIcon) {
    ownIcon = false;
    HICON icon =
        ExtractIconW(GetModuleHandleW(nullptr), path.c_str(), 0);
    if (icon && icon != reinterpret_cast<HICON>(1)) {
        ownIcon = true;
        return icon;
    }
    return LoadIconW(nullptr, IDI_APPLICATION);
}

LRESULT CALLBACK ButtonWndProc(HWND hWnd, UINT uMsg, WPARAM wParam,
                               LPARAM lParam) {
    Button* btn = ButtonFromHwnd(hWnd);
    if (uMsg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }
    if (!btn) {
        return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }

    switch (uMsg) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc{};
            GetClientRect(hWnd, &rc);
            if (btn->hover) {
                HPEN pen = CreatePen(PS_SOLID, 1,
                                     GetSysColor(COLOR_HIGHLIGHT));
                HGDIOBJ oldPen = SelectObject(hdc, pen);
                HGDIOBJ oldBrush =
                    SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
                SelectObject(hdc, oldBrush);
                SelectObject(hdc, oldPen);
                DeleteObject(pen);
            }
            int offset = btn->pressed ? 1 : 0;
            DrawIconEx(hdc, rc.left + offset + kPad,
                       rc.top + offset + kPad, btn->icon, g_iconSize,
                       g_iconSize, 0, nullptr, DI_NORMAL);
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!btn->hover) {
                btn->hover = true;
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hWnd;
                TrackMouseEvent(&tme);
                InvalidateRect(hWnd, nullptr, TRUE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            btn->hover = false;
            btn->pressed = false;
            InvalidateRect(hWnd, nullptr, TRUE);
            return 0;
        case WM_LBUTTONDOWN:
            btn->pressed = true;
            SetCapture(hWnd);
            InvalidateRect(hWnd, nullptr, TRUE);
            return 0;
        case WM_LBUTTONUP: {
            bool wasPressed = btn->pressed;
            btn->pressed = false;
            ReleaseCapture();
            InvalidateRect(hWnd, nullptr, TRUE);
            if (wasPressed) {
                POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                RECT rc{};
                GetClientRect(hWnd, &rc);
                if (PtInRect(&rc, pt)) {
                    Wh_Log(L"Launching %s", btn->path.c_str());
                    ShellExecuteW(nullptr, L"open", btn->path.c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            return 0;
        }
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK ToolbarWndProc(HWND hWnd, UINT uMsg, WPARAM wParam,
                                 LPARAM lParam) {
    if (uMsg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        HBRUSH brush = CreateSolidBrush(g_bgColor);
        FillRect(hdc, &rc, brush);
        DeleteObject(brush);
        EndPaint(hWnd, &ps);
        return 0;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

void DestroyButtons() {
    for (auto& btn : g_buttons) {
        if (btn.hwnd && IsWindow(btn.hwnd)) {
            DestroyWindow(btn.hwnd);
        }
        if (btn.icon && btn.ownIcon) {
            DestroyIcon(btn.icon);
        }
    }
    g_buttons.clear();
    if (g_tooltip && IsWindow(g_tooltip)) {
        DestroyWindow(g_tooltip);
        g_tooltip = nullptr;
    }
}

void BuildButtons() {
    DestroyButtons();
    if (!g_toolbar || g_apps.empty()) {
        return;
    }

    g_buttons.reserve(kMaxApps);  // USERDATA pointers must stay stable

    g_tooltip = CreateWindowExW(0, TOOLTIPS_CLASSW, nullptr,
                                WS_POPUP | TTS_ALWAYSTIP, CW_USEDEFAULT,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                g_toolbar, nullptr,
                                GetModuleHandleW(nullptr), nullptr);

    for (size_t i = 0; i < g_apps.size(); i++) {
        Button btn;
        btn.path = g_apps[i];
        btn.tip = FileNameOf(btn.path);
        btn.icon = ExtractAppIcon(btn.path, btn.ownIcon);
        g_buttons.push_back(std::move(btn));
        Button* stored = &g_buttons.back();

        int x = kPad + static_cast<int>(i) * (g_iconSize + kPad);
        stored->hwnd = CreateWindowExW(
            0, kBtnClass, L"", WS_CHILD | WS_VISIBLE, x, kPad, g_iconSize,
            g_iconSize, g_toolbar, nullptr, GetModuleHandleW(nullptr),
            stored);
        if (!stored->hwnd) {
            HICON failedIcon = stored->icon;
            bool failedOwn = stored->ownIcon;
            g_buttons.pop_back();
            if (failedIcon && failedOwn) {
                DestroyIcon(failedIcon);
            }
            continue;
        }

        if (g_tooltip) {
            TOOLINFOW ti{};
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd = stored->hwnd;
            ti.uId = reinterpret_cast<UINT_PTR>(stored->hwnd);
            ti.lpszText = stored->tip.data();
            SendMessageW(g_tooltip, TTM_ADDTOOLW, 0,
                         reinterpret_cast<LPARAM>(&ti));
        }
    }
}

void RepositionToolbar() {
    if (!g_toolbar || !g_shellTray || !IsWindow(g_toolbar)) {
        return;
    }
    if (!g_enabled || g_buttons.empty() || !g_trayNotify ||
        !IsWindow(g_trayNotify)) {
        ShowWindow(g_toolbar, SW_HIDE);
        return;
    }

    RECT rcTray{};
    GetClientRect(g_shellTray, &rcTray);
    RECT rcNotify{};
    GetWindowRect(g_trayNotify, &rcNotify);
    MapWindowPoints(HWND_DESKTOP, g_shellTray,
                    reinterpret_cast<POINT*>(&rcNotify), 2);

    int barH = g_iconSize + 2 * kPad;
    int barW = static_cast<int>(g_buttons.size()) * (g_iconSize + kPad) + kPad;
    int x = rcNotify.left - barW - 6;
    int y = (rcTray.bottom - rcTray.top - barH) / 2;
    if (y < 0) {
        y = 0;
    }
    if (x < 0) {
        ShowWindow(g_toolbar, SW_HIDE);
        return;
    }

    // Blend with the taskbar: sample the pixel just left of the toolbar.
    HDC hdcTray = GetDC(g_shellTray);
    if (hdcTray) {
        COLORREF sampled = GetPixel(hdcTray, x - 2, y + barH / 2);
        if (sampled != CLR_INVALID) {
            g_bgColor = sampled;
        }
        ReleaseDC(g_shellTray, hdcTray);
    }

    MoveWindow(g_toolbar, x, y, barW, barH, TRUE);
    ShowWindow(g_toolbar, SW_SHOW);
    InvalidateRect(g_toolbar, nullptr, TRUE);
}

LRESULT CALLBACK TrayWndSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam,
                                      LPARAM lParam) {
    LRESULT result = CallWindowProcW(g_origTrayProc, hWnd, uMsg, wParam,
                                     lParam);
    if (uMsg == WM_WINDOWPOSCHANGED || uMsg == WM_SIZE) {
        RepositionToolbar();
    }
    return result;
}

std::wstring Trim(const std::wstring& s) {
    const wchar_t* ws = L" \t\r\n";
    size_t start = s.find_first_not_of(ws);
    if (start == std::wstring::npos) {
        return L"";
    }
    size_t end = s.find_last_not_of(ws);
    return s.substr(start, end - start + 1);
}

void LoadSettings() {
    g_enabled = Wh_GetIntSetting(L"enabled") != 0;

    int iconSize = Wh_GetIntSetting(L"iconSizePx");
    if (iconSize < 16) {
        iconSize = 16;
    }
    if (iconSize > 64) {
        iconSize = 64;
    }
    g_iconSize = iconSize;

    g_apps.clear();
    PCWSTR raw = Wh_GetStringSetting(L"appsList");
    std::wstring list = raw ? raw : L"";
    Wh_FreeStringSetting(raw);

    size_t start = 0;
    while (start < list.size() && g_apps.size() < kMaxApps) {
        size_t end = list.find(L';', start);
        std::wstring part = Trim(list.substr(
            start, end == std::wstring::npos ? std::wstring::npos
                                             : end - start));
        if (!part.empty()) {
            g_apps.push_back(part);
        }
        if (end == std::wstring::npos) {
            break;
        }
        start = end + 1;
    }
}

bool RegisterClasses() {
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    WNDCLASSW wcBar{};
    wcBar.lpfnWndProc = ToolbarWndProc;
    wcBar.hInstance = hInst;
    wcBar.lpszClassName = kBarClass;
    wcBar.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&wcBar)) {
        return false;
    }

    WNDCLASSW wcBtn{};
    wcBtn.lpfnWndProc = ButtonWndProc;
    wcBtn.hInstance = hInst;
    wcBtn.lpszClassName = kBtnClass;
    wcBtn.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&wcBtn)) {
        UnregisterClassW(kBarClass, hInst);
        return false;
    }

    g_classesRegistered = true;
    return true;
}

void CreateToolbar() {
    if (g_toolbar) {
        return;
    }
    g_shellTray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!g_shellTray) {
        Wh_Log(L"Shell_TrayWnd not found");
        return;
    }
    g_trayNotify =
        FindWindowExW(g_shellTray, nullptr, L"TrayNotifyWnd", nullptr);

    g_toolbar = CreateWindowExW(0, kBarClass, L"", WS_CHILD | WS_CLIPSIBLINGS,
                                0, 0, 0, 0, g_shellTray, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!g_toolbar) {
        Wh_Log(L"Failed to create toolbar, error %lu", GetLastError());
        return;
    }

    g_origTrayProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        g_shellTray, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(TrayWndSubclassProc)));
    if (!g_origTrayProc) {
        Wh_Log(L"Failed to subclass Shell_TrayWnd");
        DestroyWindow(g_toolbar);
        g_toolbar = nullptr;
        return;
    }

    BuildButtons();
    RepositionToolbar();
}

void DestroyToolbar() {
    DestroyButtons();
    if (g_shellTray && g_origTrayProc && IsWindow(g_shellTray)) {
        SetWindowLongPtrW(g_shellTray, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(g_origTrayProc));
    }
    g_origTrayProc = nullptr;
    if (g_toolbar && IsWindow(g_toolbar)) {
        DestroyWindow(g_toolbar);
    }
    g_toolbar = nullptr;
    g_shellTray = nullptr;
    g_trayNotify = nullptr;
}

}  // namespace

BOOL Wh_ModInit(void) {
    Wh_Log(L"Init");
    LoadSettings();
    if (!g_enabled) {
        return TRUE;
    }
    if (!RegisterClasses()) {
        Wh_Log(L"Failed to register window classes");
        return TRUE;  // stay loaded; user may enable later
    }
    CreateToolbar();
    return TRUE;
}

void Wh_ModUninit(void) {
    Wh_Log(L"Uninit");
    DestroyToolbar();
    if (g_classesRegistered) {
        HINSTANCE hInst = GetModuleHandleW(nullptr);
        UnregisterClassW(kBtnClass, hInst);
        UnregisterClassW(kBarClass, hInst);
        g_classesRegistered = false;
    }
}

void Wh_ModSettingsChanged(void) {
    Wh_Log(L"Settings changed");
    bool wasEnabled = g_enabled;
    LoadSettings();
    if (g_enabled && !wasEnabled) {
        if (!g_classesRegistered && !RegisterClasses()) {
            return;
        }
        CreateToolbar();
    } else if (!g_enabled && wasEnabled) {
        DestroyToolbar();
    } else if (g_enabled) {
        if (!g_toolbar) {
            CreateToolbar();
        } else {
            BuildButtons();
            RepositionToolbar();
        }
    }
}
