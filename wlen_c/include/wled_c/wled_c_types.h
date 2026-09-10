/**
 * @file wled_c_types.h
 * @brief Common types, color packing macros and constants for the
 *        wled_c LED control component.
 *
 * This header is self-contained: it only depends on <stdint.h> and the
 * ESP-IDF error header, so it can be included from any translation unit
 * (including user application code) without pulling in driver headers.
 *
 * The color packing scheme reproduces WLED's colors.h conventions:
 * a 32-bit color is packed as 0xWWRRGGBB.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Component version                                                   */
/* ------------------------------------------------------------------ */

#define WLEDC_VERSION_MAJOR 0
#define WLEDC_VERSION_MINOR 1
#define WLEDC_VERSION_PATCH 0

/** Component version accessors (implemented in src/wled_c_version.c). */
int wledc_version_major(void);
int wledc_version_minor(void);
int wledc_version_patch(void);

/* ------------------------------------------------------------------ */
/* Engine constants (configurable via Kconfig)                         */
/* ------------------------------------------------------------------ */

/** Number of color layers per segment: primary, secondary, tertiary. */
#define WLEDC_NUM_COLORS 3

#ifdef CONFIG_WLEDC_FPS
  #define WLEDC_FPS CONFIG_WLEDC_FPS
#else
  #define WLEDC_FPS 42
#endif

/** Frame time in milliseconds, truncated (WLED: FRAMETIME = 1000/FPS).
 *  The refresh task converts this to FreeRTOS ticks via pdMS_TO_TICKS. */
#define WLEDC_FRAME_TIME_MS (1000 / WLEDC_FPS)

#ifdef CONFIG_WLEDC_MAX_SEGMENTS
  #define WLEDC_MAX_SEGMENTS CONFIG_WLEDC_MAX_SEGMENTS
#else
  #define WLEDC_MAX_SEGMENTS 32
#endif

#ifdef CONFIG_WLEDC_MAX_BUSES
  #define WLEDC_MAX_BUSES CONFIG_WLEDC_MAX_BUSES
#else
  #define WLEDC_MAX_BUSES 4
#endif

#ifdef CONFIG_WLEDC_MAX_LEDS_PER_BUS
  #define WLEDC_MAX_LEDS_PER_BUS CONFIG_WLEDC_MAX_LEDS_PER_BUS
#else
  #define WLEDC_MAX_LEDS_PER_BUS 512
#endif

/** Scale factor used for fractional speeds (WLED: SEGMENT.speed 0-255). */
#define WLEDC_SPEED_MAX 255

/** Maximum 16-bit value used for transition progress (0-65535). */
#define WLEDC_TRANSITION_MAX 65535

/* ------------------------------------------------------------------ */
/* 32-bit color packing: 0xWWRRGGBB                                    */
/* ------------------------------------------------------------------ */

/** Pack 8-bit channels into a 32-bit color (channel order 0xWWRRGGBB). */
#define WLEDC_RGBW32(r, g, b, w) \
    ( (((uint32_t)(w) & 0xFF) << 24) \
    | (((uint32_t)(r) & 0xFF) << 16) \
    | (((uint32_t)(g) & 0xFF) << 8)  \
    | ( (uint32_t)(b) & 0xFF) )

/** Pack 8-bit RGB channels (white = 0). */
#define WLEDC_RGB32(r, g, b) WLEDC_RGBW32((r), (g), (b), 0)

/** Extract the white channel. */
#define WLEDC_W(c) ((uint8_t)(((c) >> 24) & 0xFF))
/** Extract the red channel. */
#define WLEDC_R(c) ((uint8_t)(((c) >> 16) & 0xFF))
/** Extract the green channel. */
#define WLEDC_G(c) ((uint8_t)(((c) >> 8) & 0xFF))
/** Extract the blue channel. */
#define WLEDC_B(c) ((uint8_t)((c) & 0xFF))

/** Commonly used black. */
#define WLEDC_BLACK WLEDC_RGBW32(0, 0, 0, 0)
/** White with the white channel set (for RGBW strips). */
#define WLEDC_WHITE_W WLEDC_RGBW32(0, 0, 0, 255)
/** White on the RGB channels. */
#define WLEDC_WHITE_RGB WLEDC_RGBW32(255, 255, 255, 0)

