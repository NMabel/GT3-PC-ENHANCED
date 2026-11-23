/* overlay.h - drawing over the picture for GT3 PC's own panels (the frame
 * time monitor, the settings menu): coloured rectangles and a 5 x 7 pixel
 * font, in back buffer pixels, drawn on the back buffer just before Present
 * with its own D3D objects (the renderer forgets its bindings after).  No
 * Windows headers. */
#pragma once

#include <stdint.h>

struct ID3D11RenderTargetView;

namespace overlay {

/* R8G8B8A8_UNORM: 0xAABBGGRR */
constexpr uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return r | (g << 8) | (b << 16) | (a << 24);
}

/* Starts a panel's drawing (the D3D objects made on the first use); false
 * when there is nothing to draw on (then no end). */
bool begin();
/* PC (2026-11-08): the same for a target of w x h pixels (another screen's
 * back buffer, gpu::screen_backbuffer: end() then draws on that). */
bool begin_sized(int w, int h);
/* The back buffer's size (during a panel's drawing). */
void size(int *w, int *h);
void rect(float x, float y, float w, float h, uint32_t color);
/* A character cell is 6 x 9 font pixels of s screen pixels.  PC
 * (2026-11-16): the strings are UTF-8 (platform/ui_text.h's languages): the
 * accented capitals and Cyrillic in the same font, an accent in rows -3 and
 * -2 over the cell; other characters (Japanese) MS Gothic's 12 pixel bitmaps,
 * one or two cells wide, over rows -3 .. 8. */
float text_width(const char *str, int s);
/* The width in cells (what strlen was for an ASCII string). */
int text_cells(const char *str);
/* The text at (x, y), upper case (lower case shown so, others as blanks).
 * Returns the x after it. */
float text(float x, float y, int s, const char *str, uint32_t color);
/* Draws what was built on the back buffer. */
void end(ID3D11RenderTargetView *backbuffer);
/* PC VR (platform/vr.h kOverlayDesktop): while on, what end() draws on the game window's back buffer is drawn
 * a second time onto the headset's desktop panel, so the F2 menu, the Esc menu and the rest are seen in the
 * headset too.  The caller (pgl_targets.cpp present) sets it round the panels' present calls, once a frame. */
void headset_mirror(bool on);

} // namespace overlay
