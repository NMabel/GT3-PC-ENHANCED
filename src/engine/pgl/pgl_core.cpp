/* pgl_core.cpp - pgl's GL layer: the decompiled pgl.c (unit u117,
 * 0x00254828..0x00257A44), ported.
 *
 * CLUT / TEX0 setters, image transfers, the matrix mode / matrix stack and
 * the attribute stack, frustum / ortho, packet helpers, light and
 * light-model setters, sin / cos, the 2D primitives, 3D lines through the
 * combined matrix, the sprite Z / F and the CPU lighting fallback.
 *
 * PC: the VU0 work is pgl_math's (pgl::g_vu0 holds vf28..vf31, the parked
 * matrices and the stacks); the primitives compute the GS vertices exactly
 * as the packets held them (pgl_state_prim) and record them for the
 * renderer; the image transfers are records of the renderer's VRAM
 * registry. */
#include "engine/pgl/pgl_port.h"

using pgl::prim::GsVertex;
using pgl::prim::Batch;

/* The draw flags of the context. */
static inline u32 draw_flags(const GsContext *ctx)
{
    return ctx->halfHeight ? PGLR_HALF_HEIGHT : 0;
}

/* ======================================================================== */
/* CLUT / TEX0                                                              */
/* ======================================================================== */

// FUNC 0x00254828 gs_fix_tex0_tcc
/* TEX0.TCC after a CLUT change: a CT24 palette (clutPsm 1) on an indexed
 * format gives TCC 0, every other case TCC 1. */
void gs_fix_tex0_tcc(GsContext *ctx)
{
    u64 tex0;

    if (ctx->clutPsm != GS_CLUT_CT24) {
        ctx->reg[GS_REG_TEX0] |= GS_TEX0_TCC;
        return;
    }
    tex0 = ctx->reg[GS_REG_TEX0];
    switch ((s32)GS_FIELD_GET(tex0, GS_TEX0_PSM)) {
    case SCE_GS_PSMT8:
    case SCE_GS_PSMT4:
    case SCE_GS_PSMT8H:
    case SCE_GS_PSMT4HL:
    case SCE_GS_PSMT4HH:
        ctx->reg[GS_REG_TEX0] = tex0 & ~GS_TEX0_TCC;
        return;
    default:
        ctx->reg[GS_REG_TEX0] = tex0 | GS_TEX0_TCC;
        return;
    }
}

// FUNC 0x002548C0 gs_set_clut
/* The CLUT of TEX0 and TEX2: CBP, CPSM (CT24 sent as CT32), CSM 0, CSA 0
 * (TEX0), CLD 0 in TEX0 and 1 in TEX2; the TCC fix-up; TEX0 dirty. */
void gs_set_clut(u32 cbpWordAddr, int cpsm)
{
    GsContext *ctx = GS_SPR_CTX;
    GsTex0Bits *tex0 = GS_TEX0_BITS(ctx, GS_REG_TEX0);
    GsTex0Bits *tex2 = GS_TEX0_BITS(ctx, GS_REG_TEX2);

    ctx->clutPsm = (s8)cpsm;
    if (cpsm == GS_CLUT_CT24)
        cpsm = SCE_GS_PSMCT32;
    tex0->CBP = cbpWordAddr >> 6;
    tex2->CBP = cbpWordAddr >> 6;
    tex0->CPSM = cpsm;
    tex2->CPSM = cpsm;
    tex0->CSM = 0;
    tex2->CSM = 0;
    tex0->CSA = 0;
    tex0->CLD = GS_CLD_KEEP;
    tex2->CLD = GS_CLD_LOAD;
    gs_fix_tex0_tcc(ctx);
    ctx->dirty |= GS_DIRTY_BOTH(GS_REG_TEX0);
}

// FUNC 0x002549E0 gs_set_clut_csa
void gs_set_clut_csa(int csa)
{
    GsContext *ctx = GS_SPR_CTX;

    GS_TEX0_BITS(ctx, GS_REG_TEX0)->CSA = csa;
    ctx->dirty |= GS_DIRTY_BOTH(GS_REG_TEX0);
}

/* ======================================================================== */
/* Image transfers                                                          */
/* ======================================================================== */

// FUNC 0x00254A30 gs_build_image_transfer_packet
/* The 6-qword host -> local transfer header at pkt; returns the image's
 * qword count by DPSM (a PSM it does not know: w * h, its quirk). */
int gs_build_image_transfer_packet(u32 *pkt, const u32 *dstBitblt, int dx, int dy, int w, int h)
{
    u32 dpsm = ((const u8 *)dstBitblt)[3];
    s32 qwc = w * h;

    if (dpsm < 0x2D) {
        switch (dpsm) {
        case SCE_GS_PSMCT32:
            qwc = ((qwc << 2) + 0xF) >> 4;
            break;
        case SCE_GS_PSMCT24:
            qwc = ((qwc << 1) + qwc + 0xF) >> 4;
            break;
        case SCE_GS_PSMCT16:
        case SCE_GS_PSMCT16S:
            qwc = ((qwc << 1) + 0xF) >> 4;
            break;
        case SCE_GS_PSMT8:
        case SCE_GS_PSMT8H:
            qwc = (qwc + 0xF) >> 4;
            break;
        case SCE_GS_PSMT4:
        case SCE_GS_PSMT4HH:
        case SCE_GS_PSMT4HL:
            qwc = (qwc + 0x1F) >> 5;
            break;
        default:
            break;
        }
    }
    if (pkt != NULL) {
        u64 *q = (u64 *)pkt;

        q[0] = GIF_NREG(1) | 4;
        q[1] = GIF_REG_AD;
        q[2] = (u64)dstBitblt[0] << 32;
        q[3] = SCE_GS_BITBLTBUF;
        q[4] = (u64)(((u32)dy << 16) | (u32)dx) << 32;
        q[5] = SCE_GS_TRXPOS;
        q[6] = (u64)(u32)w | ((u64)(u32)h << 32);
        q[7] = SCE_GS_TRXREG;
        q[8] = 0;
        q[9] = SCE_GS_TRXDIR;
        q[10] = (u64)(s64)(s32)((u32)qwc | GIF_EOP) | GIF_FLG_IMAGE;
        q[11] = 0;
    }
    return qwc;
}

