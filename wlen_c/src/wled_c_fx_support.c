/**
 * @file wled_c_fx_support.c
 * @brief Integer-only effect support math (see wled_c_fx_support.h).
 *
 * All algorithms are self-written rewrites that keep the observable
 * semantics of the WLED/FastLED reference implementations:
 *   - sin16: Bhaskara I integer approximation;
 *   - beat88: ((now - timebase) * bpm88 * 280) >> 16 (uint32 math,
 *     wrap-around safe by unsigned subtraction);
 *   - beatsin*: beat -> sin -> scale -> offset;
 *   - random*: 16-bit LCG (x * 3001 + 31683, xor-shift 7);
 *   - nblend_bytes: byte-wise palette morphing with a change limit.
 */
#include "wled_c/wled_c_fx_support.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Sine / cosine                                                       */
/* ------------------------------------------------------------------ */

int16_t wledc_sin16(uint16_t theta)
{
    int scale = 1;
    if (theta > 0x7FFF) {
        theta = 0xFFFF - theta;   /* second half of the circle */
        scale = -1;
    }
    uint32_t precal = theta * (0x7FFFu - theta);
    uint64_t numerator = (uint64_t)precal * (4u * 0x7FFFu);  /* 64-bit required */
    int32_t denominator = 1342095361 - (int32_t)precal;     /* 5*0x7FFF^2/4 */
    int16_t result = (int16_t)(numerator / (uint64_t)denominator);
    return (int16_t)(result * scale);
}

int16_t wledc_cos16(uint16_t theta)
{
    return wledc_sin16((uint16_t)(theta + 0x4000u)); /* cos(x) = sin(x+pi/2) */
}

uint8_t wledc_sin8(uint8_t theta)
{
    int32_t sin16 = wledc_sin16((uint16_t)theta * 257u); /* 255*257 = 0xFFFF */
    sin16 += 0x7FFF + 128; /* shift to 0..0xFFFF, +128 for rounding */
    if (sin16 > 0xFFFF) {
        sin16 = 0xFFFF;     /* saturation prevents overflow */
    }
    return (uint8_t)(sin16 >> 8);
}

uint8_t wledc_cos8(uint8_t theta)
{
    return wledc_sin8((uint8_t)(theta + 64u)); /* cos(x) = sin(x+pi/2) */
}

/* ------------------------------------------------------------------ */
/* Scaling helpers                                                     */
/* ------------------------------------------------------------------ */

uint8_t wledc_scale8(uint8_t i, uint8_t scale)
{
    return (uint8_t)(((uint16_t)i * (uint16_t)(1u + scale)) >> 8);
}

uint8_t wledc_scale8_video(uint8_t i, uint8_t scale)
{
    uint8_t j = (uint8_t)((((int)i * (int)scale) >> 8) + ((i && scale) ? 1 : 0));
    return j;
}

uint16_t wledc_scale16(uint16_t i, uint16_t scale)
{
    /* i * (1 + scale) fits 32 bits: 65535 * 65536 < 2^32 */
    return (uint16_t)(((uint32_t)i * (uint32_t)(1u + scale)) >> 16);
}

uint32_t wledc_scale16_by16(uint32_t i, uint16_t scale)
{
    /* 64-bit intermediate required for 32-bit values */
    return (uint32_t)(((uint64_t)i * (uint64_t)(1u + scale)) >> 16);
}

/* ------------------------------------------------------------------ */
/* Easing / wave generators                                            */
/* ------------------------------------------------------------------ */

uint8_t wledc_ease8_in_out_quad(uint8_t i)
{
    uint32_t j = i;
    if (j & 0x80u) {
        j = 255u - j;
    }
    uint32_t jj = (j * j) >> 7;
    return (uint8_t)((i & 0x80u) ? (255u - jj) : jj);
}

uint8_t wledc_ease8_in_out_cubic(uint8_t i)
{
    /* S-curve: 3x^2 - 2x^3 = x^2 * (3 - 2x) */
    uint32_t ii = ((uint32_t)i * i);
    uint32_t factor = (3u << 8) - ((uint32_t)i << 1);
    return (uint8_t)((ii * factor) >> 16);
}

uint16_t wledc_ease16_in_out_cubic(uint16_t i)
{
    uint32_t ii = ((uint32_t)i * i) >> 16;
    uint32_t factor = (3u << 16) - ((uint32_t)i << 1);
    return (uint16_t)((ii * factor) >> 16);
}

uint8_t wledc_triwave8(uint8_t in)
{
    if (in & 0x80u) {
        in = (uint8_t)(255u - in);
    }
    return (uint8_t)(in << 1);
}

uint16_t wledc_triwave16(uint16_t in)
{
    if (in < 0x8000u) {
        return (uint16_t)(in * 2u);
    }
    return (uint16_t)(0xFFFFu - (in - 0x8000u) * 2u);
}

uint8_t wledc_quadwave8(uint8_t in)
{
    return wledc_ease8_in_out_quad(wledc_triwave8(in));
}

uint8_t wledc_cubicwave8(uint8_t in)
{
    return wledc_ease8_in_out_cubic(wledc_triwave8(in));
}

uint16_t wledc_oscillate(uint16_t value, uint16_t low, uint16_t high,
                        uint16_t step, bool *up)
{
    if (high <= low || step == 0) {
        return value;
    }

    /* clamp the starting position into the range */
    if (value < low) {
        value = low;
    } else if (value > high) {
        value = high;
    }

    if (up == NULL) {
        /* direction unknown: pick "up" deterministically */
        if (value == high) {
            return (uint16_t)(value > low + step ? value - step : low);
        }
        return (uint16_t)(value + step <= high ? value + step : high);
    }

    if (*up) {
        if ((uint32_t)value + step >= high) {
            *up = false;
            return high;
        }
        return (uint16_t)(value + step);
    }
    if (value <= (uint32_t)low + step) {
        *up = true;
        return low;
    }
    return (uint16_t)(value - step);
}

