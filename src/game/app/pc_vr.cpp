/* pc_vr.cpp - PC VR: the race view in the headset (pc_vr.h). */
#include "game/app/pc_vr.h"
#include <stddef.h>
#include "game/app/pc_debugcam.h"
#include "game/app/pc_headcam.h"
#include "game/camera/T383_camera_manager.h"
#include "engine/gfx/T514_gs_buffer.h"
#include "engine/pgl/G001_flare.h"              /* gfx_set_picture_vr, gfx_vr_eye, pgl_*_matrix */
#include "platform/vr_view.h"
#include <math.h>
#include <stdio.h>

/* The panels the overlay pass can feed (the numbering of vr.h's kOverlay*, which this file does not include:
 * it is Windows-free). */
enum { OVERLAY_HUD = 0, OVERLAY_MIRROR = 1, OVERLAY_MENU = 2 };

static int s_eye = -1;                          /* the eye being drawn, else -1 */
static int s_menuUp;                            /* a pause menu or a result is up (pc_vr_menu) */
static int s_layer = PC_VR_LAYER_ALL;           /* the layer of the 2D being drawn (pc_vr_layer_set) */
static int s_layerTaken;                        /* ... the 2D split by it this frame */
static f32 s_mirror[6];                         /* the mirror as last drawn: x y w h, the view's size */
static int s_mirrorSeen;                        /* ... drawn since the last capture */

int pc_vr_stereo(CameraManager *cm)
{
    /* (why the race view is or is not stereo, said in the log whenever it changes) */
    static int s_why = -1;
    int why = 0;
    if (!vrview::ready())
        why = 1;
    else if (cm == NULL || cm->pcCockpit() == 0 || !pc_headcam_on())
        why = 2;
    else if (pc_debugcam_on())
        why = 3;
    if (why != s_why) {
        static const char *const names[] = {"stereo", "the headset is not ready (no frame to draw)",
                                            "not the cockpit view", "the debug camera"};
        printf("vr: race view: %s\n", names[why]);
        s_why = why;
    }
    return why == 0;
}

int pc_vr_in_eye(void)
{
    return s_eye >= 0;
}

int pc_vr_hide_hud(void)
{
    return vrview::frame_on() && !vrview::hud_on();
}

void pc_vr_menu(int up)
{
    s_menuUp = up;
}

int pc_vr_menu_up(void)
{
    return s_menuUp;
}

void pc_vr_layer_set(int layer)
{
    s_layer = layer;
    if (layer == PC_VR_LAYER_HUD)
        s_layerTaken = 0;                       /* (a new frame's first layer) */
}

int pc_vr_layer(void)
{
    return s_layer;
}

void pc_vr_layer_taken(void)
{
    s_layerTaken = 1;
}

int pc_vr_layer_was_taken(void)
{
    return s_layerTaken;
}

void pc_vr_log_mirror_state(int switchOn, int liveRace, int cameraHasMirror)
{
    static int s_last = -1;
    const int state = (switchOn != 0) | ((liveRace != 0) << 1) | ((cameraHasMirror != 0) << 2);
    if (state == s_last)
        return;
    s_last = state;
    printf("vr: mirror panel: switch %s, live race %s, camera has a mirror %s%s\n", switchOn ? "on" : "off",
           liveRace ? "yes" : "no", cameraHasMirror ? "yes" : "no",
           (switchOn && liveRace && cameraHasMirror) ? " -> drawn" : " -> not drawn");
}

void pc_vr_note_mirror(f32 x, f32 y, f32 w, f32 h, f32 vw, f32 vh)
{
    s_mirror[0] = x;
    s_mirror[1] = y;
    s_mirror[2] = w;
    s_mirror[3] = h;
    s_mirror[4] = vw;
    s_mirror[5] = vh;
    s_mirrorSeen = 1;
}

void pc_vr_capture_mirror(GSBuffer *buf)
{
    const u32 addr = (u32)buf->addr;
    if (vrview::mirror_on() && s_mirrorSeen && s_mirror[4] > 0.0f && s_mirror[5] > 0.0f && s_mirror[2] > 0.0f &&
        s_mirror[3] > 0.0f)
        gfx_vr_overlay(addr, buf->width, buf->height, OVERLAY_MIRROR, s_mirror[0] / s_mirror[4], s_mirror[1] / s_mirror[5],
                       (s_mirror[0] + s_mirror[2]) / s_mirror[4], (s_mirror[1] + s_mirror[3]) / s_mirror[5]);
    s_mirrorSeen = 0;
}

/* The 2D's canvas to a panel: the HUD, and the race menus, are laid out on a 4:3 band across the whole width,
 * centred vertically (pc_aspect.h).  The canvas is as large as the eye texture, whose shape is not 4:3 (the
 * Index's eyes are taller than wide), so the band is a part of its height: bandHeight (a fraction of 1).
 * The part, as (u0, v0) - (u1, v1) of the canvas, goes to the panel. */