/* BITBLTBUF's DBP / DBW / DPSM of a transfer. */
static inline u32 bitblt_dbp(u32 wordAddr) { return (wordAddr >> 6) & 0x3FFF; }
static inline u32 bitblt_dbw(int fbw) { return ((u32)fbw >> 6) & 0x3F; }

// FUNC 0x00254B28 gs_upload_image
/* Queues an EE -> GS image upload (list 0).  PC: recorded; the renderer
 * registers it in its VRAM registry when the frame executes. */
void gs_upload_image(u32 fbWordAddr, int psm, int fbw, int dx, int dy, int w, int h, const void *src)
{
    pglr_upload(bitblt_dbp(fbWordAddr), bitblt_dbw(fbw), (u32)psm & 0x3F, dx, dy, w, h, src, 0);
}

// FUNC 0x00254C00 gs_upload_image_sync
/* The same upload done at once.  PC: registered at once. */
void gs_upload_image_sync(u32 fbWordAddr, int psm, int fbw, int dx, int dy, int w, int h, const void *src)
{
    pglr_upload(bitblt_dbp(fbWordAddr), bitblt_dbw(fbw), (u32)psm & 0x3F, dx, dy, w, h, src, 1);
}

// FUNC 0x00254CB0 gs_download_image
/* Queues a GS -> EE download with the readback record (callback
 * readbackCallback(madr, readbackArg) when done).  PC: the renderer copies
 * the native target back when the frame executes. */
void gs_download_image(u32 fbWordAddr, int psm, int fbw, int sx, int sy, int w, int h, void *dst)
{
    GsContext *ctx = GS_SPR_CTX;

    pglr_readback(bitblt_dbp(fbWordAddr), bitblt_dbw(fbw), (u32)psm & 0x3F, sx, sy, w, h, dst,
                  (PglrReadbackFunc)ctx->readbackCallback, (u32)dst, ctx->readbackArg);
}

/* ======================================================================== */
/* Matrix mode / stack (VU0)                                                */
/* ======================================================================== */

// FUNC 0x00254E20 pgl_matrix_mode
/* glMatrixMode: parks the current matrix of the old mode, loads the new
 * mode's (modes other than 0..2 neither). */
void pgl_matrix_mode(int mode)
{
    pgl::ctxMatrixMode(*GS_SPR_CTX, VU0, mode);
}

// FUNC 0x00254F68 pgl_push_matrix
void pgl_push_matrix(void)
{
    pgl::ctxPushMatrix(*GS_SPR_CTX, VU0);
}

// FUNC 0x00254FE8 pgl_pop_matrix
void pgl_pop_matrix(void)
{
    pgl::ctxPopMatrix(*GS_SPR_CTX, VU0);
}

/* ======================================================================== */
/* Attribute stack                                                          */
/* ======================================================================== */

// FUNC 0x00255070 pgl_push_attrib
/* glPushAttrib-like: the groups of mask (byte-wise), then the mask. */
void pgl_push_attrib(u32 m)
{
    GsContext *ctx = GS_SPR_CTX;
    u32 mask = m;
    u8 *sp = g_pglAttribSp;

    if (mask & PGL_ATTR_FOG) {
        memcpy(sp, &ctx->fogStart, 4); sp += 4;
        memcpy(sp, &ctx->fogAmount, 4); sp += 4;
        memcpy(sp, &ctx->gen[GS_GEN_FOGCOL], 8); sp += 8;
    }
    if (mask & PGL_ATTR_SCISSOR) {
        memcpy(sp, &ctx->reg[GS_REG_SCISSOR], 8); sp += 8;
    }
    if (mask & PGL_ATTR_VIEWPORT) {
        memcpy(sp, &ctx->winX, 16); sp += 16;
        memcpy(sp, &ctx->depthNear, 4); sp += 4;
        memcpy(sp, &ctx->depthFar, 4); sp += 4;
    }
    if (mask & PGL_ATTR_DEPTH) {
        memcpy(sp, &ctx->reg[GS_REG_ZBUF], 8); sp += 8;
        memcpy(sp, &ctx->depthScale, 4); sp += 4;
        memcpy(sp, &ctx->depthTest, 1); sp += 1;
        memcpy(sp, &ctx->zMask, 1); sp += 1;
    }
    if (mask & PGL_ATTR_DRAW_BUFFER) {
        memcpy(sp, &ctx->reg[GS_REG_XYOFFSET], 8); sp += 8;
        memcpy(sp, &ctx->gen[GS_GEN_SCANMSK], 8); sp += 8;
        memcpy(sp, &ctx->fieldShift, 1); sp += 1;
        memcpy(sp, &ctx->halfHeight, 1); sp += 1;
        memcpy(sp, &ctx->tgtA, sizeof(GsRenderTarget)); sp += sizeof(GsRenderTarget);
    }
    if (mask & PGL_ATTR_TARGET_B) {
        memcpy(sp, &ctx->tgtB, sizeof(GsRenderTarget)); sp += sizeof(GsRenderTarget);
        memcpy(sp, &ctx->clearColorB, 4); sp += 4;
    }
    memcpy(sp, &mask, 4); sp += 4;
    g_pglAttribSp = sp;
}

