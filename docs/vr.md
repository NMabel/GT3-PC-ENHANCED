# PC VR (SteamVR / OpenXR)

The race's cockpit view in a VR headset, drawn in stereo. Everything else (menus,
other cameras, replays) is shown on a big virtual screen in the headset.

This is a fork feature: it is not in the upstream project. It was developed and
tested with a Valve Index on SteamVR. Any headset whose OpenXR runtime accepts a
**32-bit** client and Direct3D 11 should work, but only that one has been tried.

## Using it

1. Build as usual (`build.cmd`). The first configure downloads the OpenXR SDK
   (a pinned version, checksum verified: `cmake/openxr.cmake`) and builds its
   loader, `openxr_loader.dll`, next to `gt3pc.exe`.
2. Turn it on in the launcher: **Game** tab, **Cameras**, **VR headset (OpenXR): On**.
   VR draws the cockpit camera, so this also switches **Cockpit camera** on (and says
   so in a message), or the race would stay a flat screen. The launcher's on / off
   settings are lists, not check boxes; this one is Off / On like the rest. Or in
   `gt3pc.ini`, next to the game:
   ```ini
   [VR]
   Enabled=on

   [Graphics]
   CockpitCamera=on
   ```
3. Start SteamVR, put the headset on, then run `gt3pc.exe`.
4. In a race, use the **cockpit camera** view (the PC-ENHANCED cockpit camera
   must be on). That is the view drawn in stereo.

Without a headset, a runtime, or the loader DLL the game starts as usual and the
reason is written to `gt3pc.log` (lines starting `vr:`).

### Adding it to an existing install (engine plugin)

The launcher can run another build of the game as an *engine* (its first screen,
the engine selector: **Add engine...**). That is the way to offer this fork's VR
to people who already have the game installed, without replacing anything of
theirs:

1. Run **`Build Mod Plugin.cmd "GT3 PC VR"`**. It builds, then fills
   `build\plugins\GT3 PC VR\` with `gt3launcher.exe`, `gt3pc.exe`, `SDL3.dll`,
   `openxr_loader.dll` (and `openxr_loader-LICENSE.txt`, its Apache-2.0 / MIT
   licence text, which has to go with it) and `engine.txt` (the name the
   selector lists it by). Zip that folder to share it (about 7 MB).
2. The user unzips it anywhere, opens their own launcher, chooses
   **Add engine...** (a warning asks them to trust the source), picks the folder
   and starts it from the selector.
3. The settings are *shared*: the engine reads and writes the user's own
   `gt3pc.ini`, so `[VR] Enabled=on` goes in the file they already have. Their
   installed game is not touched.

What stays the engine's own (`src/launcher/engines.h`): its **save folders**
(`saves\<engine name>\`, beside the shared `gt3pc.ini`: copy a memory card
folder over from `saves\Gran Turismo 3 A-Spec\` to carry on a save) and its
**installed game files** (`data_US` ...: the engine's launcher installs them from
a disc image, **Install game files...**, or plays straight from the image,
**Add disc...**).

`openxr_loader.dll` must be in the folder: without it the engine runs as a flat
screen game and only `gt3pc.log` says why. The script copies it when the build
has one.

### What you see

| Situation | In the headset |
|---|---|
| Cockpit camera, live race | The cockpit in stereo, head-tracked. The dashboard (speed, revs, lap...), the rear-view mirror and the menu are separate panels hanging in the cockpit. |
| Pause menu | The world stands still but stays in stereo; the pause menu is the menu panel. |
| Race finished | The cockpit stays in stereo while the car rolls on; "FINISH" shows on the dashboard, the result board on the menu panel. |
| Any other camera, replays, menus, loading | The flat screen: a large virtual screen where you sat down. |

### Controls

Keys can be rebound in the launcher. They work whether or not the game window has
focus, since the player is in the headset.

| Key | Does |
|---|---|
| `Home` | Centre the view: seat and flat screen where you are now, facing forward |
| `End` (Numpad 1 with NumLock off) | Dashboard on / off |
| `Insert` | Rear-view mirror on / off |
| `[` and `]` | World smaller / larger (hold to repeat) |

The same settings, and where each panel hangs, are in the F2 menu under **VR**.

### Settings (`gt3pc.ini`, `[VR]`)

Distances, widths and heights are in centimetres; height 0 is at your eyes,
below is negative.

| Key | Default | Meaning |
|---|---|---|
| `Enabled` | `off` | Start an OpenXR session when a headset is found |
| `RenderScale` | `1.0` | 0.5 to 2: eye texture size, times the runtime's recommended size |
| `WorldScale` | `100` | Percent, 50 to 200: above 100 the cockpit looks larger |
| `Hud` | `on` | Dashboard panel |
| `HudDistance` / `HudWidth` / `HudHeight` | `100` / `70` / `-10` | Where the dashboard hangs |
| `Mirror` | `on` | Rear-view mirror panel |
| `MirrorDistance` / `MirrorWidth` / `MirrorHeight` | `100` / `50` / `25` | Where the mirror hangs |
| `ScreenDistance` / `ScreenWidth` / `ScreenHeight` | `90` / `80` / `0` | The menu panel (pause menu, race result) |
| `MenuDistance` / `MenuWidth` | `2.5` / `4.0` | Metres: the flat screen |
| `MenuFov` | `0` | Degrees (5 to 120, 0 = off): size the flat screen by the angle it covers instead of `MenuWidth` (THX recommends about 40) |

## How a frame gets to the headset

```
 game thread (render)                          D3D thread (renderer)         headset
 ---------------------------------------------------------------------------------------
 RacePS2Base::render                (T131_race_ps2_render.cpp)
   pc_vr_stereo()? yes:
   for each eye:
     pc_vr_eye_begin(eye)           (pc_vr.cpp)
     drawView()  - the 3D scene     pc_vr_apply() puts the eye's field
                                    of view and head position into the
                                    camera's matrices
     pc_vr_eye_end()  --- CMD_VR_EYE ------------> exec_vr_eye   --> eye swapchain
   pc_vr_overlay_pass():
     mirror  -> canvas -> pc_vr_capture_mirror  -- CMD_VR_OVERLAY --> exec_vr_overlay -> mirror panel
     HUD     -> canvas -> pc_vr_capture_hud     -- CMD_VR_OVERLAY --> ...            -> dashboard panel
     menu    -> canvas -> pc_vr_capture_menu    -- CMD_VR_OVERLAY --> ...            -> menu panel
   present  ---> gpu::present_call ---> vr::end_frame()  hands the layers
                                        to the compositor, then blocks in
                                        xrWaitFrame: the headset paces the game
