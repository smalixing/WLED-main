/*
 * WLED-C 1D Effects Component Header
 *
 * This file is part of the WLED pure-C rewrite for ESP-IDF.
 *
 * A total of 49 1D effects were ported from the original WLED FX.cpp (wled00/)
 * implementation, with slot numbers strictly aligned with the FX_MODE_*
 * enum in wled00/FX.h. Unselected slots are NULL and rendered as black.
 *
 * Porting rules (differences from the original implementation):
 *  - Segment pixels is a physical buffer (length = stop - start, see
 *    wled_c_segment.c seg_realloc_pixels()); WLED 0.16 writes to a virtual
 *    buffer and folds REVERSE/MIRROR/grouping/spacing in the show() output
 *    stage. Since wled_c has no independent blend folding layer, the 1D
 *    three-piece helper (fx_1d_virtual_length/set_pixel/get_pixel) inlines
 *    the folding logic on write (folding order aligned with FX_fcn.cpp
 *    blendSegment L1744-1751: reverse -> grouping expansion -> mirror).
 *  - fade_out/blur/fill traverse the physical pixels directly, corresponding
 *    to the raw traversal semantics in WLED (spacing pixels stay black in
 *    both solutions).
 *  - Segment-local runtime data (dissolve/multi_comet/meteor) is applied
 *    for via wledc_segment_request_data(); failure falls back to
 *    FX_FALLBACK_STATIC (mode 0 static).
 *  - Some native WLED bugs (operator precedence in gradient_base, rain's
 *    aux1 judgment writing aux0 by mistake, etc.) are reproduced as-is to
 *    preserve visual consistency, with comments in the code.
 *  - Segment offset does not fold in 1D effects in WLED and is ignored here.
 */

#pragma once

#include <stddef.h>
#include "wled_c_fx.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 1D effect table accessors.
 *
 * The table is indexed by the wledc_fx_id_t slot number (0..76), which is
 * consistent with the FX_MODE_* numbering in wled00/FX.h. Unselected slots
 * are NULL. fx_run falls back to render_black for out-of-bounds/NULL slots.
 */
const wledc_fx_fn *wledc_fx_1d_table(void);

/* Number of valid slots in the table (including NULL slots, currently 77). */
size_t wledc_fx_1d_count(void);

#ifdef __cplusplus
}
#endif