// FUNC 0x002552A0 pgl_pop_attrib
/* glPopAttrib-like: the mask, then the groups in reverse order, re-applied
 * (the draw buffer through gfx_set_draw_buffer, the depth range, the
 * viewport, the fog parameters). */
void pgl_pop_attrib(void)
{
    GsContext *ctx = GS_SPR_CTX;
    s8 *sp = (s8 *)g_pglAttribSp;
    u32 mask;

    sp -= 4; memcpy(&mask, sp, 4);
    if (mask & PGL_ATTR_TARGET_B) {
        sp -= 4; memcpy(&ctx->clearColorB, sp, 4);
        sp -= sizeof(GsRenderTarget); memcpy(&ctx->tgtB, sp, sizeof(GsRenderTarget));
    }
    if (mask & PGL_ATTR_DRAW_BUFFER) {
        sp -= sizeof(GsRenderTarget); memcpy(&ctx->tgtA, sp, sizeof(GsRenderTarget));
        gfx_set_draw_buffer((u32)ctx->tgtA.base << 6, ctx->tgtA.psm, ctx->tgtA.width, ctx->tgtA.height);
        sp -= 1; ctx->halfHeight = (u8)(s32)*sp;
        sp -= 1; ctx->fieldShift = (u8)(s32)*sp;
        sp -= 8; memcpy(&ctx->gen[GS_GEN_SCANMSK], sp, 8);
        sp -= 8; memcpy(&ctx->reg[GS_REG_XYOFFSET], sp, 8);
        ctx->dirty |= GS_DIRTY_GEN(GS_GEN_SCANMSK) | GS_DIRTY_BOTH(GS_REG_XYOFFSET);
    }
    if (mask & PGL_ATTR_DEPTH) {
        sp -= 1; ctx->zMask = (u8)(s32)*sp;
        sp -= 1; ctx->depthTest = (u8)(s32)*sp;
        sp -= 4; memcpy(&ctx->depthScale, sp, 4);
        sp -= 8; memcpy(&ctx->reg[GS_REG_ZBUF], sp, 8);
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_ZBUF);
        gs_update_depth_range(ctx);
        if (ctx->depthTest != 0)
            gfx_enable(PGL_CAP_DEPTH_TEST);
        else
            gfx_disable(PGL_CAP_DEPTH_TEST);
    }
    if (mask & PGL_ATTR_VIEWPORT) {
        sp -= 4; memcpy(&ctx->depthFar, sp, 4);
        sp -= 4; memcpy(&ctx->depthNear, sp, 4);
        sp -= 16; memcpy(&ctx->winX, sp, 16);
        gs_update_scissor_and_viewport(ctx);
        gs_update_depth_range(ctx);
    }
    if (mask & PGL_ATTR_SCISSOR) {
        sp -= 8; memcpy(&ctx->reg[GS_REG_SCISSOR], sp, 8);
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_SCISSOR);
    }
    if (mask & PGL_ATTR_FOG) {
        sp -= 8; memcpy(&ctx->gen[GS_GEN_FOGCOL], sp, 8);
        sp -= 4; memcpy(&ctx->fogAmount, sp, 4);
        sp -= 4; memcpy(&ctx->fogStart, sp, 4);
        gfx_set_fog_params(ctx->fogStart, ctx->fogAmount);
        ctx->dirty |= GS_DIRTY_GEN(GS_GEN_FOGCOL);
    }
    g_pglAttribSp = (u8 *)sp;
}

/* ======================================================================== */
/* Projection matrices                                                      */
/* ======================================================================== */

// FUNC 0x002555A8 pgl_frustum
/* glFrustum (the 2n terms with `focal'), multiplied into the current
 * matrix. */
void pgl_frustum(float l, float r, float b, float t, float n, float f, float focal)
{
    alignas(16) f32 m[16];
    f32 invWidth = 1.0f / (r - l);
    f32 invHeight = 1.0f / (t - b);
    f32 invDepth = 1.0f / (f - n);

    m[0] = 2.0f * focal * invWidth;
    m[1] = 0.0f;
    m[2] = 0.0f;
    m[3] = 0.0f;
    m[4] = 0.0f;
    m[5] = 2.0f * focal * invHeight;
    m[6] = 0.0f;
    m[7] = 0.0f;
    m[8] = (r + l) * invWidth;
    m[9] = (t + b) * invHeight;
    m[10] = -(f + n) * invDepth;
    m[11] = -1.0f;
    m[12] = 0.0f;
    m[13] = 0.0f;
    m[14] = f * (-2.0f * n) * invDepth;
    m[15] = 0.0f;
    pgl_mult_matrix(m);
}

// FUNC 0x00255680 pgl_ortho
void pgl_ortho(float l, float r, float b, float t, float n, float f)
{
    alignas(16) f32 m[16];

    m[0] = 2.0f / (r - l);
    m[1] = 0.0f;
    m[2] = 0.0f;
    m[3] = 0.0f;
    m[4] = 0.0f;
    m[5] = 2.0f / (t - b);
    m[6] = 0.0f;
    m[7] = 0.0f;
    m[8] = 0.0f;
    m[9] = 0.0f;
    m[10] = -2.0f / (f - n);
    m[11] = 0.0f;
    m[12] = (l + r) / (l - r);
    m[13] = (b + t) / (b - t);
    m[14] = (n + f) / (n - f);
    m[15] = 1.0f;
    pgl_mult_matrix(m);
}

/* ======================================================================== */
/* Packet helpers                                                           */
/* ======================================================================== */

