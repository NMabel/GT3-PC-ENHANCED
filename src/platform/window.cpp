/* window.cpp - the game window of GT3 PC (window.h). */
#include "platform/window.h"
#include "platform/cockpit_osd.h"              /* PC (2026-11-08): the cockpit camera's calibration */
#include "platform/debug_camera.h"
#include "platform/frame_monitor.h"
#include "platform/frame_step.h"
#include "platform/input_map.h"
#include "platform/log.h"
#include "platform/screenshot.h"               /* PC (2026-11-15): the Screenshot hotkey */
#include "platform/settings_menu.h"
#include "platform/song_osd.h"
#include "platform/system_menu.h"
#include "platform/texture_menu.h"
#include "platform/vr.h"                          /* PC VR: the VR hotkeys */

#include <windows.h>

#include <atomic>

#include "main/resource.h"

namespace window {
namespace {

constexpr DWORD kStyleWindowed = WS_OVERLAPPEDWINDOW;
constexpr DWORD kStyleFull = WS_POPUP;

HWND g_hwnd = nullptr;
std::atomic<int> g_clientW{640}, g_clientH{480};
std::atomic<bool> g_closing{false};
std::atomic<bool> g_focus{true};
std::atomic<uint8_t> g_keys[256];
std::atomic<uint8_t> g_keyPressed[256];     /* went down since the last take_key_press */
std::atomic<uint8_t> g_hotPressed[input_map::kHotkeyCount];     /* the same of the debug hotkeys (take_hotkey) */
std::atomic<uint32_t> g_wheelUp{0}, g_wheelDown{0};     /* the mouse wheel's notches (mouse_wheel) */
std::atomic<uint32_t> g_motionX{0}, g_motionY{0};       /* the mouse's raw movement, summed (mouse_motion) */
bool g_clipped;                                         /* the cursor confined to the window (update_mouse) */
std::atomic<int> g_mode{(int)settings::DisplayMode::windowed};
std::atomic<int> g_request{-1};
/* The windowed mode's outer rectangle (kept while full screen). */
RECT g_windowRect;
/* The monitor of the full-screen modes (set_full_monitor; nullptr: the
 * window's). */
std::atomic<HMONITOR> g_fullMonitor{nullptr};

/* The screen windows' requests to the window thread (create_screen,
 * destroy_screen, activate). */
constexpr UINT kMsgScreen = WM_APP + 1;
enum { kScreenCreate = 1, kScreenDestroy, kScreenActivate };
/* PC: a device's press of a hotkey (press_hotkey; wParam the hotkey). */
constexpr UINT kMsgHotkey = WM_APP + 2;
struct ScreenArgs {
    int x, y, w, h;
    HWND hwnd;
};
const wchar_t kScreenClass[] = L"GT3PCScreen";

LRESULT CALLBACK screen_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ACTIVATE:
        /* (exclusive fullscreen may activate it): the keyboard back to the
         * game window */
        if (LOWORD(wp) != WA_INACTIVE && g_hwnd != nullptr)
            PostMessageW(g_hwnd, kMsgScreen, kScreenActivate, 0);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        return 0;                   /* (the game closes them) */
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

settings::DisplayMode cur_mode()
{
    return (settings::DisplayMode)g_mode.load();
}

/* The rectangle of the window's monitor (or of the primary one). */
RECT monitor_rect(HWND hwnd, const RECT *place)
{
    HMONITOR m = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
                      : MonitorFromRect(place, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(m, &mi);
    return mi.rcMonitor;
}

/* The windowed rectangle from the settings: the client size (shrunk at 4:3
 * to fit the work area), at its remembered place if that is on a monitor,
 * else centred on the primary one. */
RECT windowed_rect()
{
    const settings::Display &s = settings::display();
    RECT r = {0, 0, s.windowW, s.windowH};
    AdjustWindowRect(&r, kStyleWindowed, FALSE);
    int w = r.right - r.left, h = r.bottom - r.top;
    RECT place = {s.windowX, s.windowY, s.windowX + w, s.windowY + h};
    HMONITOR m = s.windowPlaced ? MonitorFromRect(&place, MONITOR_DEFAULTTONULL) : nullptr;
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(m ? m : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT &wa = mi.rcWork;
    const int waW = wa.right - wa.left, waH = wa.bottom - wa.top;
    if (w > waW || h > waH) {
        const int fw = w - s.windowW, fh = h - s.windowH;     /* the frame */
        int cw = waW - fw, ch = waH - fh;
        if (cw * 3 > ch * 4)
            cw = ch * 4 / 3;
        else
            ch = cw * 3 / 4;
        w = cw + fw;
        h = ch + fh;
    }
    if (m != nullptr)
        return RECT{s.windowX, s.windowY, s.windowX + w, s.windowY + h};
    const int x = wa.left + (waW - w) / 2, y = wa.top + (waH - h) / 2;
    return RECT{x, y, x + w, y + h};
}

/* Remembers the windowed place (the settings too) unless the window is
 * maximized or minimized. */
void remember_windowed(HWND hwnd)
{
    if (cur_mode() != settings::DisplayMode::windowed || IsIconic(hwnd) || IsZoomed(hwnd))
        return;
    RECT r, c;
    GetWindowRect(hwnd, &r);
    GetClientRect(hwnd, &c);
    g_windowRect = r;
    settings::save_window(r.left, r.top, c.right, c.bottom);
}

void clear_keys()
{
    for (auto &k : g_keys)
        k = 0;
    for (auto &k : g_keyPressed)
        k = 0;
}

/* A mouse button as its virtual key (key_down); the window keeps the mouse
 * while one is held (its release outside the window still arrives). */
void mouse_button(HWND hwnd, int vk, bool down)
{
    g_keys[vk] = down ? 1 : 0;
    if (down) {
        g_keyPressed[vk] = 1;
        SetCapture(hwnd);
    } else if (!g_keys[VK_LBUTTON] && !g_keys[VK_RBUTTON] && !g_keys[VK_MBUTTON] && !g_keys[VK_XBUTTON1] &&
               !g_keys[VK_XBUTTON2]) {
        ReleaseCapture();
    }
}

/* While the debug camera looks with the mouse (debug_camera::wants_mouse)
 * and the window has the focus: the cursor confined to the client area (and
 * hidden there: WM_SETCURSOR); else let go.  again: confined anew (the
 * window moved or changed size). */
void update_mouse(HWND hwnd, bool again)
{
    const bool want = g_focus && debug_camera::wants_mouse() && !IsIconic(hwnd);
    if (want && (!g_clipped || again)) {
        RECT r;
        GetClientRect(hwnd, &r);
        MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT *>(&r), 2);
        ClipCursor(&r);
        g_clipped = true;
    } else if (!want && g_clipped) {
        ClipCursor(nullptr);
        g_clipped = false;
    }
}

/* Hotkey h's action (gt3pc.ini [Hotkeys], input_map.h; defaults in the
 * comments): its key going down (repeat: a held key's repeat), or a device's
 * press (press_hotkey: never a repeat). */
void run_hotkey(HWND hwnd, int h, bool repeat)
{
    switch (h) {
    case input_map::kHotTextureMenu:
        /* PC (2026-11-13): F10, not repeated: the texture interface
         * (texture_menu.h), which takes its own keys only */
        if (!repeat) {
            system_menu::hide();
            settings_menu::hide();
            texture_menu::toggle();
        }
        break;
    case input_map::kHotTextureToggle:
        /* PC (2026-11-13): F12, not repeated: the replacement textures off /
         * on (texture_menu.h) */
        if (!repeat)
            texture_menu::replace_key();
        break;
    case input_map::kHotTextureRefresh:
        /* PC (2026-11-13): Ctrl+F10, not repeated: the texture folders read
         * again (texture_menu.h) */
        if (!repeat)
            texture_menu::refresh_key();
        break;
    case input_map::kHotScreenshot:
        /* PC (2026-11-15): F1, not repeated: a screenshot of the next picture
         * (screenshot.h), the menus shown too (they are not in it) */
        if (!repeat)
            screenshot::request();
        break;
    case input_map::kHotSettingsMenu:
        /* F2, not repeated: the settings menu, which takes the keys while it
         * is shown (the game's held keys let go) */
        if (!repeat) {
            texture_menu::hide();           /* PC (2026-11-13) */
            system_menu::hide();
            settings_menu::toggle();
            clear_keys();
        }
        break;
    case input_map::kHotPause:
        /* F5 (repeats too): pause / one tick; F6: run again (frame_step.h);
         * with the settings menu shown too */
        frame_step::key_step();
        break;
    case input_map::kHotResume:
        if (!repeat)
            frame_step::key_resume();
        break;
    case input_map::kHotFullscreen:
        /* Alt+Enter, not repeated: windowed <-> full screen */
        if (!repeat) {
            settings::DisplayMode to = settings::DisplayMode::windowed;
            if (cur_mode() == settings::DisplayMode::windowed) {
                to = settings::display().mode;
                if (to == settings::DisplayMode::windowed)
                    to = settings::DisplayMode::borderless;
            }
            g_request = (int)to;
        }
        break;
    case input_map::kHotFrameMonitor:
        /* F3, not repeated: the frame time monitor (the menus shown too) */
        if (!repeat)
            frame_monitor::toggle();
        break;
    case input_map::kHotDebugCamera:
        /* F7, not repeated: the debug camera, in a race (debug_camera.h) */
        if (!repeat) {
            debug_camera::key();
            update_mouse(hwnd, false);
        }
        break;
    case input_map::kHotVrRecenter:
    case input_map::kHotVrHud:
    case input_map::kHotVrMirror:
    case input_map::kHotVrScaleDown:
    case input_map::kHotVrScaleUp:
        /* PC VR (vr.h): the scale keys repeat while held */
        if (!repeat || h == input_map::kHotVrScaleDown || h == input_map::kHotVrScaleUp)
            vr::hotkey(h == input_map::kHotVrRecenter ? vr::kActRecenter
                       : h == input_map::kHotVrHud     ? vr::kActHud
                       : h == input_map::kHotVrMirror  ? vr::kActMirror
                       : h == input_map::kHotVrScaleDown ? vr::kActScaleDown
                                                         : vr::kActScaleUp);
        break;
    case input_map::kHotFlickerCapture:
    case input_map::kHotTrace:
    case input_map::kHotFrameCapture:
        /* F8 / F9 / F11, not repeated: the debug captures' (take_hotkey) */
        if (!repeat)
            g_hotPressed[h] = 1;
        break;
    case input_map::kHotSystemMenu:
        /* Esc, not repeated: the Esc menu (Back, Restart, Exit), which takes
         * the keys as the settings menu does */
        if (!repeat) {
            system_menu::toggle();
            clear_keys();
        }
        break;
    case input_map::kHotSongMenu:
        /* PC: M / Select, not repeated: the race's song changer
         * (song_osd.h); Comma / D-pad up, Period / D-pad down: its previous
         * / next song while it is open */
        if (!repeat)
            song_osd::toggle();
        break;
    case input_map::kHotPreviousSong:
    case input_map::kHotNextSong:
        if (!repeat)
            song_osd::step(h == input_map::kHotNextSong ? 1 : -1);
        break;
    case input_map::kHotCalibrationUp:
    case input_map::kHotCalibrationDown:
        /* PC (2026-11-08): the cockpit camera's calibration (cockpit_osd.h),
         * while it is on: Up / Down the axis, Left / Right a step of it (a
         * held key's repeats step on), F4 save, Delete cancel, 0 the axis
         * to 0 */
        if (!repeat)
            cockpit_osd::axis_key(h == input_map::kHotCalibrationDown ? 1 : -1);
        break;
    case input_map::kHotCalibrationLeft:
    case input_map::kHotCalibrationRight:
        cockpit_osd::adjust_key(h == input_map::kHotCalibrationRight ? 1 : -1);
        break;
    case input_map::kHotCalibrationSave:
    case input_map::kHotCalibrationCancel:
    case input_map::kHotCalibrationReset:
        if (!repeat) {
            if (h == input_map::kHotCalibrationSave)
                cockpit_osd::save_key();
            else if (h == input_map::kHotCalibrationCancel)
                cockpit_osd::cancel_key();
            else
                cockpit_osd::reset_key();
        }
        break;
    }
}

/* PC: a panel's hotkey (the song changer's, the calibration's): the menus'
 * own while one is shown. */
bool panel_hotkey(int h)
{
    return h == input_map::kHotSongMenu || h == input_map::kHotPreviousSong || h == input_map::kHotNextSong ||
           input_map::hotkey_shares_keys(h);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        g_clientW = LOWORD(lp);
        g_clientH = HIWORD(lp);
        if (g_clipped)
            update_mouse(hwnd, true);
        return 0;
    case WM_MOVE:
        if (g_clipped)
            update_mouse(hwnd, true);
        break;
    case WM_EXITSIZEMOVE:
        remember_windowed(hwnd);
        return 0;
    case WM_ACTIVATE:
        g_focus = LOWORD(wp) != WA_INACTIVE;
        if (!g_focus)
            clear_keys();
        update_mouse(hwnd, false);
        break;      /* DefWindowProc gives the window the keyboard focus */
    case WM_SETFOCUS:
        g_focus = true;
        return 0;
    case WM_KILLFOCUS:
        g_focus = false;
        clear_keys();
        update_mouse(hwnd, false);
        return 0;
    case WM_INPUT: {
        /* the mouse's raw movement (the debug camera's look: relative, not
         * held at the screen's edges) */
        RAWINPUT ri;
        UINT size = sizeof(ri);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) && g_focus) {
            g_motionX += (uint32_t)ri.data.mouse.lLastX;
            g_motionY += (uint32_t)ri.data.mouse.lLastY;
        }
        update_mouse(hwnd, false);
        break;      /* (DefWindowProc cleans up) */
    }
    case WM_SETCURSOR:
        /* no mouse in the game: no pointer over a full-screen picture (nor
         * while the debug camera looks with the mouse) */
        if (LOWORD(lp) == HTCLIENT &&
            (cur_mode() != settings::DisplayMode::windowed || debug_camera::wants_mouse())) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        /* the hotkeys (gt3pc.ini [Hotkeys], input_map.h: the key with
         * exactly its modifiers; defaults in the comments), the menus' keys,
         * then the game's: a hotkey's key does not reach the game */
        const int vk = (int)wp;
        const bool repeat = (lp & (1 << 30)) != 0;
        const bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0,
                   shift = GetKeyState(VK_SHIFT) < 0;
        auto hot = [&](int h) { return input_map::is_hotkey(h, vk, ctrl, alt, shift); };
        /* (run_hotkey: what each does) F2, F5, F6, Alt+Enter, F3, F7, F8,
         * F9: with the menus shown too */
        static const int kFirst[] = {input_map::kHotSettingsMenu, input_map::kHotPause, input_map::kHotResume,
                                     input_map::kHotFullscreen, input_map::kHotFrameMonitor,
                                     input_map::kHotDebugCamera, input_map::kHotFlickerCapture,
                                     input_map::kHotTrace, input_map::kHotFrameCapture,
                                     input_map::kHotTextureMenu, input_map::kHotTextureRefresh,
                                     input_map::kHotTextureToggle, input_map::kHotScreenshot,
                                     input_map::kHotVrRecenter, input_map::kHotVrHud, input_map::kHotVrMirror,
                                     input_map::kHotVrScaleDown, input_map::kHotVrScaleUp};
        for (int h : kFirst)
            if (hot(h)) {
                run_hotkey(hwnd, h, repeat);
                return 0;
            }
        /* PC (2026-11-13): the texture interface's own keys while it is
         * shown (the others go on to the game) */
        if (texture_menu::shown() && msg == WM_KEYDOWN && texture_menu::key(vk, repeat))
            return 0;
        if (settings_menu::shown() && msg == WM_KEYDOWN) {
            settings_menu::key(vk, repeat);
            return 0;
        }
        /* Esc: the settings menu's own while it is shown */
        if (hot(input_map::kHotSystemMenu)) {
            run_hotkey(hwnd, input_map::kHotSystemMenu, repeat);
            return 0;
        }
        if (system_menu::shown() && msg == WM_KEYDOWN) {
            system_menu::key(vk, repeat);
            return 0;
        }
        /* PC: the song changer's and the calibration's keys: the menus' own
         * while one is shown; the calibration's only while it is on (the
         * arrows, Delete, 0 are the game's otherwise: hotkey_shares_keys) */
        for (int h = 0; h < input_map::kHotkeyCount; h++) {
            if (!panel_hotkey(h) || !hot(h))
                continue;
            if (input_map::hotkey_shares_keys(h) && !cockpit_osd::active())
                break;
            run_hotkey(hwnd, h, repeat);
            return 0;
        }
        if (wp < 256) {
            g_keys[wp] = 1;
            g_keyPressed[wp] = 1;
        }
        if (msg == WM_SYSKEYDOWN && wp == VK_F4)
            break;
        return 0;
    }
    case WM_SYSCHAR:
        if (wp == VK_RETURN)
            return 0;               /* Alt+Enter: no beep */
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (wp < 256)
            g_keys[wp] = 0;
        return 0;
    case WM_MOUSEACTIVATE:
        /* the click that activates the window is not a mouse button press */
        if (LOWORD(lp) == HTCLIENT && !g_focus)
            return MA_ACTIVATEANDEAT;
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        mouse_button(hwnd, VK_LBUTTON, msg == WM_LBUTTONDOWN);
        return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        mouse_button(hwnd, VK_RBUTTON, msg == WM_RBUTTONDOWN);
        return 0;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        mouse_button(hwnd, VK_MBUTTON, msg == WM_MBUTTONDOWN);
        return 0;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        mouse_button(hwnd, GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2, msg == WM_XBUTTONDOWN);
        return TRUE;
    case WM_CAPTURECHANGED:
        /* the mouse taken from the window: its buttons let go */
        if ((HWND)lp != hwnd) {
            static const int kMouseKeys[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
            for (int vk : kMouseKeys)
                g_keys[vk] = 0;
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (GET_WHEEL_DELTA_WPARAM(wp) > 0)
            g_wheelUp++;
        else if (GET_WHEEL_DELTA_WPARAM(wp) < 0)
            g_wheelDown++;
        return 0;
    case WM_CLOSE:
        remember_windowed(hwnd);
        g_closing = true;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case kMsgHotkey: {
        /* PC: a device's press (press_hotkey) as the key's, with the menus
         * as the keys find them: the Esc menu's closes the settings menu (as
         * the Guide button's hold, gamepad.h), the song changer's are the
         * menus' while one is shown */
        const int h = (int)wp;
        if (panel_hotkey(h) && (settings_menu::shown() || system_menu::shown()))
            return 0;
        if (h == input_map::kHotSystemMenu)
            settings_menu::hide();
        run_hotkey(hwnd, h, false);
        return 0;
    }
    case kMsgScreen:
        if (wp == kScreenCreate) {
            ScreenArgs *a = (ScreenArgs *)lp;
            a->hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kScreenClass, L"Gran Turismo 3 A-Spec",
                                      WS_POPUP, a->x, a->y, a->w, a->h, hwnd, nullptr, GetModuleHandleW(nullptr),
                                      nullptr);
            if (a->hwnd != nullptr)
                ShowWindow(a->hwnd, SW_SHOWNOACTIVATE);
        } else if (wp == kScreenDestroy) {
            DestroyWindow((HWND)lp);
        } else if (wp == kScreenActivate && g_focus.load() == false && GetForegroundWindow() != nullptr) {
            /* only while the game has the foreground (its own screen took it) */
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            if (pid == GetCurrentProcessId())
                SetForegroundWindow(hwnd);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

bool create(const char *title)
{
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_GT3PC));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_GT3PC), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"GT3PCWindow";
    if (!RegisterClassExW(&wc))
        return false;
    /* the screen windows' (create_screen) */
    WNDCLASSEXW sc = wc;
    sc.style = 0;
    sc.lpfnWndProc = screen_proc;
    sc.lpszClassName = kScreenClass;
    RegisterClassExW(&sc);

