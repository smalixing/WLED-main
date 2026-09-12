/*
 * WLED-C effect engine interface.
 *
 * Schedules per-segment effect functions from a static dispatch table and
 * returns the delay (ms) the caller should wait before the next run, which
 * participates in the frame-rate control (42 FPS baseline).
 *
 * Rewritten from wled00/FX_fcn.cpp + wled00/FX.h (GPL-3.0, WLED project).
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef WLED_C_FX_H
#define WLED_C_FX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wled_c_segment.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Effect function signature                                           */
/* ------------------------------------------------------------------ */

/**
 * Effect function prototype.
 *
 * Called by wledc_fx_run() with the segment to animate. The engine sets the
 * current-segment pointer and frame time before dispatching, so effect
 * implementations may use the SEGMENT / SEGENV macros and wledc_fx_now().
 *
 * @param seg  segment to animate (never NULL when dispatched by the engine)
 * @return     delay in ms before the next call (frame-rate control);
 *             0 selects the default frame time (1000 / WLEDC_FPS)
 */
typedef uint16_t (*wledc_fx_fn)(wledc_segment_t *seg);

/* ------------------------------------------------------------------ */
/* Engine control                                                      */
/* ------------------------------------------------------------------ */

/**
 * Run one animation frame for a segment.
 *
 * Order: random palette maintenance -> reset_if_required() -> safety checks
 * (mode out of range or empty table slot render a black frame) ->
 * fx_table[seg->mode](seg). The returned delay is capped to the default
 * frame time so a broken effect cannot stall the scheduler.
 *
 * @param seg  target segment (NULL-safe: returns the default frame time)
 * @param now  current time in ms (injected by the caller)
 * @return     delay in ms before the next run of this segment
 */
uint16_t wledc_fx_run(wledc_segment_t *seg, wledc_time_ms_t now);

/**
 * Frame time passed to the last wledc_fx_run() call.
 * Effect functions use this for beat/oscillation helpers that need `now`.
 */
wledc_time_ms_t wledc_fx_now(void);

/**
 * Upper bound (exclusive) of the effect id space.
 *
 * 1D effects occupy ids 0..76; when compiled with WLEDC_ENABLE_2D, 2D
 * effects additionally occupy ids 114..186 (native FX_MODE_* numbering).
 * The id space is therefore not contiguous (77..113 unused): use
 * wledc_fx_get() to test whether a specific slot is implemented.
 */
size_t wledc_fx_count(void);

/**
 * Look up an effect function by ID.
 * @return function pointer, or NULL if the slot is empty / ID out of range
 */
wledc_fx_fn wledc_fx_get(wledc_fx_id_t id);

/* ------------------------------------------------------------------ */
/* Palette resolution (dynamic IDs 0-5 intercepted here)              */
/* ------------------------------------------------------------------ */

/**
 * Resolve the effective 16-entry palette of a segment.
 *
 * Palette IDs 0-5 are dynamic (built from the segment's colors / random
 * palette state) and are constructed here; IDs >= 6 are forwarded to
 * wledc_palette_get_entries(). This is the single source of palette truth
 * for effect rendering *and* for transition palette interpolation.
 *
 * @param seg  segment whose palette ID is resolved
 * @param out  output buffer, WLEDC_PALETTE_ENTRIES_BYTES bytes (48)
 * @return true on success; false (out zeroed) if seg is NULL or the static
 *         palette lookup fails
 */
bool wledc_fx_segment_palette_entries(
    const wledc_segment_t *seg, uint8_t out[WLEDC_PALETTE_ENTRIES_BYTES]);

/**
 * Interval (seconds) between random palette regenerations for segments
 * using palette ID 1. Default mirrors WLED (5 seconds).
 */
void wledc_fx_set_random_palette_interval(uint16_t seconds);

/* ------------------------------------------------------------------ */
/* Current segment access macros (engine-internal, used by effects)   */
/* ------------------------------------------------------------------ */

/** Set by wledc_fx_run() before dispatching; not part of the public API. */
extern wledc_segment_t *_wledc_current_segment;

/** WLED-style access to the segment being animated. */
#define SEGMENT (*_wledc_current_segment)

/**
 * WLED-style access to the segment runtime state (call/step/aux/data).
 * In this port the runtime fields live in the same struct, so SEGENV is
 * an alias of SEGMENT.
 */
#define SEGENV (*_wledc_current_segment)

/** Shorthand used by most effect implementations. */
#define SEG (*_wledc_current_segment)

#ifdef __cplusplus
}
#endif

#endif /* WLED_C_FX_H */