// FUNC 0x00255760 gfx_alloc_tail
/* qwc qwords from the end of the packet buffer (the game's vertex / colour
 * arrays; the renderer reads them when the frame executes). */
void *gfx_alloc_tail(int qwc)
{
    GsContext *ctx = GS_SPR_CTX;

    ctx->end = (u32 *)((u8 *)ctx->end - qwc * 16);
    /* PC: the two ends crossed (the frame's packet buffer is full: T001) */
    if ((uintptr_t)ctx->end < (uintptr_t)ctx->cur) {
        static int logged;
        if (!logged++)
            printf("PC: gfx_alloc_tail: the frame's packet buffer (0x%X bytes) is full\n", ctx->bufSize);
    }
    return ctx->end;
}

// FUNC 0x00255780 gfx_open_direct
/* A DIRECT block of qwc qwords the caller fills with GIF data; returns
 * where the data go.  PC: the block is recorded and decoded at execution
 * (the caller writes it after this call). */
u32 *gfx_open_direct(int qwc)
{
    GsContext *ctx = GS_SPR_CTX;
    u32 *p = ctx->cur;

    if (qwc != 0) {
        ctx->cur = p + 4 + qwc * 4;
        p[0] = DMATAG_CNT(qwc);
        p[2] = VIF_FLUSH;
        p[3] = VIF_DIRECT(qwc);
        p[1] = 0;
        pglr_gif(p + 4, (u32)qwc, draw_flags(ctx));
    }
    return p + 4;
}

// FUNC 0x002557D0 gs_prim_attr_with
u32 gs_prim_attr_with(u32 bits)
{
    return GS_SPR_CTX->primAttr | bits;
}

/* ======================================================================== */
/* Lights                                                                   */
/* ======================================================================== */

// FUNC 0x002557E8 pgl_lightfv
void pgl_lightfv(int light, u32 pname, const float *params)
{
    GsContext *ctx = GS_SPR_CTX;
    GsVec4Arg v;

    if ((u32)(light - PGL_CAP_LIGHT0) >= 8)
        return;
    switch (pname) {
    case PGL_LIGHT_AMBIENT:
    case PGL_LIGHT_DIFFUSE:
    case PGL_LIGHT_SPECULAR:
    case PGL_LIGHT_SPOT_DIRECTION:
    case PGL_LIGHT_POSITION:
        memcpy(&v, params, sizeof(v));
        gs_set_light_param_v(ctx, &ctx->lights[light - PGL_CAP_LIGHT0], pname, v);
        break;
    case PGL_LIGHT_SPOT_EXPONENT:
    case PGL_LIGHT_SPOT_CUTOFF:
    case PGL_LIGHT_CONST_ATTEN:
    case PGL_LIGHT_LINEAR_ATTEN:
    case PGL_LIGHT_QUAD_ATTEN:
        gs_set_light_param_f(ctx, &ctx->lights[light - PGL_CAP_LIGHT0], pname, params[0]);
        break;
    }
}

// FUNC 0x00255898 pgl_light_modelfv
void pgl_light_modelfv(int pname, const float *params)
{
    GsContext *ctx = GS_SPR_CTX;

    switch ((u32)pname) {
    case PGL_LIGHTMODEL_AMBIENT:
        ctx->globalAmbient.x = params[0];
        ctx->globalAmbient.y = params[1];
        ctx->globalAmbient.z = params[2];
        ctx->globalAmbient.w = params[3];
        break;
    case PGL_LIGHTMODEL_LOCAL_VIEWER:
        if (params[0] != 0.0f)
            ctx->lightFlags |= PGL_LF_LOCAL_VIEWER;
        else
            ctx->lightFlags &= ~PGL_LF_LOCAL_VIEWER;
        break;
    case 2:
        break;
    case PGL_LIGHTMODEL_EYE:
        ctx->eyeWorld.x = params[0];
        ctx->eyeWorld.y = params[1];
        ctx->eyeWorld.z = params[2];
        ctx->eyeWorld.w = params[3];
        break;
    }
}

// FUNC 0x00255940 float_to_gs_param_encode
/* A non-negative float (an exponent) as 11 greedy bits in steps 64 ..
 * 1/16: (bits >> 4) | bitrev4(bits & 0xF) << 6 | top << 16. */
u32 float_to_gs_param_encode(float v)
{
    s32 bits = 0;
    s32 top = 0;
    f32 step = 64.0f;
    s32 i;
    s32 frac, fracRev;

    for (i = 10; i >= 0; i--) {
        bits <<= 1;
        if (step <= v) {
            v -= step;
            if (top < i)
                top = i;
            bits |= 1;
        }
        step *= 0.5f;
    }
    frac = bits & 0xF;
    bits >>= 4;
    fracRev = 0;
    for (i = 0; i < 4; i++) {
        fracRev <<= 1;
        fracRev |= (frac >> i) & 1;
    }
    bits |= fracRev << 6;
    return (u32)(bits | (top << 16));
}

// FUNC 0x002559D0 float_pow_encoded
/* x ** e, e in float_to_gs_param_encode's format (square-and-multiply over
 * the integer bits, repeated square roots for the fraction; wrong for
 * exponents below 0.5, as the binary). */
float float_pow_encoded(float x, int enc)
{
    f32 r = 1.0f;
    f32 p = x;
    s32 top;
    s32 n;

    x = sqrtf(x);
    top = enc >> 16;
    for (n = top - 4; n >= 0; n--) {
        if (enc & 1)
            r *= p;
        p *= p;
        enc >>= 1;
    }
    enc >>= (9 - top) & 31;
    for (n = 4; n >= 0; n--) {
        if (enc & 1)
            r *= x;
        enc >>= 1;
        x = sqrtf(x);
    }
    return r;
}

