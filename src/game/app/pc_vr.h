/* pc_vr.h - PC VR: the race view in the headset (platform/vr.h, vr_view.h).
 * No original: new PC code, a fork's feature.
 *
 * The cockpit camera's view (pc_headcam.h) is drawn twice per frame, once for
 * each eye (RacePS2Base::render, T131): between pc_vr_eye_begin and
 * pc_vr_eye_end the view's projection is the eye's own frustum - the headset's
 * field of view, not symmetrical - with the game's depth terms kept (the GS
 * depth emulation reads them), and its view is the camera's turned and moved
 * by where the head is from where it was centred (pc_vr_view): the head
 * looks round the cockpit, and leans in it.  The car's frame is the head's
 * (x right, y up, -z forward, metres).  pc_vr_eye_end hands the eye's picture
 * to the renderer (gfx_vr_eye) for the headset's swapchain.
 *
 * The HUD, the mirrors and the screen-space effects come after the two eyes
 * (docs: the VR plan, Phase 3). */
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
/* RaceBasic::virtual_45, every frame: a pause menu or a result is up - the race is not shown in stereo meanwhile
 * (its flat picture, on the flat screen, is where they can be read). */
void pc_vr_menu(int up);
/* drawMirror: the rectangle it drew (pixels of the view, which is vw x vh), for the mirror's panel. */
void pc_vr_note_mirror(f32 x, f32 y, f32 w, f32 h, f32 vw, f32 vh);
/* The dashboard drawn into `canvas' / the mirror drawn into it (each on a canvas of its own, one after the
 * other: the dashboard's crop is a band that holds the mirror's place): its part to its panel. */
void pc_vr_capture_hud(GSBuffer *canvas);
void pc_vr_capture_mirror(GSBuffer *canvas);

/* In drawView, once the camera's projection and view are set and blended
 * (pc_view_interpolate keeps and blends the matrices it reads: the eye's must
 * come after): the projection takes the eye's frustum, the view the head's
 * place.  Leaves the modelview matrix current.  Nothing outside an eye. */
void pc_vr_apply(void);

#endif /* GAME_APP_PC_VR_H */
