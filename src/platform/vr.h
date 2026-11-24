/* vr.h - PC VR (OpenXR): the headset session of GT3 PC.  No original: new PC
 * code, a fork's feature.  How the pieces fit together (the frame's journey,
 * what is drawn where, how to set it up) is in docs/vr.md.
 *
 * gt3pc.ini [VR]:
 *   Enabled=off | on     on: start an OpenXR session when a headset is found
 *                        (SteamVR's runtime; the game is 32-bit, so the
 *                        runtime must register a 32-bit OpenXR client - SteamVR
 *                        does).  Without a headset, a runtime or the loader
 *                        DLL the game runs as without VR (logged).  The
 *                        launcher sets it: Game > Cameras > VR headset
 *                        (OpenXR), which also turns [Graphics] CockpitCamera
 *                        on, the one view drawn in stereo.
 *   MenuDistance=2.5     metres: where the flat screen (menus, and every
 *                        picture that is not a stereo race view) hangs
 *   MenuWidth=4.0        metres: its width (16:9)
 *   RenderScale=1.0      0.5 .. 2: the eye textures' size, times the runtime's
 *                        recommended one (the picture the game draws is that
 *                        size times [Graphics] RenderScale)
 *   WorldScale=100       percent (50 .. 200): the world's size to the eyes -
 *                        above 100 the cockpit looks larger (the eyes' distance
 *                        and the head's movements shrink)
 *   Hud=on | off         the dashboard (the speed, the revs, the lap): a panel
 *                        of the cockpit at HudDistance, HudWidth, HudHeight
 *                        (centimetres; the height from the eyes, below
 *                        negative)
 *   Mirror=on | off      the rear-view mirror, a panel of its own at
 *                        MirrorDistance, MirrorWidth, MirrorHeight
 *   ScreenDistance=90    the menu panel (the pause menu, the race's result), in
 *   ScreenWidth=80       the same centimetres; ScreenHeight 0 is at the eyes
 *   ScreenHeight=0
 *   MenuFov=0            degrees (5 .. 120; 0: off): the width as the angle the
 *                        screen covers, from MenuDistance (THX: ~40, SMPTE: 30)
 *   Shake=on | off       the cockpit's shake in the headset, two kinds that can be
 *                        mixed (50% and 50%): off: none.  Only the headset's view:
 *                        the flat screen is as ever.
 *   HeadShake=100        percent (0 .. 300): the view shakes (the game's own shake by
 *                        the speed and the bumps, plus a road / engine vibration):
 *                        the whole picture moves against your head
 *   CabinShake=0         percent (0 .. 300): the car's body shakes over the road,
 *                        the world and the view stay still
 *   ShakePreset=2        0 custom, 1 subtle, 2 road, 3 rally, 4 earthquake: sets the
 *                        three layers below (any of them changed by hand: 0)
 *   ShakeSway=100        percent (0 .. 300): the layers both kinds are made of -
 *   ShakeEngine=100      road sway (slow), the engine's vibration (follows the rpm and
 *   ShakeBumps=100       how fast it rises) and the kicks of kerbs, bumps, landings,
 *                        collisions and gear changes
 *
 * The session is made on the game's own Direct3D 11 device (the adapter the
 * runtime asks for: gpu.cpp makes the device on it).  Every call here is made
 * from the game thread that presents (pgl_targets.cpp present_now ->
 * gpu::present); none is thread-safe.
 *
 * What the headset shows, frame by frame:
 *   - a race view from the cockpit camera (the player's own car, live race,
 *     pause menu and result included): drawn twice, once per eye, onto the eye
 *     swapchains of a projection layer (vr_view.h is the game's side of it),
 *     with the dashboard, the rear-view mirror and the menu as panels of the
 *     cockpit (the overlays below);
 *   - every other picture - the menus, the other cameras, replays: the "flat
 *     screen", the picture the game presents drawn onto a quad layer,
 *     world-locked in front of the player where they sat down (recentre: the
 *     Home key, or SteamVR's own). */
#pragma once

#include <d3d11.h>

