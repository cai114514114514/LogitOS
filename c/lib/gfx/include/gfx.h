#ifndef GFX_H
#define GFX_H
/* ============================================================================
 * Open Logit -- the 2D rendering engine.
 *
 * WHY IT EXISTS. Before this file there were three coverage/paint paths in the
 * tree: the kernel's M14 glyph rasterizer (c/kernel/gui/raster.c, glyphs only),
 * a second coverage rasterizer inside the widget toolkit (c/apps/gui/aui.c) and
 * a third hand-rolled paint path in the browser (c/apps/gui/browser/paint/renderer.c)
 * -- plus every new app starting from gui_rect. Open Logit is the one engine
 * those are built on. ALL THREE ARE NOW DELETED: aui's and browser_paint's went
 * on the way in, and raster.c went when type moved onto this engine --
 * c/lib/text/glyphras.c converts a font outline to a gfx_path and calls
 * gfx_fill_mask_subs, and that is the whole of what used to be 285 lines of
 * second rasterizer. Every glyph of every label, menu, title and CJK page in
 * this machine is now rasterized by this file set.
 *
 * It is a LIBRARY beside c/lib/text and c/lib/image, and it CONSUMES them: it
 * rasterizes no glyph and decodes no image. That distinction survives the
 * migration exactly -- glyphras.c parses nothing and fills nothing; it converts
 * one representation into another. Blurring the line is how you end up with a
 * fourth rasterizer instead of the first engine.
 *
 * NOT ring-3-only, despite what this comment used to say. c/lib/gfx is filtered
 * out of C_SRC (kernel translation units) for c/lib/video, c/lib/audio,
 * c/lib/media and the image codecs -- but NOT for this directory. fb.c
 * (c/kernel/gui/fb/fb.c) includes gfx.h and calls into gfx_mask_corner/
 * gfx_fill_mask for the window manager's own rounded corners, so this whole
 * file set is compiled into the KERNEL as well as into every ring-3 GUI
 * binary. That means every byte added to a static here is a byte of kernel
 * .bss too -- see the LIMITS section below for what that costs.
 *
 * PHASE 1 IS FILL ONLY. paths (move/line/quad/cubic/close), nonzero + evenodd,
 * scanline coverage antialiasing, four paints (solid / linear gradient / radial
 * gradient / image), Porter-Duff src-over, an affine transform applied to paths,
 * and a rectangle clip. Stroking and path clipping are phase 2 and are not
 * started; see the note at the bottom of this header for what they will cost.
 * 2026-09-13 correction: gfx_stroke.c and the clipped entry points below now
 * implement phase 2. This header remains the software geometry/paint layer;
 * openlogit.h is the native device/resource/command/version API above it.
 *
 * THREE TECHNIQUES CARRIED OVER FROM THE TOOLKIT, because they are why it is
 * cheap enough to run a desktop on:
 *
 *   1. Masks are generated at DEVICE resolution and blitted into a POINT rect
 *      of the same device size, so the compositor's nearest-neighbour rescale
 *      is the identity and the antialiasing survives at 150% and 200%. Generate
 *      at 1x and stretch and you have a blurry mask of a sharp shape.
 *   2. Only what CURVES is rasterized. A rounded rect is a 9-slice: three
 *      opaque bands plus four r x r corner tiles, so it costs O(r^2) and not
 *      O(w*h). A gradient is one 1-px strip the compositor replicates. A
 *      uniform alpha fill is one 1x1 RGBA pixel stretched.
 *   3. Masks are cached by their exact device geometry.
 *
 * NO LIBC, NO MALLOC. Six GUI apps link this and nothing else but crt0 and
 * logit.h -- there is no memset, no memcpy and no allocator to reach for. Every
 * buffer is caller-provided or a bounded static; every bulk clear goes through
 * gfx_zero(), which writes through a volatile pointer precisely so that -O2's
 * loop-idiom pass cannot rewrite it into a call to memset that would then fail
 * to link.
 *
 * INTEGER ONLY. Coordinates are 24.8 fixed point in DEVICE pixels, matrices are
 * 16.16. That is not nostalgia: it is the same arithmetic the kernel's glyph
 * rasterizer uses, so a shape and the type beside it are antialiased by the same
 * rule and agree at the same size.
 * ========================================================================== */

/* The umbrella: the declarations live in the sub-headers, included here in the
 * order the original header declared them. Each sub-header is self-contained. */
#include "gfx_math.h"
#include "gfx_path.h"
#include "gfx_paint.h"
#include "gfx_raster.h"
#include "gfx_stroke.h"
#include "gfx_mask.h"
#include "gfx_shape.h"
#include "gfx_blend.h"

#endif /* GFX_H */