static void capture_band(GSBuffer *canvas, int overlay)
{
    int eyeWidth = 0, eyeHeight = 0;
    vrview::eye_size(&eyeWidth, &eyeHeight);
    const f32 canvasAspect = (eyeWidth > 0 && eyeHeight > 0) ? (f32)eyeWidth / (f32)eyeHeight : 1.0f;
    const f32 menuAspectInverse = 3.0f / 4.0f;      /* the 4:3 screen's height over its width */
    f32 bandHeight = canvasAspect * menuAspectInverse;
    if (bandHeight > 1.0f)
        bandHeight = 1.0f;                          /* (a canvas wider than 4:3 would show the whole of it) */
    gfx_vr_overlay((u32)canvas->addr, canvas->width, canvas->height, overlay, 0.0f, (1.0f - bandHeight) * 0.5f, 1.0f,
                   (1.0f + bandHeight) * 0.5f);
}

void pc_vr_capture_hud(GSBuffer *canvas)
{
    if (vrview::hud_on())
        capture_band(canvas, OVERLAY_HUD);
}

void pc_vr_capture_menu(GSBuffer *canvas)
{
    capture_band(canvas, OVERLAY_MENU);
}

void pc_vr_frame(int on)
{
    vrview::frame(on != 0);
    if (on)
        gfx_set_picture_vr(1);                  /* (recorded with the frame: not undone) */
    else
        s_eye = -1;
}

void pc_vr_eye_begin(int eye)
{
    s_eye = eye;
}

void pc_vr_eye_end(GSBuffer *buf)
{
    if (s_eye < 0)
        return;
    gfx_vr_eye((u32)buf->addr, buf->width, buf->height, s_eye);
    s_eye = -1;
}

/* The elements of a projection matrix that glFrustum sets, as indices into the column-major array (element
 * [column * 4 + row]) the game's matrices are: */
enum ProjectionElement {
    PROJ_X_SCALE = 0,                   /* x' = x * this ...                      (column 0, row 0) */
    PROJ_Y_SCALE = 5,                   /* y' = y * this ...                      (column 1, row 1) */
    PROJ_X_PER_Z = 8,                   /* ... + z * this: the frustum's off-centre shift in x */
    PROJ_Y_PER_Z = 9,                   /* ... + z * this: and in y */
    PROJ_Z_PER_Z = 10,                  /* the depth: z' = z * this + PROJ_Z_OFFSET (the game's own terms) */
    PROJ_W_PER_Z = 11,                  /* the perspective divide: w' = -z (glFrustum: -1) */
    PROJ_Z_OFFSET = 14
};

/* ---- the shake in the headset (pc_vr.h) ---- */
static const f32 kVibrationDegrees = 0.5f;      /* the vibration's largest sum at 100% and the top speed */
static const f32 kVibrationTopSpeed = 83.3f;    /* m/s: 300 km/h, where the drag-like growth reaches its full size */
static const f32 kVibrationAtRest = 0.1f;       /* the share of it at a stop (the engine idling) */
static const f32 kCabinDegrees = 0.8f;          /* the cabin shake's largest at 100% and the top speed */
static const f32 kTwoPi = 6.2831853f;
static f32 s_shakeTime;                         /* seconds, advanced a tick (1/60 s) at a time */
static f32 s_shakeSpeedSq = 1.0f;

void pc_vr_shake_tick(f32 speedSq)
{
    s_shakeTime += 1.0f / 60.0f;
    if (s_shakeTime > 3600.0f)
        s_shakeTime -= 3600.0f;                 /* (keeps the sines' arguments small: a float's precision) */
    s_shakeSpeedSq = speedSq;
}

f32 pc_vr_shake_cap(f32 degrees)
{
    return degrees < -3.0f ? -3.0f : degrees > 3.0f ? 3.0f : degrees;
}

/* two sines of different frequencies (Hz), the second a third the size: a wobble that does not repeat at once */
static f32 wobble(f32 hz1, f32 hz2, f32 phase)
{
    return sinf(kTwoPi * hz1 * s_shakeTime + phase) + 0.33f * sinf(kTwoPi * hz2 * s_shakeTime + phase * 2.3f);
}

/* the shake's size from the speed: from a little at a stop to full at the top speed, as the square of it
 * (drag), under a slow swell */
static f32 shake_envelope(void)
{
    f32 ratio = sqrtf(s_shakeSpeedSq > 0.0f ? s_shakeSpeedSq : 0.0f) / kVibrationTopSpeed;
    if (ratio > 1.0f)
        ratio = 1.0f;
    const f32 growth = kVibrationAtRest + (1.0f - kVibrationAtRest) * ratio * ratio;
    const f32 swell = 0.65f + 0.35f * sinf(kTwoPi * 0.37f * s_shakeTime);
    return growth * swell / 1.33f;
}

