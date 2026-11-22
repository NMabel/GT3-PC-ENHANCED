/* pc_vr.h - PC VR: the race view in the headset (platform/vr.h, vr_view.h).
 * No original: new PC code, a fork's feature.
 *
 * The cockpit camera's view (pc_headcam.h) is drawn twice per frame, once for
 * each eye (RacePS2Base::render, T131): between pc_vr_eye_begin and
 * pc_vr_eye_end the view's projection is the eye's own frustum - the headset's
 * field of view, not symmetrical - with the game's depth terms kept (the GS
 * depth emulation reads them), and its view is the camera's turned and moved
 * by where the head is from where it was centred (pc_vr_apply): the head
 * looks round the cockpit, and leans in it.  The car's frame is the head's
 * (x right, y up, -z forward, metres).  pc_vr_eye_end hands the eye's picture
 * to the renderer (gfx_vr_eye) for the headset's swapchain.
 *
 * What is not the 3D view comes after the two eyes, in the "overlay pass"
 * (pc_vr_overlay_pass in T131): the rear-view mirror, the HUD (the
 * dashboard) and the menu layer (pause menu, race result) are each drawn on a
 * clean canvas and handed to the renderer (gfx_vr_overlay) for a panel of
 * their own.  docs/vr.md follows a frame through all of it. */
#ifndef GAME_APP_PC_VR_H
#define GAME_APP_PC_VR_H

#include "game/game.h"

class CameraManager;
class GSBuffer;

/* This race view is drawn in stereo: the headset is ready and the view is the
 * cockpit camera's. */
int pc_vr_stereo(CameraManager *cm);
/* The race frame is a stereo one from here (the picture is sized to the
 * eyes); off again at the end of the frame's views. */
void pc_vr_frame(int on);
/* One eye's view of the stereo frame. */
void pc_vr_eye_begin(int eye);
void pc_vr_eye_end(GSBuffer *buf);
/* A race view of the stereo frame is being drawn (the eyes): the mirror is not drawn into them - it is a panel
 * of its own (the overlay pass). */
int pc_vr_in_eye(void);
/* The dashboard is hidden in this stereo frame (the race display is not drawn into the overlay). */
int pc_vr_hide_hud(void);
/* RaceBasic::virtual_45, every frame: a pause menu is up, or the player's car has finished and the result is
 * on.  The race stays in stereo meanwhile (the world goes on behind, or stands still, in the headset): what
 * the 2D draws for them is the menu layer, a panel of its own in front of the player. */
void pc_vr_menu(int up);
int pc_vr_menu_up(void);
/* The race's 2D (RaceBasic::virtual_45) in a stereo frame is drawn in two layers, each on a canvas of its own:
 * the HUD (the dashboard: the race display), and the menu (the pause menu, the result's board).  The overlay pass
 * sets the layer it draws; PC_VR_LAYER_ALL (-1) everywhere else, where everything is drawn at once.  A 2D that
 * splits by layer says so (pc_vr_layer_taken): the pass draws the menu layer only for one that does. */
enum { PC_VR_LAYER_ALL = -1, PC_VR_LAYER_HUD = 0, PC_VR_LAYER_MENU = 1 };
void pc_vr_layer_set(int layer);
int pc_vr_layer(void);
void pc_vr_layer_taken(void);
int pc_vr_layer_was_taken(void);
/* drawMirror: the rectangle it drew (pixels of the view, which is vw x vh), for the mirror's panel. */
void pc_vr_note_mirror(f32 x, f32 y, f32 w, f32 h, f32 vw, f32 vh);
/* The dashboard drawn into `canvas' / the mirror drawn into it / the menu layer (each on a canvas of its own,
 * one after the other: the dashboard's crop is a band that holds the mirror's place): its part to its panel. */
void pc_vr_capture_hud(GSBuffer *canvas);
void pc_vr_capture_mirror(GSBuffer *canvas);
void pc_vr_capture_menu(GSBuffer *canvas);

/* In drawView, once the camera's projection and view are set and blended
 * (pc_view_interpolate keeps and blends the matrices it reads: the eye's must
 * come after): the projection takes the eye's frustum, the view the head's
 * place.  Leaves the modelview matrix current.  Nothing outside an eye. */
void pc_vr_apply(void);

#endif /* GAME_APP_PC_VR_H */