    const settings::DisplayMode m = settings::display().mode;
    g_mode = (int)m;
    g_windowRect = windowed_rect();
    RECT r = g_windowRect;
    DWORD style = kStyleWindowed;
    if (m != settings::DisplayMode::windowed) {
        r = monitor_rect(nullptr, &g_windowRect);
        style = kStyleFull;
    }
    wchar_t wtitle[256];
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 256);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, wtitle, style, r.left, r.top, r.right - r.left,
                             r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd)
        return false;
    RECT c;
    GetClientRect(g_hwnd, &c);
    g_clientW = c.right;
    g_clientH = c.bottom;
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    SetForegroundWindow(g_hwnd);
    SetFocus(g_hwnd);
    /* the mouse's raw movement (WM_INPUT, while in the foreground: the
     * debug camera's look; the usual mouse messages go on) */
    const RAWINPUTDEVICE mouse = {0x01, 0x02, 0, g_hwnd};     /* generic desktop, mouse */
    if (!RegisterRawInputDevices(&mouse, 1, sizeof(mouse)))
        pclog::write("window: no raw mouse input (%lu): the debug camera cannot look with the mouse",
                     (unsigned long)GetLastError());
    pclog::write("window: %s, client %dx%d", settings::mode_name(m), (int)c.right, (int)c.bottom);
    return true;
}