namespace vr {

/* [VR] Enabled. */
bool wanted();

/* Before the game's device is made: the OpenXR instance and the headset
 * system, and the adapter the runtime wants the device on (its LUID).  false:
 * no VR this run (not wanted, no loader DLL, no runtime, no headset: said in
 * the log); then the device is made as without VR. */
bool prepare(LUID *adapter);
/* After the device: the session on it.  false: no VR this run. */
bool start(ID3D11Device *device);
/* The session is running and frames are being exchanged with the compositor
 * (false before the runtime is ready and after it ended the session: the
 * game then goes on flat). */
bool active();
/* Ends the session (at exit, or when the runtime asks). */
void shutdown();

/* ---- the settings ([VR]; the F2 menu's VR category; saved when changed) ---- */
enum Setting { kWorldScale, kHudDistance, kHudWidth, kHudHeight, kMirrorDistance, kMirrorWidth, kMirrorHeight,
               kScreenDistance, kScreenWidth, kScreenHeight, kHeadShake, kCabinShake, kShakePreset,
               kShakeSway, kShakeEngine, kShakeBumps, kSettingCount };
struct SettingInfo {
    const char *key;                    /* the ini key */
    int def, min, max, step;            /* percent (the world's scale), else centimetres */
};
const SettingInfo &setting_info(Setting s);
int setting(Setting s);
void set_setting(Setting s, int value);     /* held to its limits */
bool hud_on();
void set_hud_on(bool on);
bool mirror_on();
void set_mirror_on(bool on);
/* The cockpit's shake in the headset ([VR] Shake, HeadShake, CabinShake; the flat screen's is not touched):
 * off, no shake at all; on, the head shake (the view: the game's own shake and a vibration, times kHeadShake
 * percent) and the cabin shake (the car's body over the road, times kCabinShake percent), mixed as set. */
bool shake_on();
void set_shake_on(bool on);
/* A headset session exists (the F2 menu says so). */
bool available();
/* The VR hotkeys (input_map::kHotVr*, window.cpp; vr.cpp also polls them while the
 * game's window has not the keyboard - the player is in the headset). */
enum Action { kActRecenter, kActHud, kActMirror, kActScaleDown, kActScaleUp };
void hotkey(Action a);

/* ---- the overlays: the dashboard, the mirror and the menu, panels of the cockpit ----
 * kind 0 the dashboard, 1 the mirror, 2 the menu (the pause menu and the result of a
 * race: shown in front of the player while the race stays in the headset).  A render
 * target for this frame's panel (aspect: its width over its height; nullptr / false:
 * none this frame - off, or no stereo frame); the renderer's executor draws the
 * panel's part of the overlay picture onto it (pgl_exec.cpp exec_vr_overlay:
 * overlay_draw on the D3D thread). */
constexpr int kOverlayHud = 0, kOverlayMirror = 1, kOverlayScreen = 2, kOverlayDesktop = 3, kOverlayCount = 4;
/* kind 3, the desktop panels: the game's own panels over the picture (the F2 menu, the Esc menu, the frame
 * monitor, the notices: platform/overlay.h), which the overlay draws a second time onto this panel (not
 * through overlay_acquire / overlay_draw).  The panel's render target for this frame - cleared by the caller
 * when `fresh' (the first call of the frame) - for a picture of this aspect; false: none (no session, or
 * not a frame to draw).  Shown for stereo and flat frames alike, in front of everything else. */
bool desktop_panel_target(float aspect, ID3D11RenderTargetView **rtv, int *width, int *height, bool *fresh);
bool overlay_wanted(int kind);
bool overlay_acquire(int kind, float aspect, ID3D11RenderTargetView **rtv, int *width, int *height);
/* The part uv[4] (u0, v0, u1, v1) of src over all of rtv (width x height); the
 * dashboard (kind 0) and the menu (2) keyed - black is clear - the mirror opaque.  On the D3D
 * thread; the context's state is left as it finds it not: the caller
 * invalidates its caches. */
void overlay_draw(ID3D11DeviceContext *dc, ID3D11ShaderResourceView *src, ID3D11RenderTargetView *rtv, int width,
                  int height, const float uv[4], int kind);

/* The flat screen: the render target this frame's picture is also drawn onto
 * (nullptr / false: no flat screen this frame).  Call once per presented
 * picture, before gpu::present, which ends the frame (end_frame). */
bool flat_acquire(ID3D11RenderTargetView **rtv, int *width, int *height);

/* The stereo cockpit.  A stereo frame can be drawn (vr_view.h ready); the eye
 * textures' size; one eye's render target for this frame (the renderer's
 * executor draws the eye's picture onto it: pgl_exec.cpp exec_vr_eye); false:
 * none. */
bool stereo_ready();
void eye_size(int *width, int *height);
bool eye_acquire(int eye, ID3D11RenderTargetView **rtv, int *width, int *height);

/* Ends the frame: the layers handed to the compositor, then the wait for the
 * next frame (the headset's rate: it paces the game while VR is active).
 * Called by gpu::present with the D3D thread idle. */
void end_frame();

/* The headset's refresh rate (Hz; 0 while no session is running). */
double refresh_hz();

} // namespace vr
