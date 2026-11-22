/* pc_vr.cpp - PC VR: the race view in the headset (pc_vr.h). */
#include "game/app/pc_vr.h"
#include <stddef.h>
#include "game/app/pc_debugcam.h"
#include "game/app/pc_headcam.h"
#include "game/camera/T383_camera_manager.h"
#include "engine/gfx/T514_gs_buffer.h"
#include "engine/pgl/G001_flare.h"              /* gfx_set_picture_vr, gfx_vr_eye, pgl_*_matrix */
#include "platform/vr_view.h"
#include <stdio.h>

static int s_eye = -1;                          /* the eye being drawn, else -1 */
static int s_menuUp;                            /* a pause menu or a result is up (pc_vr_menu) */
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
    else if (s_menuUp)
        why = 4;
    if (why != s_why) {
        static const char *const names[] = {"stereo", "the headset is not ready (no frame to draw)",
                                            "not the cockpit view", "the debug camera", "a pause menu or the result is up"};
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
        gfx_vr_overlay(addr, buf->width, buf->height, 1, s_mirror[0] / s_mirror[4], s_mirror[1] / s_mirror[5],
                       (s_mirror[0] + s_mirror[2]) / s_mirror[4], (s_mirror[1] + s_mirror[3]) / s_mirror[5]);
    s_mirrorSeen = 0;
}

void pc_vr_capture_hud(GSBuffer *buf)
{
    const u32 addr = (u32)buf->addr;
    if (vrview::hud_on()) {
        /* the dashboard's canvas: a 4:3 band across the whole width, centred vertically (pc_aspect.h) */
        int ew = 0, eh = 0;
        vrview::eye_size(&ew, &eh);
        const f32 a = (ew > 0 && eh > 0) ? (f32)ew / (f32)eh : 1.0f;
        f32 fh = a * 0.75f;
        if (fh > 1.0f)
            fh = 1.0f;
        gfx_vr_overlay(addr, buf->width, buf->height, 0, 0.0f, (1.0f - fh) * 0.5f, 1.0f, (1.0f + fh) * 0.5f);
    }
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

void pc_vr_apply(void)
{
    if (s_eye < 0)
        return;
    f32 tan[4], inv[16];
    if (!vrview::eye(s_eye, tan, inv))
        return;
    f32 m[16];
    pgl_matrix_mode(PGL_MTX_PROJECTION);
    pgl_get_matrix(m);
    /* glFrustum's x / y terms from the headset's field of view, the game's own z terms */
    f32 p[16];
    for (int i = 0; i < 16; i++)
        p[i] = 0.0f;
    const f32 tl = tan[0], tr = tan[1], tb = tan[2], tt = tan[3];
    p[0] = 2.0f / (tr - tl);
    p[5] = 2.0f / (tt - tb);
    p[8] = (tr + tl) / (tr - tl);
    p[9] = (tt + tb) / (tt - tb);
    p[10] = m[10];
    p[11] = m[11];
    p[14] = m[14];
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