```

Not a stereo frame (other camera, a menu...): the normal picture is drawn as usual
and `vr::flat_acquire` gives it a swapchain image too, shown as the flat screen.

### Where things are

| File | Role |
|---|---|
| `src/platform/vr.h`, `vr.cpp` | The OpenXR session: swapchains, frame loop, panels, settings, hotkeys. The only file that talks to OpenXR. |
| `src/platform/vr_view.h` | The small interface the game's own code sees (is a stereo frame ready, each eye's field of view and pose). No Windows headers. |
| `src/game/app/pc_vr.h`, `pc_vr.cpp` | The race view's side: when to go stereo, applying an eye's matrices, handing canvases to the panels. |
| `src/game/race/core/T131_race_ps2_render.cpp` | `RacePS2Base::render` (the two eyes) and `pc_vr_overlay_pass` (the panels' canvases). |
| `src/game/race/core/T132_race_basic.cpp` | `RaceBasic::virtual_45`: the race's 2D split into the dashboard layer and the menu layer. |
| `src/engine/pgl/native/pgl_exec.cpp` | `exec_vr_eye`, `exec_vr_overlay`: draw a recorded picture onto a swapchain image. |
| `src/platform/gpu.cpp` | Makes the device on the runtime's adapter, ends each frame through `vr::end_frame`, paces by the headset. |
| `src/platform/settings_menu.cpp` | The F2 VR category. |
| `cmake/openxr.cmake` | Fetches and builds the OpenXR loader. |
| `tools/vrspike/` | A standalone 32-bit OpenXR + D3D11 test (red and blue eyes), the first proof the approach worked. |

## Things that are easy to get wrong

These cost time to find; they are also commented where they matter.

- **The game is 32-bit.** Its data files are full of pointers, so a 64-bit build
  is not possible. SteamVR registers a 32-bit OpenXR client, so the session runs
  inside the game process. The loader is delay-loaded so the game starts without it.
- **Matrix order.** `pgl_mult_matrix(m)` applies `m` first (to the world);
  `pgl_mult_matrix_90(m)` applies it last (in the camera's frame). The head's
  offset must use the `_90` form.
- **`pc_view_interpolate` (pc_frame.cpp) reads and overwrites the camera's
  matrices.** The eye's matrices go in after it (`pc_vr_apply` in `drawView`),
  or the two eyes contaminate each other.
- **Render targets are found by address and size.** Several sizes share a VRAM
  address (the 640x224 field buffers and the 640x448 race screen), so capture with
  `find_color_target_sized`, not `find_color_target`.
- **The headset paces the game.** `xrWaitFrame` blocks inside `vr::end_frame`; do
  not add a second limiter.
- **Panels are keyed.** The dashboard and the menu panel treat black as clear
  (alpha = brightest channel x 4), so dark boxes in the game's 2D become see-through.
- **Stereo only for the cockpit camera, live, no debug camera.** `pc_vr_stereo`
  logs why it is not stereo whenever that changes (`vr: race view: ...`).

## Not done yet

- Translations of the new menu strings (they show in English).
- Replays and the other cameras in stereo.
- Screen-space effects (lens flares, FXAA, the replay depth of field) are drawn
  per eye as for a flat screen; some may not look right in stereo.
- VR controller input (the wheel and pad work as before).
- Rendering both eyes from one recorded frame (the CPU cost of recording the
  scene twice is the main cost; single-pass stereo would also help the GPU).