/* ------------------------------------------------------------------ */
/* Beat / beatsin family                                               */
/* ------------------------------------------------------------------ */

uint16_t wledc_beat88(uint16_t bpm88, wledc_time_ms_t now,
                      wledc_time_ms_t timebase)
{
    /* unsigned subtraction is wrap-around safe;
     * uint32 math on purpose: matches reference periodic behavior */
    return (uint16_t)((((now - timebase) * bpm88) * 280u) >> 16);
}

uint16_t wledc_beat16(uint16_t bpm, wledc_time_ms_t now,
                      wledc_time_ms_t timebase)
{
    if (bpm < 256u) {
        return wledc_beat88((uint16_t)(bpm << 8), now, timebase);
    }
    return wledc_beat88(bpm, now, timebase);
}

uint8_t wledc_beat8(uint16_t bpm, wledc_time_ms_t now,
                    wledc_time_ms_t timebase)
{
    return (uint8_t)(wledc_beat16(bpm, now, timebase) >> 8);
}

uint16_t wledc_beatsin88(uint16_t bpm88, uint16_t lowest,
                         uint16_t highest, wledc_time_ms_t now,
                         wledc_time_ms_t timebase,
                         uint16_t phase_offset)
{
    uint16_t beat = wledc_beat88(bpm88, now, timebase);
    uint16_t beatsin = (uint16_t)(wledc_sin16((uint16_t)(beat + phase_offset)) + 32768);
    uint16_t rangewidth = (uint16_t)(highest - lowest);
    uint16_t scaledbeat = wledc_scale16(beatsin, rangewidth);
    return (uint16_t)(lowest + scaledbeat);
}

uint16_t wledc_beatsin16(uint16_t bpm, uint16_t lowest,
                         uint16_t highest, wledc_time_ms_t now,
                         wledc_time_ms_t timebase,
                         uint16_t phase_offset)
{
    uint16_t beat = wledc_beat16(bpm, now, timebase);
    uint16_t beatsin = (uint16_t)(wledc_sin16((uint16_t)(beat + phase_offset)) + 32768);
    uint16_t rangewidth = (uint16_t)(highest - lowest);
    uint16_t scaledbeat = wledc_scale16(beatsin, rangewidth);
    return (uint16_t)(lowest + scaledbeat);
}

uint8_t wledc_beatsin8(uint16_t bpm, uint8_t lowest, uint8_t highest,
                       wledc_time_ms_t now, wledc_time_ms_t timebase,
                       uint16_t phase_offset)
{
    uint8_t beat = wledc_beat8(bpm, now, timebase);
    uint8_t beatsin = wledc_sin8((uint8_t)(beat + phase_offset));
    uint8_t rangewidth = (uint8_t)(highest - lowest);
    uint8_t scaledbeat = wledc_scale8(beatsin, rangewidth);
    return (uint8_t)(lowest + scaledbeat);
}

/* ------------------------------------------------------------------ */
/* PRNG (WLED prng.h semantics)                                        */
/* ------------------------------------------------------------------ */

static uint16_t s_prng_seed = 0x1234u;

void wledc_random_set_seed(uint16_t seed)
{
    s_prng_seed = seed;
}

uint16_t wledc_random16(void)
{
    s_prng_seed = (uint16_t)(s_prng_seed * 3001u + 31683u);
    s_prng_seed ^= (uint16_t)(s_prng_seed >> 7);
    return s_prng_seed;
}

uint16_t wledc_random16_lim(uint16_t lim)
{
    return (uint16_t)(((uint32_t)wledc_random16() * lim) >> 16);
}

uint8_t wledc_random8(void)
{
    return (uint8_t)wledc_random16();
}

uint8_t wledc_random8_lim(uint8_t lim)
{
    return (uint8_t)(((uint16_t)wledc_random8() * lim) >> 8);
}

uint16_t wledc_random16_between(uint16_t min, uint16_t max)
{
    if (max <= min) {
        return min;
    }
    return (uint16_t)(min + wledc_random16_lim((uint16_t)(max - min + 1u)));
}

uint8_t wledc_random8_between(uint8_t min, uint8_t max)
{
    if (max <= min) {
        return min;
    }
    return (uint8_t)(min + wledc_random8_lim((uint8_t)(max - min + 1u)));
}

/* ------------------------------------------------------------------ */
/* Palette blend (nblendPaletteTowardPalette semantics)                 */
/* ------------------------------------------------------------------ */

bool wledc_nblend_bytes(uint8_t *a, const uint8_t *b, unsigned len,
                        unsigned max_changes)
{
    if (a == NULL || b == NULL || len == 0) {
        return true;
    }
    unsigned changes = 0;
    for (unsigned i = 0; i < len; ++i) {
        if (a[i] == b[i]) {
            continue;
        }
        if (changes >= max_changes) {
            break;
        }
        if (a[i] < b[i]) {
            ++a[i];
            ++changes;
        }
        if (a[i] > b[i]) {
            --a[i];
            ++changes;
            if (a[i] > b[i]) {
                --a[i];
            }
        }
        if (changes >= max_changes) {
            break;
        }
    }
    return memcmp(a, b, len) == 0;
}