/* ======================================================================== */
/* Sine / cosine (VU0)                                                      */
/* ======================================================================== */

// FUNC 0x00255A48 pgl_sin_deg
float pgl_sin_deg(float deg)
{
    return pgl::pglSinDeg(VU0, deg);
}

// FUNC 0x00255A70 pgl_cos_deg
float pgl_cos_deg(float deg)
{
    return pgl::pglCosDeg(VU0, deg);
}

// FUNC 0x00255A98 pgl_sincos_deg
void pgl_sincos_deg(float deg, float *s, float *c)
{
    pgl::pglSinCosDeg(VU0, deg, s, c);
}

// FUNC 0x00255AE0 pseudo_angle
float pseudo_angle(float a, float b)
{
    return pgl::pseudoAngle(a, b);
}

/* ======================================================================== */
/* Simple state setters                                                     */
/* ======================================================================== */

// FUNC 0x00255B70 gs_set_scanmask_mode
void gs_set_scanmask_mode(u32 mode)
{
    GsContext *ctx = GS_SPR_CTX;

    switch (mode) {
    case PGL_SCANMASK_OFF:
        ctx->gen[GS_GEN_SCANMSK] &= ~GS_FIELD_MASK(GS_SCANMSK_MSK);
        break;
    case PGL_SCANMASK_EVEN:
        ctx->gen[GS_GEN_SCANMSK] = GS_FIELD_SET(ctx->gen[GS_GEN_SCANMSK], GS_SCANMSK_MSK, GS_SCANMSK_EVEN);
        break;
    case PGL_SCANMASK_ODD:
        ctx->gen[GS_GEN_SCANMSK] |= GS_SCANMSK_ODD;
        break;
    case PGL_SCANMASK_FIELD:
        ctx->gen[GS_GEN_SCANMSK] = GS_FIELD_SET(ctx->gen[GS_GEN_SCANMSK], GS_SCANMSK_MSK,
                                                (u8)ctx->field | GS_SCANMSK_EVEN);
        break;
    case PGL_SCANMASK_OTHER_FIELD:
        ctx->gen[GS_GEN_SCANMSK] = GS_FIELD_SET(ctx->gen[GS_GEN_SCANMSK], GS_SCANMSK_MSK,
                                                (u8)ctx->field ^ GS_SCANMSK_ODD);
        break;
    }
    ctx->dirty |= GS_DIRTY_GEN(GS_GEN_SCANMSK);
}

/* PC (G001_flare.h; docs/renderer.md, "Fields"): the draw buffer (target A)
 * of the drawing at half strength, to the native renderer. */
void gfx_half_blend_begin(void)
{
    GsRenderTarget *t = &GS_SPR_CTX->tgtA;

    pglr_half_blend(1, t->base, t->fbw, t->psm, t->width, t->height);
}

void gfx_half_blend_end(void)
{
    GsRenderTarget *t = &GS_SPR_CTX->tgtA;

    pglr_half_blend(0, t->base, t->fbw, t->psm, t->width, t->height);
}

/* PC (G001_flare.h; docs/renderer.md, "Aspect"): the frame's picture, to
 * the native renderer. */
void gfx_set_picture_aspect(float aspect)
{
    pglr_set_picture_aspect(aspect);
}

void gfx_set_picture_multi(int multi)
{
    pglr_set_picture_multi(multi);
}

/* PC VR (G001_flare.h) */
void gfx_set_picture_vr(int on)
{
    pglr_set_picture_vr(on);
}

void gfx_vr_eye(u32 fbWordAddr, int w, int h, int eye)
{
    pglr_vr_eye(fbWordAddr >> 6, w, h, eye);
}

void gfx_vr_overlay(u32 fbWordAddr, int w, int h, int kind, f32 u0, f32 v0, f32 u1, f32 v1)
{
    pglr_vr_overlay(fbWordAddr >> 6, w, h, kind, u0, v0, u1, v1);
}

/* PC (G001_flare.h; docs/renderer.md "Aspect"): the menus' stretch. */
void gfx_set_menu_aspect(float aspect)
{
    pglr_set_menu_aspect(aspect);
}
// FUNC 0x00255C30 pgl_set_const_vector
void pgl_set_const_vector(float x, float y, float z, float w)
{
    GsContext *ctx = GS_SPR_CTX;

    ctx->constVector.x = x;
    ctx->constVector.y = y;
    ctx->constVector.z = z;
    ctx->constVector.w = w;
}

// FUNC 0x00255C50 pgl_set_const_color
void pgl_set_const_color(float r, float g, float b, float a)
{
    GsContext *ctx = GS_SPR_CTX;

    ctx->constColor.x = r * 255.0f;
    ctx->constColor.y = g * 255.0f;
    ctx->constColor.z = b * 255.0f;
    ctx->constColor.w = a * 128.0f;
}

/* ======================================================================== */
/* 2D primitives                                                            */
/* ======================================================================== */

// FUNC 0x00255C90 gs_draw_flat_sprite
/* An untextured sprite in `color', pixels relative to the draw buffer
 * offset (no draw-window origin). */
void gs_draw_flat_sprite(int x0, int y0, int x1, int y1)
{
    GsContext *ctx = GS_SPR_CTX;
    GsVertex v[2];
    Batch b;

    gs_flush_dirty_regs();
    b = pgl::prim::flatSprite(pgl::prim::primCtxOf(*ctx), x0, y0, x1, y1, v);
    pglr_prim(b.prim, v, b.n, draw_flags(ctx));
}

