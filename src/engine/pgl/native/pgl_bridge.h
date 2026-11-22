/* pgl_bridge.h - the interface between pgl (the graphics library's C, in
 * src/engine/pgl/*.cpp, compiled as decompiled code) and the native
 * renderer (src/engine/pgl/native/pgl_render*.cpp, the gt3_render library,
 * Direct3D 11).
 *
 * pgl keeps its state model: GsContext and its GS register shadows, the
 * packet buffer as plain memory, the VU0 matrices in pgl::g_vu0.  Where the
 * PS2 library wrote GS / VIF / DMA packets, the port's pgl records what the
 * packet meant through these calls; the renderer keeps the records in a
 * command list and executes the list when the frame is kicked
 * (gfx_kick_frame_lists), as the DMA did, and presents the displayed frame
 * buffer at gfx_set_display_fb.  Pointers handed over here are read at
 * execution time, as the DMA read them.
 *
 * Plain C types only: the decompiled side compiles with the port's prelude
 * (its macros rename C library names), the native side without it.
 * docs/renderer.md describes both sides. */
#ifndef PGL_NATIVE_PGL_BRIDGE_H
#define PGL_NATIVE_PGL_BRIDGE_H

#include <stdint.h>

#include "engine/pgl/native/pgl_state_prim.h"     /* pgl::prim::GsVertex */