int pc_vr_cabin_matrix(f32 m[16])
{
    f32 gain, vibration, cabin = 0.0f;
    vrview::shake(&gain, &vibration, &cabin);
    if (cabin <= 0.0f)
        return 0;
    /* the car's body on its springs: more roll and pitch than yaw, at its own frequencies (not the view's:
     * the two kinds mixed do not beat together) */
    const f32 amplitude = kCabinDegrees * cabin * shake_envelope();
    const f32 deg2rad = 3.1415927f / 180.0f;
    const f32 pitch = pc_vr_shake_cap(amplitude * wobble(3.1f, 8.3f, 1.3f)) * deg2rad;
    const f32 yaw = pc_vr_shake_cap(amplitude * 0.4f * wobble(4.7f, 9.1f, 3.4f)) * deg2rad;
    const f32 roll = pc_vr_shake_cap(amplitude * wobble(2.3f, 6.7f, 5.6f)) * deg2rad;
    /* R = Rz(roll) Ry(yaw) Rx(pitch), column-major (the game's matrices' layout: m[column * 4 + row]) */
    const f32 cx = cosf(pitch), sx = sinf(pitch), cy = cosf(yaw), sy = sinf(yaw), cz = cosf(roll), sz = sinf(roll);
    m[0] = cz * cy;
    m[1] = sz * cy;
    m[2] = -sy;
    m[3] = 0.0f;
    m[4] = cz * sy * sx - sz * cx;
    m[5] = sz * sy * sx + cz * cx;
    m[6] = cy * sx;
    m[7] = 0.0f;
    m[8] = cz * sy * cx + sz * sx;
    m[9] = sz * sy * cx - cz * sx;
    m[10] = cy * cx;
    m[11] = 0.0f;
    m[12] = m[13] = m[14] = 0.0f;
    m[15] = 1.0f;
    return 1;
}

void pc_vr_shake_get(f32 *gain, f32 *pitch, f32 *yaw, f32 *roll)
{
    f32 vibration = 0.0f, cabin = 0.0f;
    vrview::shake(gain, &vibration, &cabin);
    *pitch = *yaw = *roll = 0.0f;
    if (vibration <= 0.0f)
        return;
    const f32 amplitude = kVibrationDegrees * vibration * shake_envelope();
    *pitch = amplitude * wobble(7.3f, 17.9f, 0.7f);
    *yaw = amplitude * 0.5f * wobble(9.7f, 21.1f, 2.1f);
    *roll = amplitude * 0.8f * wobble(11.3f, 19.7f, 4.2f);
}

void pc_vr_apply(void)
{
    if (s_eye < 0)
        return;
    f32 tan[4], inv[16];                /* the eye's field of view as tangents, and its place (a view matrix) */
    if (!vrview::eye(s_eye, tan, inv))
        return;
    f32 m[16];                          /* the game's projection, as the camera set it */
    pgl_matrix_mode(PGL_MTX_PROJECTION);
    pgl_get_matrix(m);
    /* Replace its x / y terms with the headset's field of view, which is not symmetrical (an eye sits off
     * the middle of its lens), as glFrustum builds them from the left / right / bottom / top tangents.  The depth
     * terms stay the game's own: the renderer's emulation of the PS2's depth buffer reads them. */
    f32 p[16];
    for (int i = 0; i < 16; i++)
        p[i] = 0.0f;
    const f32 tanLeft = tan[0], tanRight = tan[1], tanDown = tan[2], tanUp = tan[3];
    p[PROJ_X_SCALE] = 2.0f / (tanRight - tanLeft);
    p[PROJ_Y_SCALE] = 2.0f / (tanUp - tanDown);
    p[PROJ_X_PER_Z] = (tanRight + tanLeft) / (tanRight - tanLeft);
    p[PROJ_Y_PER_Z] = (tanUp + tanDown) / (tanUp - tanDown);
    p[PROJ_Z_PER_Z] = m[PROJ_Z_PER_Z];
    p[PROJ_W_PER_Z] = m[PROJ_W_PER_Z];
    p[PROJ_Z_OFFSET] = m[PROJ_Z_OFFSET];
    pgl_load_matrix(p);
    {
        /* (a few lines in the log: the game's own projection terms against the headset's, and where the eye is) */
        static int logged;
        if (logged < 8 && (s_eye == 0 || logged < 2)) {
            logged++;
            f32 v[16];
            pgl_matrix_mode(PGL_MTX_MODELVIEW);
            pgl_get_matrix(v);
            printf("vr: eye %d: game projection [0]=%.3f [5]=%.3f [8]=%.3f [9]=%.3f [10]=%.4f [11]=%.1f [14]=%.4f; "
                   "headset tan L%.2f R%.2f D%.2f U%.2f; head offset (%.3f %.3f %.3f) m; view translation (%.2f %.2f %.2f)\n",
                   s_eye, m[0], m[5], m[8], m[9], m[10], m[11], m[14], tan[0], tan[1], tan[2], tan[3], inv[12], inv[13],
                   inv[14], v[12], v[13], v[14]);
        }
    }
    /* the head's place: after the camera's view (pgl_mult_matrix applies its matrix first,
     * to the world; the _90 form last, in the camera's frame) */
    pgl_matrix_mode(PGL_MTX_MODELVIEW);
    pgl_mult_matrix_90(inv);
}
