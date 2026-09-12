/*
 * WLED-C 2D Effects Component Header
 *
 * This file is part of the WLED pure-C rewrite for ESP-IDF.
 *
 * A total of 13 2D effects were ported from the original WLED FX.cpp
 * (wled00/) implementation, with slot numbers strictly aligned with the
 * FX_MODE_* enum in wled00/FX.h (range 114..186). Unselected slots are
 * NULL and rendered as black.
 *
 * Porting rules (differences from the original implementation):
 *  - The whole component is guarded by WLEDC_ENABLE_2D (CONFIG based).
 *  - Effects call wledc_segment_is_2d() on entry; failure falls back to
 *    mode-0-like static fill of colors[0].
 *  - Virtual addressing goes through wled_c_2d.h (virtual_width/height,
 *    set/get_pixel_color_xy); raw physical traversal is not used for
 *    per-pixel drawing (fadeToBlackBy/blur still traverse the physical
 *    buffer, aligned with the original raw semantics).
 *  - Segment-local runtime data (Matrix) is applied for via
 *    wledc_segment_request_data(); failure falls back to a static fill.
 *  - Native check1/check2/check3 bit-fields are not provided by the
 *    host segment structure; they are fixed to the WLED UI defaults
 *    (all false/0) and noted per effect.
 *  - Drift keeps the native float math via local fx2d_sin_t/fx2d_cos_t
 *    replicas (GNU C17 on ESP targets provides float math).
 *  - Some native WLED quirks (Lissajous map rounding special-case,
 *    ColoredBursts beatsin16 max clamps, DNASpiral anti-hole line
 *    stepping, Sindots byte narrowing, BlackHole inner-star custom3
 *    used without >>3, etc.) are reproduced as-is, with comments in
 *    the code.
 */

#pragma once

#include <stddef.h>
#include "wled_c_fx.h"

#ifdef WLEDC_ENABLE_2D

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 2D effect table accessors.
 *
 * The table is indexed by (wledc_fx_id_t slot - 114), i.e. local slot
 * 0 corresponds to FX_MODE_2DPLASMAROTOZOOM (114) and local slot 72
 * to FX_MODE_2DAKEMI (186). Unselected slots are NULL. fx_run falls
 * back to render_black for out-of-bounds/NULL slots.
 */
const wledc_fx_fn *wledc_fx_2d_table(void);

/* Number of valid slots in the table (including NULL slots, currently 73). */
size_t wledc_fx_2d_count(void);

#ifdef __cplusplus
}
#endif

#endif /* WLEDC_ENABLE_2D */