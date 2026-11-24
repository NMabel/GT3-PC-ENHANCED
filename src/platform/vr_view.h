/* vr_view.h - PC VR: what the game's own code (the race view) asks of the
 * headset session; no Windows headers (the game code includes it; vr.cpp
 * implements it).  See platform/vr.h. */
#pragma once

namespace vrview {

/* A stereo frame can be drawn now: a session is running, its frame is open
 * and the head has been located for it. */
bool ready();

/* The race frame being recorded is a stereo one (set before its first race
 * view, cleared after its last): the picture area answers the eye texture's
 * size (pacing::output_size) while it is. */
void frame(bool on);
bool frame_on();

/* The dashboard / the mirror are shown (settings; the stereo race draws their
 * overlay pass only for what is on). */
bool hud_on();
bool mirror_on();

/* The cockpit's shake in the headset ([VR] Shake, HeadShake, CabinShake): gain, the factor for the game's
 * own shake of the view (1: as it is); vibration, the factor for the road / engine vibration added to the
 * view's (0: none); cabin, the factor for the car's body shaking over the road (0: none).  Without a headset
 * view (flat) they are 1, 0 and 0: the flat screen's shake is untouched. */
void shake(float *gain, float *vibration, float *cabin);

/* The eye textures' size (pixels). */
void eye_size(int *width, int *height);

/* Eye `eye' (0 left, 1 right) of this frame:
 *   tan[4]  the half-angles of its field of view as tangents, left, right,
 *           down, up (left and down negative: the frustum is not
 *           symmetrical);
 *   viewInv the column-major 4x4 (OpenGL: the game's matrices) that takes the
 *           seat's frame to the eye's - the inverse of the eye's pose in the
 *           seat's frame - to be multiplied before the camera's view (the
 *           car's frame: x right, y up, -z forward, metres; the head's
 *           movements and the eyes' distance divided by the world's scale).  The seat is
 *           where the head was when the view was last centred (Home), looking
 *           along the car's forward.
 * false: no pose this frame. */
bool eye(int eye, float tan[4], float viewInv[16]);

} // namespace vrview