// FUNC 0x00255DF0 gs_draw_flat_sprite_strips
/* The same flat sprite cut at 64-pixel boundaries. */
void gs_draw_flat_sprite_strips(int x0, int y0, int x1, int y1)
{
    GsContext *ctx = GS_SPR_CTX;
    GsVertex stackV[64];
    GsVertex *v = stackV;
    int count;
    Batch b;

    gs_flush_dirty_regs();
    count = pgl::prim::flatSpriteStripCount(x0, x1) * 2;
    if (count > 64)
        v = (GsVertex *)malloc(sizeof(GsVertex) * count);
    b = pgl::prim::flatSpriteStrips(pgl::prim::primCtxOf(*ctx), x0, y0, x1, y1, v);
    pglr_prim(b.prim, v, b.n, draw_flags(ctx));
    if (v != stackV)
        free(v);
}

// FUNC 0x00256020 gs_draw_textured_sprite
/* A textured sprite (SPRITE | TME | FST) in fullColor (PC: was colorTex),
 * the draw window origin added, UVs + half a texel. */
void gs_draw_textured_sprite(int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1)
{
    GsContext *ctx = GS_SPR_CTX;
    GsVertex v[2];
    Batch b;

    b = pgl::prim::texturedSprite(pgl::prim::primCtxOf(*ctx), x0, y0, x1, y1, u0, v0, u1, v1, v);
    gs_flush_dirty_regs();
    pglr_prim(b.prim, v, b.n, draw_flags(ctx));
}

// FUNC 0x00256188 gs_draw_textured_sprite_f
void gs_draw_textured_sprite_f(float x0, float y0, float x1, float y1,
                               float u0, float v0, float u1, float v1)
{
    GsContext *ctx = GS_SPR_CTX;
    GsVertex v[2];
    Batch b;

    b = pgl::prim::texturedSpriteF(pgl::prim::primCtxOf(*ctx), x0, y0, x1, y1, u0, v0, u1, v1, v);
    gs_flush_dirty_regs();
    pglr_prim(b.prim, v, b.n, draw_flags(ctx));
}

// FUNC 0x00256360 gs_draw_textured_sprite_strips_f
/* A textured sprite cut at 32-pixel boundaries, u interpolated by integer
 * division; nothing for a zero width. */
void gs_draw_textured_sprite_strips_f(float x0, float y0, float x1, float y1,
                                      float u0, float v0, float u1, float v1)
{
    GsContext *ctx = GS_SPR_CTX;
    pgl::prim::PrimCtx pc;
    GsVertex stackV[128];
    GsVertex *v = stackV;
    int count;
    Batch b;

    gs_flush_dirty_regs();
    pc = pgl::prim::primCtxOf(*ctx);
    count = pgl::prim::texturedSpriteStripCount(pc, x0, x1) * 2;
    if (count == 0)
        return;
    if (count > 128)
        v = (GsVertex *)malloc(sizeof(GsVertex) * count);
    b = pgl::prim::texturedSpriteStripsF(pc, x0, y0, x1, y1, u0, v0, u1, v1, v);
    pglr_prim(b.prim, v, b.n, draw_flags(ctx));
    if (v != stackV)
        free(v);
}

// FUNC 0x002566F0 gs_draw_line
/* A line in `color', pixels relative to the draw window. */
void gs_draw_line(int x0, int y0, int x1, int y1)
{
    GsContext *ctx = GS_SPR_CTX;
    GsVertex v[2];
    Batch b;

    gs_flush_dirty_regs();
    b = pgl::prim::line(pgl::prim::primCtxOf(*ctx), x0, y0, x1, y1, v);
    pglr_prim(b.prim, v, b.n, draw_flags(ctx));
}

// FUNC 0x00256838 gs_draw_line_f
void gs_draw_line_f(float x0, float y0, float x1, float y1)
{
    GsContext *ctx = GS_SPR_CTX;
    GsVertex v[2];
    Batch b;

    gs_flush_dirty_regs();
    b = pgl::prim::lineF(pgl::prim::primCtxOf(*ctx), x0, y0, x1, y1, v);
    pglr_prim(b.prim, v, b.n, draw_flags(ctx));
}

/* ======================================================================== */
/* Primitives through the VU0 combined matrix                               */
/* ======================================================================== */

// FUNC 0x002569C0 pgl_draw_line_strip_2d
/* A 2D line strip of n points through the combined matrix (z = 0); a
 * segment whose ends are outside one plane gets ADC.  PC: in runs of 256
 * points (a run repeats the previous run's last point, which never draws). */
void pgl_draw_line_strip_2d(const float *xy, int n)
{
    GsContext *ctx = GS_SPR_CTX;
    GsVertex v[256];
    pgl::prim::PrimCtx pc;

    if (n < 2)
        return;
    if (ctx->matrixDirty != 0)
        pgl_sync_matrices(ctx);
    pc = pgl::prim::primCtxOf(*ctx);
    for (int start = 0; start < n - 1; ) {
        int count = n - start;
        Batch b;

        if (count > 256)
            count = 256;
        b = pgl::prim::lineStrip2d(pc, VU0, xy + start * 2, count, v);
        pglr_prim(b.prim, v, b.n, draw_flags(ctx));
        start += count - 1;
    }
}

// FUNC 0x00256B80 pgl_set_sprite_depth
/* The sprites' Z / F from an eye-space distance z (through the projection
 * and the viewport; F 0xFF when z <= 0). */
void pgl_set_sprite_depth(float z)
{
    pgl::ctxSetSpriteDepth(*GS_SPR_CTX, VU0, z);
}

// FUNC 0x00256C68 pgl_set_sprite_depth_i
void pgl_set_sprite_depth_i(int z)
{
    pgl_set_sprite_depth((f32)z);
}

