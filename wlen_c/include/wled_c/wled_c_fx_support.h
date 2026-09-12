/**
 * @file wled_c_fx_support.h
 * @brief Effect support math utilities (integer only).
 *
 * Pure-C reimplementation of the WLED / FastLED effect support math
 * used by effect functions (tasks 9/10). All functions are integer
 * only (no float, no libm) so they are safe for the RMT-driven
 * rendering path on any ESP target.
 *
 * Reference semantics (self-written, aligned with WLED behavior):
 *   - wledc_sin16: Bhaskara I sine approximation, 16-bit angle in,
 *     signed 16-bit out (-32767..32767);
 *   - wledc_sin8: 8-bit angle, 8-bit unsigned out (0..255);
 *   - beat/beatsin family: BPM-driven oscillators parameterized by
 *     an injected "now" (wledc_time_ms_t) instead of millis();
 *   - wledc_oscillate: generic triangle-wave back-and-forth helper.
 *
 * None of these functions read global state; they are pure and
 * therefore unit-test friendly.
 */
#ifndef WLED_C_FX_SUPPORT_H
#define WLED_C_FX_SUPPORT_H

#include <stdint.h>
#include <stdbool.h>

#include "wled_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Sine / cosine (Bhaskara I integer approximation)                    */
/* ------------------------------------------------------------------ */

/**
 * @brief 16-bit sine, input 0-65535 maps to full circle.
 * @param theta angle (0..65535 = 0..2*PI)
 * @return sine scaled to signed 16-bit (-32767..32767)
 */
int16_t wledc_sin16(uint16_t theta);

/**
 * @brief 16-bit cosine (sin shifted by a quarter turn).
 * @param theta angle (0..65535)
 * @return cosine scaled to signed 16-bit
 */
int16_t wledc_cos16(uint16_t theta);

/**
 * @brief 8-bit sine, input 0-255 maps to full circle.
 * @param theta angle (0..255)
 * @return sine scaled to 0..255
 */
uint8_t wledc_sin8(uint8_t theta);

/**
 * @brief 8-bit cosine (sin shifted by 64).
 * @param theta angle (0..255)
 * @return cosine scaled to 0..255
 */
uint8_t wledc_cos8(uint8_t theta);

/* ------------------------------------------------------------------ */
/* Scaling helpers                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Scale one byte by a second byte (i * scale / 255).
 */
uint8_t wledc_scale8(uint8_t i, uint8_t scale);

/**
 * @brief Scale one byte by a second byte, saturating; with the
 *        WLED/FastLED guarantee that i == 255 keeps 255.
 */
uint8_t wledc_scale8_video(uint8_t i, uint8_t scale);

/**
 * @brief Scale an unsigned 16-bit value by an unsigned 16-bit
 *        fraction (i * scale / 65535).
 */
uint16_t wledc_scale16(uint16_t i, uint16_t scale);

/**
 * @brief Scale an unsigned 32-bit value by an unsigned 16-bit
 *        fraction (i * scale / 65535), 64-bit intermediate.
 */
uint32_t wledc_scale16_by16(uint32_t i, uint16_t scale);

/* ------------------------------------------------------------------ */
/* Easing / wave generators                                            */
/* ------------------------------------------------------------------ */

/** Quadratic ease-in-out on 0..255 input. */
uint8_t wledc_ease8_in_out_quad(uint8_t i);

/** Cubic ease-in-out on 0..255 input. */
uint8_t wledc_ease8_in_out_cubic(uint8_t i);

/** Cubic ease-in-out on 0..65535 input. */
uint16_t wledc_ease16_in_out_cubic(uint16_t i);

/** Triangle wave, 8-bit input/output (ramp up then down). */
uint8_t wledc_triwave8(uint8_t in);

/** Triangle wave, 16-bit input/output. */
uint16_t wledc_triwave16(uint16_t in);

/** Quadratic-eased triangle wave (8-bit). */
uint8_t wledc_quadwave8(uint8_t in);

/** Cubic-eased triangle wave (8-bit). */
uint8_t wledc_cubicwave8(uint8_t in);

/**
 * @brief Generic back-and-forth oscillator (triangle walk).
 *
 * Moves @p value by @p step each call, bouncing at @p low / @p high.
 * Semantics aligned with WLED oscillator effects: the returned value
 * is the new position after one step.
 *
 * @param value current position (clamped into [low, high] first)
 * @param low   lower turning point
 * @param high  upper turning point (must be > low)
 * @param step  increment per call (> 0)
 * @param up    current direction; set to true when moving up. The
 *              variable is updated by the caller (pass &dir).
 */