HWND handle()
{
    return g_hwnd;
}

int run_message_loop()
{
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

void client_size(int *w, int *h)
{
    *w = g_clientW;
    *h = g_clientH;
}

bool closing()
{
    return g_closing;
}

bool key_down(int vk)
{
    return vk >= 0 && vk < 256 && g_focus && g_keys[vk] != 0;
}

bool take_key_press(int vk)
{
    return vk >= 0 && vk < 256 && g_keyPressed[vk].exchange(0) != 0 && g_focus;
}

bool take_hotkey(int h)
{
    return h >= 0 && h < input_map::kHotkeyCount && g_hotPressed[h].exchange(0) != 0 && g_focus;
}

void press_hotkey(int h)
{
    if (g_hwnd != nullptr && h >= 0 && h < input_map::kHotkeyCount)
        PostMessageW(g_hwnd, kMsgHotkey, (WPARAM)h, 0);
}

void mouse_wheel(uint32_t *up, uint32_t *down)
{
    *up = g_wheelUp.load();
    *down = g_wheelDown.load();
}

void mouse_motion(uint32_t *x, uint32_t *y)
{
    *x = g_motionX.load();
    *y = g_motionY.load();
}

settings::DisplayMode mode()
{
    return cur_mode();
}

bool take_mode_request(settings::DisplayMode *m)
{
    int r = g_request.exchange(-1);
    if (r < 0)
        return false;
    *m = (settings::DisplayMode)r;
    return true;
}

void request_mode(settings::DisplayMode m)
{
    g_request = (int)m;
}

void apply_mode(settings::DisplayMode m)
{
    HWND hwnd = g_hwnd;
    if (cur_mode() == settings::DisplayMode::windowed && m != settings::DisplayMode::windowed)
        remember_windowed(hwnd);
    RECT r;
    DWORD style;
    if (m == settings::DisplayMode::windowed) {
        style = kStyleWindowed;
        r = g_windowRect;
    } else {
        style = kStyleFull;
        r = monitor_rect(hwnd, nullptr);
        if (HMONITOR fm = g_fullMonitor.load()) {     /* the span's (set_full_monitor) */
            MONITORINFO mi = {};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(fm, &mi))
                r = mi.rcMonitor;
        }
    }
    g_mode = (int)m;
    if (IsZoomed(hwnd) || IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);
    SetWindowLongPtrW(hwnd, GWL_STYLE, (LONG_PTR)(style | WS_VISIBLE));
    SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    pclog::write("window: %s", settings::mode_name(m));
}

bool active()
{
    return g_focus && g_hwnd != nullptr && !IsIconic(g_hwnd);
}

HWND create_screen(int x, int y, int w, int h)
{
    ScreenArgs a = {x, y, w, h, nullptr};
    SendMessageW(g_hwnd, kMsgScreen, kScreenCreate, (LPARAM)&a);
    return a.hwnd;
}

void destroy_screen(HWND hwnd)
{
    if (hwnd != nullptr)
        SendMessageW(g_hwnd, kMsgScreen, kScreenDestroy, (LPARAM)hwnd);
}

void set_full_monitor(void *monitor)
{
    g_fullMonitor = (HMONITOR)monitor;
}

void activate()
{
    PostMessageW(g_hwnd, kMsgScreen, kScreenActivate, 0);
}

} // namespace window