// FUNC 0x00256C78 gs_set_sprite_zf
void gs_set_sprite_zf(int z, u32 fog)
{
    GsContext *ctx = GS_SPR_CTX;

    if (z < 0)
        z = 0;
    if (z > GS_Z24_MAX)
        z = GS_Z24_MAX;
    if (fog > 0xFF)
        fog = GS_FOG_NONE;
    ctx->spriteZF = (fog << 24) | (u32)z;
}

// FUNC 0x00256CB8 clip_line_homogeneous
int clip_line_homogeneous(float *a, float *b)
{
    pgl::Vec4 va = vec4_load(a), vb = vec4_load(b);
    u32 r = pgl::clipLineHomogeneous(va, vb);

    memcpy(a, &va, 16);
    memcpy(b, &vb, 16);
    return (int)r;
}

// FUNC 0x00256E60 pgl_draw_line_3d
/* A 3D line through the combined matrix, clipped, divided, viewport-mapped
 * (nothing when rejected). */
void pgl_draw_line_3d(const float *p0, const float *p1)
{
    GsContext *ctx = GS_SPR_CTX;
    GsVertex v[2];
    Batch b;

    if (ctx->matrixDirty != 0)
        pgl_sync_matrices(ctx);
    b = pgl::prim::line3d(pgl::prim::primCtxOf(*ctx), VU0, p0, p1, v);
    if (b.n == 0)
        return;
    gs_flush_dirty_regs();
    pglr_prim(b.prim, v, b.n, draw_flags(ctx));
}

// FUNC 0x00256FF8 pgl_draw_line_3f
void pgl_draw_line_3f(float x0, float y0, float z0, float x1, float y1, float z1)
{
    alignas(16) f32 a[4];
    alignas(16) f32 b[4];

    a[0] = x0;
    a[1] = y0;
    a[2] = z0;
    a[3] = 0.0f;
    b[0] = x1;
    b[1] = y1;
    b[2] = z1;
    b[3] = 0.0f;
    pgl_draw_line_3d(a, b);
}

// FUNC 0x00257030 gs_emit_stmask
/* VIF STMASK into the packet (the VU1 unpack mask: the port's renderer
 * reads the arrays directly); the context remembers it. */
void gs_emit_stmask(GsContext *ctx, u32 mask)
{
    u32 *p = ctx->cur;

    p[0] = DMATAG_CNT(0);
    p[1] = 0;
    p[2] = VIF_STMASK;
    p[3] = mask;
    ctx->cur = p + 4;
    ctx->stmask = mask;
}

// FUNC 0x00257060 vu0_clip_code
int vu0_clip_code(const float *v)
{
    return (int)pgl::clipCode(vec4_load(v));
}

/* ======================================================================== */
/* CPU lighting (__pglShading)                                              */
/* ======================================================================== */

// FUNC 0x002570C8 pgl_shade_vertices
/* OpenGL-style lighting of n vertices on the CPU (material 0, the light
 * list, attenuation, spots, Blinn specular), out = clamped RGBA8. */