uint16_t wledc_oscillate(uint16_t value, uint16_t low, uint16_t high,
                         uint16_t step, bool *up);

/* ------------------------------------------------------------------ */
/* Beat / beatsin family (BPM driven, time injected)                   */
/* ------------------------------------------------------------------ */

/**
 * @brief 88/256 BPM 16-bit beat accumulator.
 *
 * @param bpm88   BPM in 88/256 beats-per-minute units (BPM * 256 / 88)
 * @param now     current time in milliseconds (injected)
 * @param timebase epoch when the beat started (milliseconds)
 * @return beat position 0..65535
 */
uint16_t wledc_beat88(uint16_t bpm88, wledc_time_ms_t now,
                      wledc_time_ms_t timebase);

/**
 * @brief 16-bit beat accumulator.
 *
 * For BPM < 256 the BPM value is scaled into the beat88 domain;
 * otherwise it is used directly.
 */
uint16_t wledc_beat16(uint16_t bpm, wledc_time_ms_t now,
                      wledc_time_ms_t timebase);

/** 8-bit beat accumulator (beat16 >> 8). */
uint8_t wledc_beat8(uint16_t bpm, wledc_time_ms_t now,
                    wledc_time_ms_t timebase);

/**
 * @brief Sine wave at a given BPM in 88/256 units, within a range.
 * @return lowest + (0..rangewidth) sine scaled
 */
uint16_t wledc_beatsin88(uint16_t bpm88, uint16_t lowest,
                         uint16_t highest, wledc_time_ms_t now,
                         wledc_time_ms_t timebase,
                         uint16_t phase_offset);

/** Sine wave at a given BPM (16-bit beat domain). */
uint16_t wledc_beatsin16(uint16_t bpm, uint16_t lowest,
                         uint16_t highest, wledc_time_ms_t now,
                         wledc_time_ms_t timebase,
                         uint16_t phase_offset);

/** Sine wave at a given BPM (8-bit, 0..255 angle domain). */
uint8_t wledc_beatsin8(uint16_t bpm, uint8_t lowest, uint8_t highest,
                       wledc_time_ms_t now, wledc_time_ms_t timebase,
                       uint16_t phase_offset);

/* ------------------------------------------------------------------ */
/* Pseudo random number generator (WLED prng.h semantics)              */
/* ------------------------------------------------------------------ */

/**
 * @brief Seed the effect PRNG (deterministic tests / first-boot).
 *
 * WLED default seed: 0x1234.
 */
void wledc_random_set_seed(uint16_t seed);

/**
 * @brief Full-range 8-bit random value.
 */
uint8_t wledc_random8(void);

/**
 * @brief Random byte in [0, lim).
 */
uint8_t wledc_random8_lim(uint8_t lim);

/**
 * @brief Full-range 16-bit random value.
 */
uint16_t wledc_random16(void);

/**
 * @brief Random 16-bit value in [0, lim).
 */
uint16_t wledc_random16_lim(uint16_t lim);

/**
 * @brief Random 16-bit value in [min, max] (inclusive).
 */
uint16_t wledc_random16_between(uint16_t min, uint16_t max);

/**
 * @brief Random byte in [min, max] (inclusive).
 */
uint8_t wledc_random8_between(uint8_t min, uint8_t max);

/* ------------------------------------------------------------------ */
/* Palette-blend helper (random palette morphing)                      */
/* ------------------------------------------------------------------ */

/**
 * @brief Blend two 48-byte palette entries arrays byte by byte,
 *        moving each channel of a towards b by at most
 *        @p max_changes channels per call (WLED nblend semantics:
 *        p1 < p2 -> ++, p1 > p2 -> --, stop after max_changes).
 *
 * @param a         current entries (modified in place)
 * @param b         target entries
 * @param len       bytes to blend (use WLEDC_PALETTE_ENTRIES_BYTES)
 * @param max_changes maximum number of channels adjusted per call
 * @return true if a fully equals b afterwards
 */
bool wledc_nblend_bytes(uint8_t *a, const uint8_t *b, unsigned len,
                        unsigned max_changes);

#ifdef __cplusplus
}
#endif

#endif /* WLED_C_FX_SUPPORT_H */