/* ------------------------------------------------------------------ */
/* Segment option bits — aligned with WLED FX.h (Segment::options)      */
/* ------------------------------------------------------------------ */

#define SEG_OPT_SELECTED   0x0001  /**< Segment is selected (UI focus). */
#define SEG_OPT_REVERSE    0x0002  /**< Mirror along X. */
#define SEG_OPT_ON         0x0004  /**< Segment is active. */
#define SEG_OPT_MIRROR     0x0008  /**< Mirror along X (duplicate half). */
#define SEG_OPT_FREEZE     0x0010  /**< Freeze effect engine for segment. */
#define SEG_OPT_RESET      0x0020  /**< Reset runtime state next frame. */
#define SEG_OPT_REVERSE_Y  0x0040  /**< Mirror along Y (2D only). */
#define SEG_OPT_MIRROR_Y   0x0080  /**< Mirror along Y (2D only). */
#define SEG_OPT_TRANSPOSE  0x0100  /**< Swap X and Y axes (2D only). */

/* ------------------------------------------------------------------ */
/* Error codes (esp_err_t compatible, custom range 0x8000+)            */
/* ------------------------------------------------------------------ */

/**
 * wled_c specific error codes. Values live in the component-specific
 * range above ESP_ERR_INVALID_STATE so they never collide with IDF's
 * core errors. Reuse IDF core errors (ESP_ERR_INVALID_ARG,
 * ESP_ERR_NO_MEM, ...) where the semantics match.
 */
enum {
    WLEDC_ERR_NO_SEGMENT = 0x8001,  /**< Segment not found / no free slot. */
    WLEDC_ERR_NO_BUS     = 0x8002,  /**< Bus not found / no free slot / no
                                          free RMT TX channel. */
    WLEDC_ERR_NO_FX      = 0x8003,  /**< Invalid or unimplemented effect id. */
    WLEDC_ERR_NO_PALETTE = 0x8004,  /**< Invalid palette id. */
    WLEDC_ERR_NOT_INIT   = 0x8005,  /**< Component not started yet. */
    WLEDC_ERR_NOT_2D     = 0x8006,  /**< Operation needs 2D support
                                         (WLEDC_ENABLE_2D off). */
};

/* ------------------------------------------------------------------ */
/* Basic handle / forward types shared across modules                  */
/* ------------------------------------------------------------------ */

/** Opaque handle to the component instance (created by wledc_init). */
typedef struct wledc_s *wledc_handle_t;

/** Segment index type; WLEDC_MAX_SEGMENTS bounds valid values. */
typedef uint8_t wledc_segment_id_t;

/** Bus index type; WLEDC_MAX_BUSES bounds valid values. */
typedef uint8_t wledc_bus_id_t;

/** Effect id type; WLEDC_FX_COUNT bounds valid values. */
typedef uint8_t wledc_fx_id_t;

/** Palette id type. */
typedef uint8_t wledc_palette_id_t;

/** Timebase for effect engine, in milliseconds since start. */
typedef uint32_t wledc_time_ms_t;

/* ------------------------------------------------------------------ */
/* Color order definitions (used by the bus layer)                     */
/* ------------------------------------------------------------------ */

/**
 * Logical channel remapping applied when a bus transmits a pixel.
 * Mirrors WLED's COL_ORDER_* constants.
 */
typedef enum {
    WLEDC_ORDER_GRB = 0,  /**< WS2812 family default. */
    WLEDC_ORDER_RGB = 1,
    WLEDC_ORDER_BRG = 2,
    WLEDC_ORDER_RBG = 3,
    WLEDC_ORDER_GBR = 4,
    WLEDC_ORDER_BGR = 5,
    WLEDC_ORDER_MAX,
} wledc_color_order_t;

/** Pixel color format of a bus. */
typedef enum {
    WLEDC_FMT_INVALID = 0,
    WLEDC_FMT_RGB    = 1,  /**< 3 bytes per pixel. */
    WLEDC_FMT_GRB    = 2,  /**< Alias of RGB order GRB, 3 bytes/pixel. */
    WLEDC_FMT_RGBW   = 4,  /**< 4 bytes per pixel. */
} wledc_pixel_format_t;

#ifdef __cplusplus
}
#endif