void pgl_shade_vertices(GsContext *ctx, u32 *out, const float *pos, const float *nrm,
                        const u32 *colors, int n)
{
    GsMaterial *mat = &ctx->material[0];
    f32 scale = ctx->colorScale;
    f32 base[4];
    f32 normal[3];
    f32 alpha;
    f32 r, g, b, a;
    GsLight *lt;
    f32 k255 = 255.0f;

    base[0] = mat->emission.x + ctx->globalAmbient.x * mat->ambient.x;
    base[1] = mat->emission.y + ctx->globalAmbient.y * mat->ambient.y;
    base[2] = mat->emission.z + ctx->globalAmbient.z * mat->ambient.z;
    base[3] = mat->emission.w + ctx->globalAmbient.w * mat->ambient.w;
    while (n-- != 0) {
        if (colors != NULL && ctx->colorMaterial != 0) {
            gs_set_material_param(ctx->colorMaterialFaces, ctx->colorMaterialKind, *colors);
            base[0] = mat->emission.x + ctx->globalAmbient.x * mat->ambient.x;
            base[1] = mat->emission.y + ctx->globalAmbient.y * mat->ambient.y;
            base[2] = mat->emission.z + ctx->globalAmbient.z * mat->ambient.z;
            base[3] = mat->emission.w + ctx->globalAmbient.w * mat->ambient.w;
        }
        r = base[0];
        g = base[1];
        b = base[2];
        a = base[3];
        if (colors != NULL && ctx->colorMaterial == 0) {
            u32 c = *colors;

            r = base[0] + (f32)(c & 0xFF) / k255;
            g = base[1] + (f32)((c >> 8) & 0xFF) / k255;
            b = base[2] + (f32)((c >> 16) & 0xFF) / k255;
            a = base[3] + (f32)((c >> 24) & 0xFF) / k255;
        }
        normal[0] = nrm[0];
        normal[1] = nrm[1];
        normal[2] = nrm[2];
        nrm += 3;
        alpha = a * scale;
        for (lt = ctx->lightList; lt != NULL; lt = lt->next) {
            f32 lx, ly, lz;
            f32 atten, spot;
            f32 ambR, ambG, ambB;
            f32 nDotL, lightScale;

            if (lt->eyePosition.w == 0.0f) {
                atten = 1.0f;
                lx = lt->eyePosition.x;
                ly = lt->eyePosition.y;
                spot = 1.0f;
                lz = lt->eyePosition.z;
            } else {
                f32 dist;

                lx = lt->eyePosition.x - pos[0];
                ly = lt->eyePosition.y - pos[1];
                lz = lt->eyePosition.z - pos[2];
                dist = sqrtf(lx * lx + ly * ly + lz * lz);
                if (0.0001f < dist) {
                    f32 inv = 1.0f / dist;

                    lz *= inv;
                    lx *= inv;
                    ly *= inv;
                }
                atten = 1.0f / (lt->constAtten + dist * (lt->linearAtten + dist * lt->quadAtten));
                spot = 1.0f;
                if (lt->spotCutoff != PGL_SPOT_CUTOFF_NONE) {
                    f32 spotDot = -lx * lt->spotDir.x + -ly * lt->spotDir.y + -lz * lt->spotDir.z;

                    spot = 0.0f;
                    if (!(spotDot <= 0.0f) && !(spotDot < lt->spotCosCutoff)) {
                        if (lt->spotTable != NULL)
                            spot = lt->spotTable[(s32)(ee_clampf(spotDot, 0.0f, 1.0f) * lt->spotTableScale)];
                        else
                            spot = float_pow_encoded(spotDot, (int)lt->spotExpEnc);
                    }
                }
            }
            nDotL = normal[0] * lx + normal[1] * ly + normal[2] * lz;
            ambG = mat->ambient.y * lt->ambient.y;
            ambR = mat->ambient.x * lt->ambient.x;
            ambB = mat->ambient.z * lt->ambient.z;
            if (nDotL <= 0.0f) {
                lightScale = atten * spot;
                b += lightScale * ambB;
                r += lightScale * ambR;
                g += lightScale * ambG;
            } else {
                f32 difR, difG, difB;
                f32 hx, hy, hz;
                f32 specR, specG, specB;
                f32 nDotH;

                difR = nDotL * mat->diffuse.x * lt->diffuse.x;
                difG = nDotL * mat->diffuse.y * lt->diffuse.y;
                difB = nDotL * mat->diffuse.z * lt->diffuse.z;
                if (ctx->lightFlags & PGL_LF_LOCAL_VIEWER) {
                    f32 vx = pos[0] - ctx->eyeModel.x;
                    f32 vy = pos[1] - ctx->eyeModel.y;
                    f32 vz = pos[2] - ctx->eyeModel.z;
                    f32 len = sqrtf(vx * vx + vy * vy + vz * vz);

                    if (0.0001f < len) {
                        f32 inv = 1.0f / len;

                        vz *= inv;
                        vx *= inv;
                        vy *= inv;
                    }
                    hx = lx - vx;
                    hy = ly - vy;
                    hz = lz - vz;
                } else {
                    hx = lx - ctx->viewDir.x;
                    hy = ly - ctx->viewDir.y;
                    hz = lz - ctx->viewDir.z;
                }
                nDotH = normal[0] * hx + normal[1] * hy + normal[2] * hz;
                if (nDotH <= 0.0f) {
                    specR = 0.0f;
                    specG = 0.0f;
                    specB = 0.0f;
                } else {
                    nDotH = ee_minf(nDotH * pgl::vu::rsqrt(1.0f, hx * hx + hy * hy + hz * hz), 1.0f);
                    if (mat->x44.powTable != NULL)
                        nDotH = mat->x44.powTable[(s32)(nDotH * mat->x48.powTableScale + 0.5f)];
                    else
                        nDotH = float_pow_encoded(nDotH, (int)mat->shininessEnc);
                    specG = nDotH * mat->specular.y * lt->specular.y;
                    specR = nDotH * mat->specular.x * lt->specular.x;
                    specB = nDotH * mat->specular.z * lt->specular.z;
                }
                lightScale = atten * spot;
                r += lightScale * (ambR + difR + specR);
                g += lightScale * (ambG + difG + specG);
                b += lightScale * (ambB + difB + specB);
            }
        }
        pos += 3;
        {
            s32 outR = (s32)ee_clampf(r * scale, 0.0f, k255);
            s32 outG = (s32)ee_clampf(g * scale, 0.0f, k255);
            s32 outB = (s32)ee_clampf(b * scale, 0.0f, k255);
            s32 outA = (s32)ee_clampf(alpha, 0.0f, scale);

            if (colors != NULL)
                colors++;
            *out = (((u32)outA << 24) | (u32)outR) | (((u32)outG << 8) | ((u32)outB << 16));
            out++;
        }
    }
}

// FUNC 0x00257910 __pglShading
/* Lights n vertices into a block from the packet end (one RGBA word per
 * vertex) and returns it; prints "__pglShading (): sorry." on every call.
 * Without normals the current normal is replicated into the block itself
 * (12n bytes into the 4n-byte block, past its end: as in the binary). */
u32 *__pglShading(GsContext *ctx, const float *pos, const float *nrm, const u32 *colors, int n)
{
    u32 *out;

    printf("__pglShading (): sorry.\n");
    out = (u32 *)((u8 *)ctx->end - ((n + 3) >> 2) * 16);
    ctx->end = out;
    if (nrm == NULL) {
        f32 nx = ctx->normal[0];
        f32 ny = ctx->normal[1];
        f32 nz = ctx->normal[2];
        f32 *q = (f32 *)out;
        s32 i;

        nrm = q;
        for (i = 0; i < n; i++) {
            q[i * 3 + 0] = nx;
            q[i * 3 + 1] = ny;
            q[i * 3 + 2] = nz;
        }
    }
    switch (ctx->lightMode) {
    case 0:
    case 2:
    case 3:
    case 4:
        pgl_shade_vertices(ctx, out, pos, nrm, colors, n);
        return out;
    case 1:
        printf("%s: Sorry, Not yet implemented.\n", "__pglShading");
        return out;
    }
    return out;
}