extern "C" {

/* ======================================================================== */
/* Driver                                                                   */
/* ======================================================================== */

/* gfx_init: the renderer's objects (the D3D11 device is the platform's). */
void pglr_init(void);

/* 64-bit counters shared with the vblank handler (it runs on the timer
 * thread; 64-bit loads and stores are not atomic on x86). */
uint64_t pglr_load64(const volatile uint64_t *p);
void     pglr_store64(volatile uint64_t *p, uint64_t v);
void     pglr_add64(volatile uint64_t *p, uint64_t v);

/* A CPU cycle counter (gfx_perf_counter0: the cameras seed random numbers
 * with it). */
uint64_t pglr_cycles(void);

/* ======================================================================== */
/* Frame                                                                    */
/* ======================================================================== */

/* gfx_begin_packet: a new frame command list (what was recorded outside a
 * packet is dropped, as the PS2 never sent it). */
void pglr_begin_frame(void);
/* gfx_end_packet: the list is complete. */
void pglr_end_frame(void);
/* PC (gfx_set_picture_aspect): the frame being recorded is a race frame
 * whose picture fills the window at `aspect' (width / height); 0, the
 * default of every frame, the 4:3 picture (docs/renderer.md "Aspect"). */
void pglr_set_picture_aspect(float aspect);
/* PC (gfx_set_picture_multi): that race frame's picture is drawn over the
 * screens' canvas (platform/gpu.h "Screens"), each screen showing its part;
 * 0, the default, the game window alone. */
void pglr_set_picture_multi(int multi);
/* PC (gfx_set_menu_aspect; kept until changed, not reset per frame): a
 * frame that sets no picture aspect of its own (a 4:3 menu) is shown at
 * `aspect' - the window's, the 640 x 448 picture stretched over it (the
 * menus' stretch, [Display] MenuAspect); 0: at 4:3 with bars. */
void pglr_set_menu_aspect(float aspect);
/* gfx_kick_frame_lists: executes the completed list. */
void pglr_kick(void);
/* gfx_sync_pipeline / gfx_dma_sync_frame: execution is synchronous at the
 * kick; nothing to wait for. */
void pglr_sync(void);

/* gfx_set_display_fb: presents the frame buffer at fbBlock (64-word blocks)
 * of format psm, buffer width fbw (pixels), showing the display rectangle
 * (dx, dy, dw, dh) of it (gfx_set_display_area), at 4:3 in the window. */
void pglr_display(uint32_t fbBlock, uint32_t psm, uint32_t fbw,
                  int dx, int dy, int dw, int dh);
/* gfx_set_display_enable: 0 shows black, 1 the last displayed buffer. */
void pglr_display_enable(int on);
/* PC: what is shown presented again, its overlays redrawn (the tick step's
 * pause: the game draws nothing, platform/frame_step.h). */
void pglr_present_again(void);
/* PC: GPU time by section (GT3PC_GPU_SECTIONS=N: the means logged every N
 * frames): the draws recorded after a mark count to its section until the
 * next mark; a frame starts in PGLR_SEC_OTHER. */
enum {
    PGLR_SEC_OTHER,         /* (the rest: menus, the HUD, the 2D) */
    PGLR_SEC_SCENE,         /* a race view's 3D: course, cars, mirror */
    PGLR_SEC_EFFECTS,       /* its lens flare's queries, particles, FXAA */
    PGLR_SEC_COPY,          /* the view into its target (blitField) */
    PGLR_SEC_REPLAY_BLUR,   /* the replays' depth of field (instead of the copy) */
    PGLR_SEC_WEATHER,       /* the heat-haze / rain drops */
    PGLR_SEC_SMOKE,         /* the tyre smoke */
    PGLR_SEC_FLARE,         /* the lens flare's glares */
    PGLR_SEC_POST,          /* colour correction, afterimage, frame effect */
    /* PC (2026-11-03): the renderer's own, within the scene: the cars (the
     * VU1 draws textured from a Tex1 set; the rest of the scene is the
     * course) and the copies of a target for the draws that read it
     * (DATE, C = Ad, a texture of it) */
    PGLR_SEC_CARS,
    PGLR_SEC_TARGET_COPY,
    /* PC (2026-11-05): within the effects - the lens flare's occlusion
     * queries (exec_flare_query) and FXAA (exec_post_aa) apart, so that the
     * section holds the particles */
    PGLR_SEC_FLARE_QUERY,
    PGLR_SEC_POST_AA,
    PGLR_SEC_COUNT
};
void pglr_gpu_section(int id);

/* ======================================================================== */
/* GS registers                                                             */
/* ======================================================================== */

/* gs_flush_dirty_regs / pgl_send_dirty_regs: the shadows pgl emits (mask:
 * GsContext.dirty bits - 0..12 context 1, 16..28 context 2, 32..41 the
 * general registers; reg / gen the shadow arrays). */
void pglr_regs(uint64_t mask, const uint64_t *reg, const uint64_t *gen);
/* One A+D register write (GS register address SCE_GS_*). */
void pglr_ad(uint32_t gsReg, uint64_t value);

/* The size of a render target pgl draws into (gfx_set_draw_buffer: target
 * A; gfx_set_target_b: target B), by its base block. */
void pglr_target(uint32_t baseBlock, uint32_t fbw, uint32_t psm, int width, int height);
/* PC (2026-11-14): the width x height buffer at baseBlock drawn at dx x dy
 * times the output scale (finer render targets for it) from the next draw
 * into it on; 1, 1: back to the output scale.  Its GS size and the game's
 * draws are unchanged: only the native pixels per GS pixel (the road
 * reflection, [Graphics] ReflectionResolution: T131 drawReflections). */
void pglr_target_detail(uint32_t baseBlock, int width, int height, float dx, float dy);
/* PC (2026-11-13): the game's VRAM allocator (T513) took a buffer's block
 * back (VramAllocator::dealloc) / handed a block out as a buffer
 * (VramAllocator::alloc).  A block freed and not handed out again by the
 * frame's end goes to the executor with the frame (a CMD_TARGET of size -1
 * at the list's end): the render targets at that block are dead, and go
 * after a few frames instead of 600 (pgl_targets.cpp target_freed). */
void pglr_buffer_freed(uint32_t baseBlock);
void pglr_buffer_alloc(uint32_t baseBlock);
/* PC (2026-11-13): the render targets alive, and since the start: the
 * surfaces created, those taken from retired targets instead, the targets
 * retired (the benchmark's check; any may be NULL). */
void pglr_target_stats(int *alive, unsigned *created, unsigned *reused, unsigned *retired);

/* ======================================================================== */
/* Primitives                                                               */
/* ======================================================================== */

/* Flags of the draw calls. */
enum {
    PGLR_HALF_HEIGHT = 0x01         /* drawn with GsContext.halfHeight (y in 12.3) */
};

/* A primitive as the GS received it: PRIM and n vertices (pgl's 2D paths,
 * pgl_state_prim.h). */
void pglr_prim(uint32_t prim, const pgl::prim::GsVertex *v, int n, uint32_t flags);

/* A block of GIF data the game wrote into the packet buffer itself
 * (gfx_open_direct, gfx_alloc_direct, the text renderer): qwc quadwords of
 * GIF tags and data at data, decoded at execution. */
void pglr_gif(const void *data, uint32_t qwc, uint32_t flags);

/* The state a VU1 program ran with: the combined matrix vf24..vf27 (rows)
 * and the viewport vectors pgl_send_matrix sends, the program (vu1Prog:
 * 0x320 / 0x33E, + 0xA / + 0x14 culling; 0 draws nothing).
 * PC: projZ - the projection's clip z = A w + B as the matrix was sent (A,
 * B, 1; 0 0 0: not of that form): the draws' depth from w (docs/renderer.md
 * 4, "Precision"). */
typedef struct PglrVu1 {
    float    m[16];
    float    viewScale[4];
    float    viewCenter[4];
    float    limits[4];
    float    vBB0[4];
    uint32_t prog;
    float    projZ[3];
} PglrVu1;

/* A strip run of the VU1 primitive program (pgl_draw_strip, strip_batch_*,
 * strip_draw): n vertices of xyz (3 floats), optional st (2 floats) and
 * rgba (bytes, 0x80 = 1.0 for textured parts); without rgba every vertex
 * has constRgba (PC: 0xFF = 1.0 under PRIM_FULL_COLOR, docs/renderer.md "Full
 * colours").  The strip lengths: nStrips entries of lengths (NULL: one strip
 * of n).  prim: the GIF tag's PRIM (TRISTRIP | attributes). */
void pglr_strip(const PglrVu1 *vu, uint32_t prim, const float *xyz, const float *st,
                const uint32_t *rgba, uint32_t constRgba, int n,
                const uint8_t *lengths, int nStrips, uint32_t flags);

/* tri_draw16 (memory card icons): n triangles of 16-bit xyzw (integers),
 * st (/ 4096) and rgba. */
void pglr_tri16(const PglrVu1 *vu, uint32_t prim, const int16_t *xyzw, const int16_t *st,
                const uint32_t *rgba, int nTris, uint32_t flags);

/* One part of a prebuilt VU1 model (model_draw, vif_model_draw): the
 * VifModel and the part index; lightProg the lighting program the EE chose
 * (GsContext.vu1LightProg; 0x4E unlit, 0x2F7 the flare program), lightOfs
 * its vertex-format offset; light[] the lighting block of the mode
 * (lightQwords quadwords, VU1 0xC9...); colorScale / alphaScale;
 * morphWeight; primMask / primAttr: the PRIM override of the library state
 * (prim = (part prim & ~primMask) | (primAttr & primMask)); preXyz:
 * positions vu1_unpack_xyz put in front of the part (NULL: none). */
typedef struct PglrModelPart {
    const void *model;
    int         part;
    uint32_t    lightProg;
    int         lightOfs;
    float       light[16][4];
    int         lightQwords;
    float       colorScale;
    float       alphaScale;
    float       morphWeight;
    uint32_t    primMask;
    uint32_t    primAttr;
    const float *preXyz;
    int         preXyzCount;
    /* the flare program (lightProg 0x2F7): the group of 4 occlusion result
     * texels (read at execution) and the index of the one to use */
    const uint32_t *flareResults;
    uint32_t    flareIndex;
    /* PC: a result disc's texel read on the GPU instead (flareStripH > 0):
     * texel flareQ of the 64-wide result strip at flareStripBlock, the one
     * the frame's queries wrote (flare_readback no longer waits for it) */
    uint32_t    flareStripBlock;
    int32_t     flareStripH;
    int32_t     flareQ;
    /* PC (2026-11-07): the hide box (vifmodel_pc_hide, G001_flare.h: the
     * cockpit camera's driver's head, the eye in it): hideOn 1, a box in the
     * model's space, centre hideC and half sizes hideH, whose inside the
     * pixel shader discards (vs_vu1's hide, ps_body); 2, the part's
     * translucent texels (a glass shell facing the eye: its tint, not the
     * pillars drawn with it); 0: nothing hidden.
     * Part of the constants' head (CbVu1), so draws join only with the same
     * box, and of the command's key (cmd_model_key). */
    float       hideC[3];
    float       hideOn;
    float       hideH[3];
    float       hidePad;
} PglrModelPart;
void pglr_model_part(const PglrVu1 *vu, const PglrModelPart *part, uint32_t flags);
/* A VifModel was written at `model' (gfx_model_written: TStripBuilder::
 * finish, rebuilt there): its mesh is decoded again at its next draw.  Any
 * thread. */
void pglr_model_written(const void *model);
/* PC: a VifModel was loaded at `model' (vif_model_relocate): as
 * pglr_model_written, and its mesh is decoded ahead on a worker (from a copy
 * taken now).  The game thread. */
void pglr_model_loaded(const void *model);
/* PC: a ModelSet's n packets (VifModels, its draw list's order) were loaded
 * (modelset_relocate): the overlays among them are found - a packet whose
 * triangles lie on an earlier packet's (a lit lamp's glow on its lens),
 * drawn a hair nearer (docs/renderer.md "Overlays").  The game thread. */
void pglr_modelset_loaded(const void *const *packets, int n);

/* ======================================================================== */
/* VRAM transfers                                                           */
/* ======================================================================== */

/* gs_upload_image (queued: immediate 0) / gs_upload_image_sync (immediate
 * 1): w x h pixels of psm at data into the buffer (dbp blocks, dbw 64-pixel
 * units) at (dx, dy). */
void pglr_upload(uint32_t dbp, uint32_t dbw, uint32_t dpsm, int dx, int dy, int w, int h,
                 const void *data, int immediate);
/* tex1_upload_packet (immediate 0) / tex1_upload_to_vram (immediate 1): the
 * uploads of a relocated 'Tex1' set (Tex1Image). */
void pglr_tex1_upload(const void *tex1Image, int immediate);
/* tex1_relocate: a set got (re)loaded at img (the registry's identity
 * stamp). */
void pglr_tex1_loaded(void *tex1Image);
/* PC (ROADMAP 18, no memory limits): the texture just selected (tex0: TEX0
 * as it goes out) is one of the Tex1 set's at tex1Image, which found no
 * VRAM: not linked or uploaded, it is linked for vramBase (blocks) - the
 * draws with that TEX0 sample the set itself, and the CLUT loads of its
 * CBP read the set.  NULL: the textures that follow are VRAM's again. */
void pglr_texture_source(const void *tex1Image, uint32_t vramBase, uint64_t tex0);

/* PC (vector fonts, pgl_vfont.h; docs/renderer.md "Host images"): a picture
 * the game makes on the CPU at native resolution, standing for a GS texture
 * no VRAM holds.  The draws whose TEX0 is the image's (pglr_host_image_add:
 * PSMCT32, TBW 0 - a buffer width no texture of the game has - the image's
 * number in TBP0, TCC 1, TFX MODULATE) sample it, bilinear: its gsW x gsH
 * texels are its w x h pixels (RGBA bytes, GS units: alpha 0x80 = 1.0).  The
 * game changes it between pglr_host_image_lock and _unlock: a new size or
 * generation makes all of it new, else rows [dirtyY0, dirtyY1) changed; the
 * executor takes what changed under the lock when a draw binds it.  Images
 * are never released. */
typedef struct PglrHostImage {
    int32_t   id;               /* (pglr_host_image_add) */
    int32_t   gsW, gsH;         /* the GS texels it holds, from (0, 0) of the TEX0's space */
    int32_t   w, h;             /* its pixels */
    uint32_t *pixels;           /* w x h */
    uint32_t  generation;       /* bumped when all of it is new */
    int32_t   dirtyY0, dirtyY1; /* rows changed since the executor took them (none: y1 <= y0) */
} PglrHostImage;
/* Registers img (the game thread; it stays registered): its TEX0, of a
 * 2^tw x 2^th texel space; 0 when no more images fit (4096). */
uint64_t pglr_host_image_add(PglrHostImage *img, uint32_t tw, uint32_t th);
void pglr_host_image_lock(PglrHostImage *img);
void pglr_host_image_unlock(PglrHostImage *img);
/* The native pixels per GS pixel of the frames' height (the output scale:
 * the window, the render scale; any thread). */
float pglr_text_scale(void);
/* ... of their width (the vector fonts' digits: whole output pixels). */
float pglr_text_scale_x(void);

/* PC (docs/movies.md "MP4 movies", pgl_movie.cpp): a movie decoded by the GPU.
 * A draw whose TEX0 has TBP0 PGLR_MOVIE_TBP, TBW 0 and PSMCT32 samples the
 * picture shown, its whole w x h over the 2^PGLR_MOVIE_TEXEL_LOG2 square of
 * the TEX0's space.  attach: the movie's shared textures (keyed mutex, key
 * 0), count 0 at its end.  stamp: the picture a slot now holds (0: being
 * written); busy: a copy from the slot is queued.  show: the picture to draw
 * from now on, by its number (any thread). */
enum { PGLR_MOVIE_SLOTS = 10, PGLR_MOVIE_TBP = 0x3FFF, PGLR_MOVIE_TEXEL_LOG2 = 10 };
void pglr_movie_attach(void *const *handles, int count, int w, int h);
void pglr_movie_stamp(int slot, uint32_t seq);
int  pglr_movie_busy(int slot);
void pglr_movie_show(int slot, uint32_t seq);

/* gs_download_image: w x h pixels at (sx, sy) of the buffer (sbp blocks,
 * sbw) read back as psm into dst, then callback(madr, arg) if set. */
typedef void (*PglrReadbackFunc)(uint32_t madr, uint32_t arg);
void pglr_readback(uint32_t sbp, uint32_t sbw, uint32_t spsm, int sx, int sy, int w, int h,
                   void *dst, PglrReadbackFunc callback, uint32_t madr, uint32_t arg);
/* PC (flare_readback): the next readback is the flare result strip's.  The
 * glows read their texels on the GPU (PglrModelPart.flareQ), so it is done
 * only for GT3PC_FLARE_LOG; otherwise skipped, the CPU never waits for the
 * GPU there (docs/renderer.md 8). */
void pglr_readback_flare(void);

/* ======================================================================== */
/* Sub-packets (gfx_begin_subpacket / _close / _call)                       */
/* ======================================================================== */

void pglr_sub_begin(const void *header);
void pglr_sub_end(void);
void pglr_sub_call(const void *header);

/* ======================================================================== */
/* Effects with their own recipes (graphics.md 8)                           */
/* ======================================================================== */

/* pfx_color_correction (graphics.md 8.5): the frame at srcBlock (the TEX0
 * texture) through the tone-curve CLUT (curve: 256 entries of R, G, B
 * bytes, by index) and the saturation matrix (colR / colG / colB: the
 * matrix columns of the R, G and B inputs as RGBA words, 0x80 = 1.0) into
 * the buffer at dstBlock (FRAME, fbw 64-pixel units), w x h; RGB written,
 * alpha kept.  The curve is copied at the call. */
void pglr_pfx_color_correction(uint32_t srcBlock, uint32_t dstBlock, uint32_t fbw, int w, int h,
                               const uint8_t *curve, uint32_t colR, uint32_t colG, uint32_t colB);
/* pfx_split_rgb_planes (graphics.md 8.6): the R, G, B bytes of the frame at
 * srcBlock (the TEX0 texture) into the alpha bytes of the w x h planes at
 * blockR / blockG / blockB. */
void pglr_pfx_split_rgb(uint32_t srcBlock, uint32_t blockR, uint32_t blockG, uint32_t blockB,
                        int w, int h);
/* A lens-flare occlusion query (flare_occlusion_query, docs/renderer.md 8):
 * the share of the screen's box (x0, y0, w, h; GS pixels) where the shape
 * (nRects rectangles x0 y0 x1 y1, exclusive) passes the depth test at Z
 * against the screen's Z buffer, stored as the GS stored it - Cs x A >> 7,
 * alpha A - into texel q of the 64-wide result strip at resultBlock. */
struct PglrFlareQuery {
    uint32_t frameBlock, fbw, fpsm;     /* the screen (FRAME) */
    int32_t  frameW, frameH;
    uint32_t zBlock, zpsm;              /* its Z buffer (ZBUF) */
    int32_t  ztest;                     /* 0 never, 1 always, 2 GEQUAL, 3 GREATER */
    uint32_t z;                         /* the shape's Z */
    int32_t  halfY;                     /* field rendering: every other row */
    int32_t  box[4];
    int32_t  rect[5][4];
    int32_t  nRects;
    uint32_t cs;                        /* the store's colour (GS units) */
    uint32_t resultBlock;
    int32_t  resultH;
    int32_t  q;
};
void pglr_flare_query(const struct PglrFlareQuery *query);
/* PC (docs/renderer.md, "Fields"): the draws into the buffer at block (FRAME
 * fbw in 64-pixel units, psm, w x h GS pixels) between begin (1) and end (0)
 * at half strength: at the end the buffer becomes the mean of what it held
 * at the begin and what the draws left.  The progressive form of drawing on
 * one field's rows only (the ghost cars' SCANMSK), which the interlaced
 * display and GSBuffer::blitField's field filter averaged with the rows in
 * between.  An end without a begin does nothing. */
void pglr_half_blend(int begin, uint32_t block, uint32_t fbw, uint32_t psm, int w, int h);
/* PC (gfx_set_design_scale): the draws that follow are 2D designed for the
 * 4:3 screen and squeezed by (kx, ky) into the GS frame (the race's dynamic
 * aspect, game/app/pc_aspect.h): what the executor derives from the GS pixel
 * size of a textured sprite (the texel centres' bias) is taken in design
 * pixels.  (1, 1), the default of every frame, ends it. */
void pglr_set_design_scale(float kx, float ky);
/* PC (gfx_set_line_width): the GS lines (LINE, LINE_STRIP) that follow are
 * `px' native pixels wide, drawn as quads; 1 (the default of every frame)
 * draws them as the GPU's own one-pixel lines again. */
void pglr_set_line_width(float px);
/* PC (gfx_post_antialias): the 3D picture of a race view is done in the w x
 * h buffer at `block' (its 2D comes after): with FXAA chosen
 * (settings::anti_aliasing), the executor filters it there. */
void pglr_post_aa(uint32_t block, int w, int h);
/* PC VR (gfx_set_picture_vr; platform/vr.h): the frame being recorded is a
 * stereo race frame: its picture (the eye's shape: gfx_set_picture_aspect)
 * is sized to the headset's eye textures, not the window.  Per frame, like
 * the aspect. */
void pglr_set_picture_vr(int on);
/* PC VR (gfx_vr_eye): one eye's picture is done in the w x h buffer at
 * `block' (before the HUD): the executor draws it onto the eye's swapchain
 * image (vr::eye_acquire; eye 0 left, 1 right). */
void pglr_vr_eye(uint32_t block, int w, int h, int eye);
/* PC VR (gfx_vr_overlay): the overlay picture (the dashboard, the mirror) is
 * done in the w x h buffer at `block': its part (u0, v0) - (u1, v1), as
 * fractions of the buffer, goes onto the panel `kind' (vr.h: 0 the dashboard,
 * 1 the mirror). */
void pglr_vr_overlay(uint32_t block, int w, int h, int kind, float u0, float v0, float u1, float v1);

/* ======================================================================== */
/* Logging                                                                  */
/* ======================================================================== */

/* Logs a message once per distinct key (effects not yet native ...). */
void pglr_log_once(const char *key, const char *message);

}  /* extern "C" */

#endif /* PGL_NATIVE_PGL_BRIDGE_H */
