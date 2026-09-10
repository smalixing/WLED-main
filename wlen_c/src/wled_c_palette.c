/**
 * @file wled_c_palette.c
 * @brief WLED 调色板模块实现(数据表 + 算法)
 *
 * 数据源: WLED wled00/palettes.cpp(FastLED @ 3.6.0 MIT 许可调色板 +
 * cpt-city 公共域调色板;前者施加 2.2 逆伽马,后者按 gammas(1.182,1.0,1.136)
 * 伽马校正)。全部数值与 WLED 原文逐一核对,重复位置原样保留。
 *
 * 算法源: WLED colors.cpp 的 ColorFromPalette()、fastled_slim.h 的
 * loadDynamicGradientPalette()、fastled_slim.cpp 的 fill_gradient_RGB()。
 *
 * ID 映射(与 WLED 完全一致):
 *  - 6-12  : fastled_palettes[](3 个 gc22 + 4 个 FastLED CRGB 调色板)
 *  - 13-71 : gradient_palettes[](59 个渐变停色点调色板,数组顺序为映射权威)
 *  - 72-200: custom_palettes[](动态注册,ID 从 200 向下生长)
 */

#include <string.h>

#include "wled_c/wled_c_palette.h"

/* ------------------------------------------------------------------------ */
/* gc22 调色板(16x3 字节 entries)                                           */
/* ------------------------------------------------------------------------ */

/* ID 6 - Party */
static const uint8_t party_colors_gc22[] = {
    0x9B, 0x00, 0xD5, 0xBD, 0x00, 0xB8, 0xDA, 0x00, 0x92, 0xF3, 0x00, 0x5C,
    0xF4, 0x55, 0x00, 0xDC, 0x8F, 0x00, 0xD5, 0xB4, 0x00, 0xD5, 0xD5, 0x00,
    0xD5, 0x9B, 0x00, 0xEF, 0x66, 0x00, 0xF9, 0x00, 0x44, 0xE1, 0x00, 0x86,
    0xC4, 0x00, 0xB0, 0xA3, 0x00, 0xCF, 0x76, 0x00, 0xE8, 0x00, 0x32, 0xFC
};

/* ID 11 - Rainbow */
static const uint8_t rainbow_colors_gc22[] = {
    0xFF, 0x00, 0x00, 0xEB, 0x70, 0x00, 0xD5, 0x9B, 0x00, 0xD5, 0xBA, 0x00,
    0xD5, 0xD5, 0x00, 0x9C, 0xEB, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xEB, 0x70,
    0x00, 0xD5, 0x9B, 0x00, 0x9C, 0xD4, 0x00, 0x00, 0xFF, 0x70, 0x00, 0xEB,
    0x9B, 0x00, 0xD5, 0xBA, 0x00, 0xBB, 0xD5, 0x00, 0x9B, 0xEB, 0x00, 0x72
};

/* ID 12 - Rainbow Bands */
static const uint8_t rainbow_stripe_colors_gc22[] = {
    0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0xD5, 0x9B, 0x00, 0x00, 0x00, 0x00,
    0xD5, 0xD5, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xD5, 0x9B, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
    0x9B, 0x00, 0xD5, 0x00, 0x00, 0x00, 0xD5, 0x00, 0x9B, 0x00, 0x00, 0x00
};

/* ------------------------------------------------------------------------ */
/* FastLED CRGB 调色板(按 CSS 标准色值换算为 16x3 entries)                  */
/* ------------------------------------------------------------------------ */

/* ID 7 - Cloud */
static const uint8_t cloud_colors_p[] = {
    /* Blue, DarkBlue x7, Blue, DarkBlue, SkyBlue x2, LightBlue, White,
       LightBlue, SkyBlue(CSS: 0x0000FF / 0x00008B / 0x87CEEB / 0xADD8E6) */
    0x00, 0x00, 0xFF, 0x00, 0x00, 0x8B, 0x00, 0x00, 0x8B, 0x00, 0x00, 0x8B,
    0x00, 0x00, 0x8B, 0x00, 0x00, 0x8B, 0x00, 0x00, 0x8B, 0x00, 0x00, 0x8B,
    0x00, 0x00, 0xFF, 0x00, 0x00, 0x8B, 0x87, 0xCE, 0xEB, 0x87, 0xCE, 0xEB,
    0xAD, 0xD8, 0xE6, 0xFF, 0xFF, 0xFF, 0xAD, 0xD8, 0xE6, 0x87, 0xCE, 0xEB
};

/* ID 8 - Lava */
static const uint8_t lava_colors_p[] = {
    /* Black, Maroon, Black, Maroon, DarkRed x2, Maroon, DarkRed x2, Red,
       Orange, White, Orange, Red, DarkRed(CSS: 0x000000 / 0x800000 /
       0x8B0000 / 0xFF0000 / 0xFFA500 / 0xFFFFFF) */
    0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
    0x8B, 0x00, 0x00, 0x8B, 0x00, 0x00, 0x80, 0x00, 0x00, 0x8B, 0x00, 0x00,
    0x8B, 0x00, 0x00, 0x8B, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0xA5, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xA5, 0x00, 0xFF, 0x00, 0x00, 0x8B, 0x00, 0x00
};

/* ID 9 - Ocean */
static const uint8_t ocean_colors_p[] = {
    /* MidnightBlue, DarkBlue, MidnightBlue, Navy, DarkBlue, MediumBlue,
       SeaGreen, Teal, CadetBlue, Blue, DarkCyan, CornflowerBlue, Aquamarine,
       SeaGreen, Aqua, LightSkyBlue(CSS 值: 0x191970 / 0x00008B / 0x000080 /
       0x0000CD / 0x2E8B57 / 0x008080 / 0x5F9EA0 / 0x008B8B / 0x6495ED /
       0x7FFFD4 / 0x00FFFF / 0x87CEFA) */
    0x19, 0x19, 0x70, 0x00, 0x00, 0x8B, 0x19, 0x19, 0x70, 0x00, 0x00, 0x80,
    0x00, 0x00, 0x8B, 0x00, 0x00, 0xCD, 0x2E, 0x8B, 0x57, 0x00, 0x80, 0x80,
    0x5F, 0x9E, 0xA0, 0x00, 0x00, 0xFF, 0x00, 0x8B, 0x8B, 0x64, 0x95, 0xED,
    0x7F, 0xFF, 0xD4, 0x2E, 0x8B, 0x57, 0x00, 0xFF, 0xFF, 0x87, 0xCE, 0xFA
};

/* ID 10 - Forest */
static const uint8_t forest_colors_p[] = {
    /* DarkGreen x2, DarkOliveGreen, DarkGreen, Green, ForestGreen, OliveDrab,
       Green, SeaGreen, MediumAquamarine, LimeGreen, YellowGreen, LightGreen,
       LawnGreen, MediumAquamarine, ForestGreen(CSS 值: 0x006400 / 0x556B2F /
       0x008000 / 0x228B22 / 0x6B8E23 / 0x2E8B57 / 0x66CDAA / 0x32CD32 /
       0x9ACD32 / 0x90EE90 / 0x7CFC00) */
    0x00, 0x64, 0x00, 0x00, 0x64, 0x00, 0x55, 0x6B, 0x2F, 0x00, 0x64, 0x00,
    0x00, 0x80, 0x00, 0x22, 0x8B, 0x22, 0x6B, 0x8E, 0x23, 0x00, 0x80, 0x00,
    0x2E, 0x8B, 0x57, 0x66, 0xCD, 0xAA, 0x32, 0xCD, 0x32, 0x9A, 0xCD, 0x50,
    0x90, 0xEE, 0x90, 0x7C, 0xFC, 0x00, 0x66, 0xCD, 0xAA, 0x22, 0x8B, 0x22
};

/* ------------------------------------------------------------------------ */
/* FastLED/gc22 索引表(ID 6-12,顺序与 WLED fastledPalettes[] 一致)         */
/* ------------------------------------------------------------------------ */

static const uint8_t *const fastled_palettes[WLEDC_PALETTE_FASTLED_COUNT] = {
    party_colors_gc22,        /* ID 6  - Party */
    cloud_colors_p,           /* ID 7  - Cloud */
    lava_colors_p,            /* ID 8  - Lava */
    ocean_colors_p,           /* ID 9  - Ocean */
    forest_colors_p,          /* ID 10 - Forest */
    rainbow_colors_gc22,      /* ID 11 - Rainbow */
    rainbow_stripe_colors_gc22 /* ID 12 - Rainbow Bands */
};

/* ------------------------------------------------------------------------ */
/* 渐变调色板(4 字节/项: 位置,R,G,B;首项位置 0、末项位置 255)               */
/* ------------------------------------------------------------------------ */

/* ID 13 - Sunset */
static const uint8_t sunset_real_gp[] = {
    0, 181, 0, 0,  22, 218, 85, 0,  51, 255, 170, 0,  85, 211, 85, 77,
    135, 167, 0, 169,  198, 73, 0, 188,  255, 0, 0, 207
};

/* ID 14 - Rivendell */
static const uint8_t es_rivendell_15_gp[] = {
    0, 24, 69, 44,  101, 73, 105, 70,  165, 129, 140, 97,
    242, 200, 204, 166,  255, 200, 204, 166
};

/* ID 15 - Breeze */
static const uint8_t es_ocean_breeze_036_gp[] = {
    0, 16, 48, 51,  89, 27, 166, 175,  153, 197, 233, 255,  255, 0, 145, 152
};

/* ID 16 - Red & Blue */
static const uint8_t rgi_15_gp[] = {
    0, 41, 14, 99,  31, 128, 24, 74,  63, 227, 34, 50,  95, 132, 31, 76,
    127, 47, 29, 102,  159, 109, 47, 101,  191, 176, 66, 100,
    223, 129, 57, 104,  255, 84, 48, 108
};

/* ID 17 - Yellowout */
static const uint8_t retro2_16_gp[] = {
    0, 222, 191, 8,  255, 117, 52, 1
};

/* ID 18 - Analogous */
static const uint8_t analogous_1_gp[] = {
    0, 38, 0, 255,  63, 86, 0, 255,  127, 139, 0, 255,
    191, 196, 0, 117,  255, 255, 0, 0
};

/* ID 19 - Splash */
static const uint8_t es_pinksplash_08_gp[] = {
    0, 186, 63, 255,  127, 227, 9, 85,  175, 234, 205, 213,
    221, 205, 38, 176,  255, 205, 38, 176
};

/* ID 20 - Pastel */
static const uint8_t sunset_yellow_gp[] = {
    0, 61, 135, 184,  36, 129, 188, 169,  87, 203, 241, 155,
    100, 228, 237, 141,  107, 255, 232, 127,  115, 251, 202, 130,
    120, 248, 172, 133,  128, 251, 202, 130,  180, 255, 232, 127,
    223, 255, 242, 120,  255, 255, 252, 113
};

/* ID 21 - Sunset 2 (WLED 原文 68 位置重复,原样保留) */
static const uint8_t another_sunset_gp[] = {
    0, 175, 121, 62,  29, 128, 103, 60,  68, 84, 84, 58,  68, 248, 184, 55,
    97, 239, 204, 93,  124, 230, 225, 133,  178, 102, 125, 129,  255, 0, 26, 125
};

/* ID 22 - Beech (WLED 原文 28/136 位置重复,原样保留) */
static const uint8_t beech_gp[] = {
    0, 255, 254, 236,  12, 255, 254, 236,  22, 255, 254, 236,
    26, 223, 224, 178,  28, 192, 195, 124,  28, 176, 255, 231,
    50, 123, 251, 236,  71, 74, 246, 241,  93, 33, 225, 228,
    120, 0, 204, 215,  133, 4, 168, 178,  136, 10, 132, 143,
    136, 51, 189, 212,  208, 23, 159, 201,  255, 0, 129, 190
};

/* ID 23 - Vintage */
static const uint8_t es_vintage_01_gp[] = {
    0, 41, 18, 24,  51, 73, 0, 22,  76, 165, 170, 38,  101, 255, 189, 80,
    127, 139, 56, 40,  153, 73, 0, 22,  229, 41, 18, 24,  255, 41, 18, 24
};

/* ID 24 - Departure */
static const uint8_t departure_gp[] = {
    0, 53, 34, 0,  42, 86, 51, 0,  63, 147, 108, 49,  84, 212, 166, 108,
    106, 235, 212, 180,  116, 255, 255, 255,  138, 191, 255, 193,
    148, 84, 255, 88,  170, 0, 255, 0,  191, 0, 192, 0,  212, 0, 128, 0,
    255, 0, 128, 0
};

/* ID 25 - Landscape */
static const uint8_t es_landscape_64_gp[] = {
    0, 0, 0, 0,  37, 31, 89, 19,  76, 72, 178, 43,  127, 150, 235, 5,
    128, 186, 234, 119,  130, 222, 233, 252,  153, 197, 219, 231,
    204, 132, 179, 253,  255, 28, 107, 225
};

/* ID 26 - Beach (WLED 名称表错位: 名称 22"Beach"/26"Beech" 与数据表互换,
   ID→数据映射以 gGradientPalettes 数组顺序为权威,此处保持 WLED 行为) */
static const uint8_t es_landscape_33_gp[] = {
    0, 12, 45, 0,  19, 101, 86, 2,  38, 207, 128, 4,  63, 243, 197, 18,
    66, 109, 196, 146,  255, 5, 39, 7
};

/* ID 27 - Sherbet */
static const uint8_t rainbowsherbet_gp[] = {
    0, 255, 102, 41,  43, 255, 140, 90,  86, 255, 51, 90,  127, 255, 153, 169,
    170, 255, 255, 249,  209, 113, 255, 85,  255, 157, 255, 137
};

/* ID 28 - Hult */
static const uint8_t gr65_hult_gp[] = {
    0, 251, 216, 252,  48, 255, 192, 255,  89, 239, 95, 241,
    160, 51, 153, 217,  216, 24, 184, 174,  255, 24, 184, 174
};

/* ID 29 - Hult 64 */
static const uint8_t gr64_hult_gp[] = {
    0, 24, 184, 174,  66, 8, 162, 150,  104, 124, 137, 7,  130, 178, 186, 22,
    150, 124, 137, 7,  201, 6, 156, 144,  239, 0, 128, 117,  255, 0, 128, 117
};

/* ID 30 - Drywet */
static const uint8_t gmt_drywet_gp[] = {
    0, 119, 97, 33,  42, 235, 199, 88,  84, 169, 238, 124,  127, 37, 238, 232,
    170, 7, 120, 236,  212, 27, 1, 175,  255, 4, 51, 101
};

/* ID 31 - Jul */
static const uint8_t ib_jul01_gp[] = {
    0, 226, 6, 12,  94, 26, 96, 78,  132, 130, 189, 94,  255, 177, 3, 9
};

/* ID 32 - Grintage */
static const uint8_t es_vintage_57_gp[] = {
    0, 29, 8, 3,  53, 76, 1, 0,  104, 142, 96, 28,  153, 211, 191, 61,
    255, 117, 129, 42
};

/* ID 33 - Rewhi */
static const uint8_t ib15_gp[] = {
    0, 177, 160, 199,  72, 205, 158, 149,  89, 233, 155, 101,  107, 255, 95, 63,
    141, 192, 98, 109,  255, 132, 101, 159
};

/* ID 34 - Tertiary */
static const uint8_t tertiary_01_gp[] = {
    0, 0, 25, 255,  63, 38, 140, 117,  127, 86, 255, 0,
    191, 167, 140, 19,  255, 255, 25, 41
};

/* ID 35 - Fire */
static const uint8_t lava_gp[] = {
    0, 0, 0, 0,  46, 77, 0, 0,  96, 177, 0, 0,  108, 196, 38, 9,
    119, 215, 76, 19,  146, 235, 115, 29,  174, 255, 153, 41,
    188, 255, 178, 41,  202, 255, 204, 41,  218, 255, 230, 41,
    234, 255, 255, 41,  244, 255, 255, 143,  255, 255, 255, 255
};

/* ID 36 - Icefire */
static const uint8_t fierce_ice_gp[] = {
    0, 0, 0, 0,  59, 0, 51, 117,  119, 0, 102, 255,  149, 38, 153, 255,
    180, 86, 204, 255,  217, 167, 230, 255,  255, 255, 255, 255
};

/* ID 37 - Cyane */
static const uint8_t colorfull_gp[] = {
    0, 61, 155, 44,  25, 95, 174, 77,  60, 132, 193, 113,  93, 154, 166, 125,
    106, 175, 138, 136,  109, 183, 121, 137,  113, 194, 104, 138,
    116, 225, 179, 165,  124, 255, 255, 192,  168, 167, 218, 203,
    255, 84, 182, 215
};

/* ID 38 - Light Pink */
static const uint8_t pink_purple_gp[] = {
    0, 79, 32, 109,  25, 90, 40, 117,  51, 102, 48, 124,  76, 141, 135, 185,
    102, 180, 222, 248,  109, 208, 236, 252,  114, 237, 250, 255,
    122, 206, 200, 239,  149, 177, 149, 222,  183, 187, 130, 203,
    255, 198, 111, 184
};

/* ID 39 - Autumn (WLED 原文 249/255 位置重复,原样保留) */
static const uint8_t es_autumn_19_gp[] = {
    0, 90, 14, 5,  51, 139, 41, 13,  84, 180, 70, 17,  104, 192, 202, 125,
    112, 177, 137, 3,  122, 190, 200, 131,  124, 192, 202, 124,
    135, 177, 137, 3,  142, 194, 203, 118,  163, 177, 68, 17,
    204, 128, 35, 12,  249, 74, 5, 2,  255, 74, 5, 2
};

/* ID 40 - Magenta */
static const uint8_t black_blue_magenta_white_gp[] = {
    0, 0, 0, 0,  42, 0, 0, 117,  84, 0, 0, 255,  127, 113, 0, 255,
    170, 255, 0, 255,  212, 255, 128, 255,  255, 255, 255, 255
};

/* ID 41 - Magred */
static const uint8_t black_magenta_red_gp[] = {
    0, 0, 0, 0,  63, 113, 0, 117,  127, 255, 0, 255,
    191, 255, 0, 117,  255, 255, 0, 0
};

/* ID 42 - Yelmag */
static const uint8_t black_red_magenta_yellow_gp[] = {
    0, 0, 0, 0,  42, 113, 0, 0,  84, 255, 0, 0,  127, 255, 0, 117,
    170, 255, 0, 255,  212, 255, 128, 117,  255, 255, 255, 0
};

/* ID 43 - Yelblu */
static const uint8_t blue_cyan_yellow_gp[] = {
    0, 0, 0, 255,  63, 0, 128, 255,  127, 0, 255, 255,
    191, 113, 255, 117,  255, 255, 255, 0
};

/* ID 44 - Orange & Teal (WLED 原文 55/200 处重复位置,原样保留) */
static const uint8_t orange_teal_gp[] = {
    0, 0, 150, 92,  55, 0, 150, 92,  200, 255, 72, 0,  255, 255, 72, 0
};

/* ID 45 - Tiamat */
static const uint8_t tiamat_gp[] = {
    0, 1, 2, 14,  33, 2, 5, 35,  100, 13, 135, 92,  120, 43, 255, 193,
    140, 247, 7, 249,  160, 193, 17, 208,  180, 39, 255, 154,
    200, 4, 213, 236,  220, 39, 252, 135,  240, 193, 213, 253,
    255, 255, 249, 255
};

/* ID 46 - April Night */
static const uint8_t april_night_gp[] = {
    0, 1, 5, 45,  10, 1, 5, 45,  25, 5, 169, 175,  40, 1, 5, 45,
    61, 1, 5, 45,  76, 45, 175, 31,  91, 1, 5, 45,  112, 1, 5, 45,
    127, 249, 150, 5,  143, 1, 5, 45,  162, 1, 5, 45,  178, 255, 92, 0,
    193, 1, 5, 45,  214, 1, 5, 45,  229, 223, 45, 72,  244, 1, 5, 45,
    255, 1, 5, 45
};

/* ID 47 - Orangery */
static const uint8_t orangery_gp[] = {
    0, 255, 95, 23,  30, 255, 82, 0,  60, 223, 13, 8,  90, 144, 44, 2,
    120, 255, 110, 17,  150, 255, 69, 0,  180, 158, 13, 11,
    210, 241, 82, 17,  255, 213, 37, 4
};

/* ID 48 - C9 */
static const uint8_t c9_gp[] = {
    0, 184, 4, 0,  60, 184, 4, 0,  65, 144, 44, 2,  125, 144, 44, 2,
    130, 4, 96, 2,  190, 4, 96, 2,  195, 7, 7, 88,  255, 7, 7, 88
};

/* ID 49 - Sakura */
static const uint8_t sakura_gp[] = {
    0, 196, 19, 10,  65, 255, 69, 45,  130, 223, 45, 72,
    195, 255, 82, 103,  255, 223, 13, 17
};

/* ID 50 - Aurora */
static const uint8_t aurora_gp[] = {
    0, 1, 5, 45,  64, 0, 200, 23,  128, 0, 255, 0,
    170, 0, 243, 45,  200, 0, 135, 7,  255, 1, 5, 45
};

/* ID 51 - Atlantica */
static const uint8_t atlantica_gp[] = {
    0, 0, 28, 112,  50, 32, 96, 255,  100, 0, 243, 45,
    150, 12, 95, 82,  200, 25, 190, 95,  255, 40, 170, 80
};

/* ID 52 - C9 2 */
static const uint8_t c9_2_gp[] = {
    0, 6, 126, 2,  45, 6, 126, 2,  46, 4, 30, 114,  90, 4, 30, 114,
    91, 255, 5, 0,  135, 255, 5, 0,  136, 196, 57, 2,  180, 196, 57, 2,
    181, 137, 85, 2,  255, 137, 85, 2
};

/* ID 53 - C9 New */
static const uint8_t c9_new_gp[] = {
    0, 255, 5, 0,  60, 255, 5, 0,  61, 196, 57, 2,  120, 196, 57, 2,
    121, 6, 126, 2,  180, 6, 126, 2,  181, 4, 30, 114,  255, 4, 30, 114
};

/* ID 54 - Temperature */
static const uint8_t temperature_gp[] = {
    0, 20, 92, 171,  14, 15, 111, 186,  28, 6, 142, 211,  42, 2, 161, 227,
    56, 16, 181, 239,  70, 38, 188, 201,  84, 86, 204, 200,
    99, 139, 219, 176,  113, 182, 229, 125,  127, 196, 230, 63,
    141, 241, 240, 22,  155, 254, 222, 30,  170, 251, 199, 4,
    184, 247, 157, 9,  198, 243, 114, 15,  226, 213, 30, 29,
    240, 151, 38, 35,  255, 151, 38, 35
};

/* ID 55 - Aurora 2 */
static const uint8_t aurora2_gp[] = {
    0, 17, 177, 13,  64, 121, 242, 5,  128, 25, 173, 121,
    192, 250, 77, 127,  255, 171, 101, 221
};

/* ID 56 - Retro Clown */
static const uint8_t retro_clown_gp[] = {
    0, 242, 168, 38,  117, 226, 78, 80,  255, 161, 54, 225
};

/* ID 57 - Candy */
static const uint8_t candy_gp[] = {
    0, 243, 242, 23,  15, 242, 168, 38,  142, 111, 21, 151,
    198, 74, 22, 150,  255, 0, 0, 117
};

/* ID 58 - Toxy Reaf */
static const uint8_t toxy_reaf_gp[] = {
    0, 2, 239, 126,  255, 145, 35, 217
};

/* ID 59 - Fairy Reaf */
static const uint8_t fairy_reaf_gp[] = {
    0, 220, 19, 187,  160, 12, 225, 219,  219, 203, 242, 223,
    255, 255, 255, 255
};

/* ID 60 - Semi Blue */
static const uint8_t semi_blue_gp[] = {
    0, 0, 0, 0,  12, 24, 4, 38,  53, 55, 8, 84,  80, 43, 48, 159,
    119, 31, 89, 237,  145, 50, 59, 166,  186, 71, 30, 98,
    233, 31, 15, 45,  255, 0, 0, 0
};

/* ID 61 - Pink Candy */
static const uint8_t pink_candy_gp[] = {
    0, 255, 255, 255,  45, 50, 64, 255,  112, 242, 16, 186,
    140, 255, 255, 255,  155, 242, 16, 186,  196, 116, 13, 166,
    255, 255, 255, 255
};

/* ID 62 - Red Reaf */
static const uint8_t red_reaf_gp[] = {
    0, 36, 68, 114,  104, 149, 195, 248,  188, 255, 0, 0,
    255, 94, 14, 9
};

/* ID 63 - Aqua Flash */
static const uint8_t aqua_flash_gp[] = {
    0, 0, 0, 0,  66, 130, 242, 245,  96, 255, 255, 53,  124, 255, 255, 255,
    153, 255, 255, 53,  188, 130, 242, 245,  255, 0, 0, 0
};

/* ID 64 - Yelblu Hot */
static const uint8_t yelblu_hot_gp[] = {
    0, 43, 30, 57,  58, 73, 0, 119,  122, 87, 0, 74,  158, 197, 57, 22,
    183, 218, 117, 27,  219, 239, 177, 32,  255, 246, 247, 27
};

/* ID 65 - Lite Light */
static const uint8_t lite_light_gp[] = {
    0, 0, 0, 0,  9, 20, 21, 22,  40, 46, 43, 49,  66, 46, 43, 49,
    101, 61, 16, 65,  255, 0, 0, 0
};

/* ID 66 - Red Flash */
static const uint8_t red_flash_gp[] = {
    0, 0, 0, 0,  99, 242, 12, 8,  130, 253, 228, 163,
    155, 242, 12, 8,  255, 0, 0, 0
};

/* ID 67 - Blink Red */
static const uint8_t blink_red_gp[] = {
    0, 4, 7, 4,  43, 40, 25, 62,  76, 61, 15, 36,  109, 207, 39, 96,
    127, 255, 156, 184,  165, 185, 73, 207,  204, 105, 66, 240,
    255, 77, 29, 78
};

/* ID 68 - Red Shift */
static const uint8_t red_shift_gp[] = {
    0, 98, 22, 93,  45, 103, 22, 73,  99, 192, 45, 56,  132, 235, 187, 59,
    175, 228, 85, 26,  201, 228, 56, 48,  255, 2, 0, 2
};

/* ID 69 - Red Tide */
static const uint8_t red_tide_gp[] = {
    0, 251, 46, 0,  28, 255, 139, 25,  43, 246, 158, 63,  58, 246, 216, 123,
    84, 243, 94, 10,  114, 177, 65, 11,  140, 255, 241, 115,
    168, 177, 65, 11,  196, 250, 233, 158,  216, 255, 94, 6,
    255, 126, 8, 4
};

/* ID 70 - Candy2 */
static const uint8_t candy2_gp[] = {
    0, 109, 102, 102,  25, 42, 49, 71,  48, 121, 96, 84,  73, 241, 214, 26,
    89, 216, 104, 44,  130, 42, 49, 71,  163, 255, 177, 47,
    186, 241, 214, 26,  211, 109, 102, 102,  255, 20, 19, 13
};

/* ID 71 - Traffic Light */
static const uint8_t trafficlight_gp[] = {
    0, 0, 0, 0,  85, 0, 255, 0,  170, 255, 255, 0,  255, 255, 0, 0
};

/*
 * 渐变调色板索引表(ID 13-71)。
 * 顺序与 WLED gGradientPalettes 完全一致,是 ID→数据映射的权威来源。
 * 注意:WLED 的 JSON_palette_names 存在原生错位(名称表 22 位="Beach"/26 位="Beech"
 * 与数据表 22=beech_gp/26=es_landscape_33_gp 互换),此处按数据表顺序保持映射,
 * 名称表保持同样错位以与 WLED 行为一致。
 */
static const uint8_t *const gradient_palettes[WLEDC_PALETTE_GRADIENT_COUNT] = {
    sunset_real_gp,            /* ID 13 */
    es_rivendell_15_gp,        /* ID 14 */
    es_ocean_breeze_036_gp,    /* ID 15 */
    rgi_15_gp,                 /* ID 16 */
    retro2_16_gp,              /* ID 17 */
    analogous_1_gp,            /* ID 18 */
    es_pinksplash_08_gp,       /* ID 19 */
    sunset_yellow_gp,          /* ID 20 */
    another_sunset_gp,         /* ID 21 */
    beech_gp,                  /* ID 22 */
    es_vintage_01_gp,          /* ID 23 */
    departure_gp,               /* ID 24 */
    es_landscape_64_gp,        /* ID 25 */
    es_landscape_33_gp,        /* ID 26 */
    rainbowsherbet_gp,         /* ID 27 */
    gr65_hult_gp,              /* ID 28 */
    gr64_hult_gp,              /* ID 29 */
    gmt_drywet_gp,             /* ID 30 */
    ib_jul01_gp,               /* ID 31 */
    es_vintage_57_gp,          /* ID 32 */
    ib15_gp,                   /* ID 33 */
    tertiary_01_gp,            /* ID 34 */
    lava_gp,                   /* ID 35 */
    fierce_ice_gp,             /* ID 36 */
    colorfull_gp,              /* ID 37 */
    pink_purple_gp,            /* ID 38 */
    es_autumn_19_gp,           /* ID 39 */
    black_blue_magenta_white_gp, /* ID 40 */
    black_magenta_red_gp,      /* ID 41 */
    black_red_magenta_yellow_gp, /* ID 42 */
    blue_cyan_yellow_gp,       /* ID 43 */
    orange_teal_gp,            /* ID 44 */
    tiamat_gp,                 /* ID 45 */
    april_night_gp,            /* ID 46 */
    orangery_gp,               /* ID 47 */
    c9_gp,                     /* ID 48 */
    sakura_gp,                 /* ID 49 */
    aurora_gp,                 /* ID 50 */
    atlantica_gp,              /* ID 51 */
    c9_2_gp,                   /* ID 52 */
    c9_new_gp,                 /* ID 53 */
    temperature_gp,            /* ID 54 */
    aurora2_gp,                /* ID 55 */
    retro_clown_gp,            /* ID 56 */
    candy_gp,                  /* ID 57 */
    toxy_reaf_gp,              /* ID 58 */
    fairy_reaf_gp,             /* ID 59 */
    semi_blue_gp,              /* ID 60 */
    pink_candy_gp,             /* ID 61 */
    red_reaf_gp,               /* ID 62 */
    aqua_flash_gp,             /* ID 63 */
    yelblu_hot_gp,             /* ID 64 */
    lite_light_gp,             /* ID 65 */
    red_flash_gp,              /* ID 66 */
    blink_red_gp,              /* ID 67 */
    red_shift_gp,              /* ID 68 */
    red_tide_gp,               /* ID 69 */
    candy2_gp,                 /* ID 70 */
    trafficlight_gp            /* ID 71 */
};

/* ------------------------------------------------------------------ */
/* 名称表:来源 palettes.cpp JSON_palette_names(72 项,索引即调色板 ID)。 */
/* 22 位 "Beach" / 26 位 "Beech" 与渐变数据表互换 —— WLED 原生错位,   */
/* 按原样保真(见 gradient_palettes 上方注释)。                       */
/* ------------------------------------------------------------------ */
static const char *const palette_names[WLEDC_PALETTE_FIXED_COUNT] = {
  "Default",             /*  0 */
  "* Random Cycle",      /*  1 */
  "* Color 1",           /*  2 */
  "* Colors 1&2",        /*  3 */
  "* Color Gradient",    /*  4 */
  "* Colors Only",       /*  5 */
  "Party",               /*  6 */
  "Cloud",               /*  7 */
  "Lava",                /*  8 */
  "Ocean",               /*  9 */
  "Forest",              /* 10 */
  "Rainbow",             /* 11 */
  "Rainbow Bands",       /* 12 */
  "Sunset",              /* 13 */
  "Rivendell",           /* 14 */
  "Breeze",              /* 15 */
  "Red & Blue",          /* 16 */
  "Yellowout",           /* 17 */
  "Analogous",           /* 18 */
  "Splash",              /* 19 */
  "Pastel",              /* 20 */
  "Sunset 2",            /* 21 */
  "Beach",               /* 22 */
  "Vintage",             /* 23 */
  "Departure",           /* 24 */
  "Landscape",           /* 25 */
  "Beech",               /* 26 */
  "Sherbet",             /* 27 */
  "Hult",                /* 28 */
  "Hult 64",             /* 29 */
  "Drywet",              /* 30 */
  "Jul",                 /* 31 */
  "Grintage",            /* 32 */
  "Rewhi",               /* 33 */
  "Tertiary",            /* 34 */
  "Fire",                /* 35 */
  "Icefire",             /* 36 */
  "Cyane",               /* 37 */
  "Light Pink",          /* 38 */
  "Autumn",              /* 39 */
  "Magenta",             /* 40 */
  "Magred",              /* 41 */
  "Yelmag",              /* 42 */
  "Yelblu",              /* 43 */
  "Orange & Teal",       /* 44 */
  "Tiamat",              /* 45 */
  "April Night",         /* 46 */
  "Orangery",            /* 47 */
  "C9",                  /* 48 */
  "Sakura",              /* 49 */
  "Aurora",              /* 50 */
  "Atlantica",           /* 51 */
  "C9 2",                /* 52 */
  "C9 New",              /* 53 */
  "Temperature",         /* 54 */
  "Aurora 2",            /* 55 */
  "Retro Clown",         /* 56 */
  "Candy",               /* 57 */
  "Toxy Reaf",           /* 58 */
  "Fairy Reaf",          /* 59 */
  "Semi Blue",           /* 60 */
  "Pink Candy",          /* 61 */
  "Red Reaf",            /* 62 */
  "Aqua Flash",          /* 63 */
  "Yelblu Hot",          /* 64 */
  "Lite Light",          /* 65 */
  "Red Flash",           /* 66 */
  "Blink Red",           /* 67 */
  "Red Shift",           /* 68 */
  "Red Tide",            /* 69 */
  "Candy2",              /* 70 */
  "Traffic Light"        /* 71 */
};

/* ------------------------------------------------------------------ */
/* 渐变填充:16.16 定点累加(来源 fastled_slim.cpp fill_gradient_RGB)。 */
/* startpos > endpos 时翻转参数对;distance==0 时 divisor 置 1 防除零。*/
/* ------------------------------------------------------------------ */
static void fill_gradient_rgb(uint8_t *entries,
                              uint8_t startpos, uint8_t r1, uint8_t g1, uint8_t b1,
                              uint8_t endpos,   uint8_t r2, uint8_t g2, uint8_t b2)
{
  if (endpos < startpos) { /* 翻转,保证 startpos <= endpos */
    uint8_t t;
    t = startpos; startpos = endpos; endpos = t;
    t = r1; r1 = r2; r2 = t;
    t = g1; g1 = g2; g2 = t;
    t = b1; b1 = b2; b2 = t;
  }
  int32_t rd = (int32_t)r2 - (int32_t)r1;
  int32_t gd = (int32_t)g2 - (int32_t)g1;
  int32_t bd = (int32_t)b2 - (int32_t)b1;
  int32_t divisor = (int32_t)endpos - (int32_t)startpos;
  if (divisor == 0) divisor = 1; /* 防除零 */
  int32_t rd_delta = (rd << 16) / divisor;
  int32_t gd_delta = (gd << 16) / divisor;
  int32_t bd_delta = (bd << 16) / divisor;
  int32_t r_shifted = (int32_t)r1 << 16;
  int32_t g_shifted = (int32_t)g1 << 16;
  int32_t b_shifted = (int32_t)b1 << 16;
  for (int32_t i = startpos; i <= endpos; i++) {
    entries[i * 3 + 0] = (uint8_t)(r_shifted >> 16);
    entries[i * 3 + 1] = (uint8_t)(g_shifted >> 16);
    entries[i * 3 + 2] = (uint8_t)(b_shifted >> 16);
    r_shifted += rd_delta;
    g_shifted += gd_delta;
    b_shifted += bd_delta;
  }
}

/* ------------------------------------------------------------------ */
/* 渐变数据 → 16 停色点展开(来源 loadDynamicGradientPalette)。        */
/* do-while 逐 4 字节项直至位置==255 计数;count>16 拒绝;             */
/* lastSlotUsed 防槽位复用;跨段越界回绕条目由 fill 覆盖。           */
/* ------------------------------------------------------------------ */
bool wledc_load_gradient_palette(const uint8_t *stops, uint8_t *out /*[48]*/)
{
  if (!stops || !out) return false;
  int count = 0; /* 渐变原始停色点数(含末位 255 项) */
  int index = 0;
  do {
    index = stops[count * 4];
    count++;
  } while (index != 255);
  if (count > 16) return false; /* 超出 16 停色点容量 */

  memset(out, 0, WLEDC_PALETTE_ENTRIES_BYTES);
  uint8_t istart8 = stops[0] / 16;
  uint8_t iend8   = istart8;
  uint8_t r = stops[1], g = stops[2], b = stops[3];
  int last_slot_used = -1;

  int i = 0;
  while (i < count && stops[i * 4] < 255) {
    i++;
    int indexend = stops[i * 4];
    uint8_t rgbend[3] = { stops[i * 4 + 1], stops[i * 4 + 2], stops[i * 4 + 3] };
    iend8 = indexend / 16;
    /* 防槽位复用:起点不得早于已占用末槽 */
    if (count < 16) {
      if ((int)istart8 <= last_slot_used && last_slot_used < 15) {
        istart8 = (uint8_t)(last_slot_used + 1);
        if (iend8 < istart8) iend8 = istart8;
      }
      last_slot_used = iend8;
    }
    fill_gradient_rgb(out, istart8, r, g, b, iend8, rgbend[0], rgbend[1], rgbend[2]);
    istart8 = iend8;
    r = rgbend[0]; g = rgbend[1]; b = rgbend[2];
  }
  return true;
}

/* ------------------------------------------------------------------ */
/* 16 停色点取色(来源 colors.cpp ColorFromPalette 16 停色点分支)。   */
/* LINEAR_NOWRAP:index 先重映射 (index*0xF0)>>8 防回绕;             */
/* hi4=entry 索引,lo4=插值权重:f2=lo4<<4、f1=256-f2;               */
/* hi4==15 时次取色回绕到 entries[0](环绕语义)。                    */
/* ------------------------------------------------------------------ */
uint32_t wledc_color_from_palette(const uint8_t *pal16x3, uint8_t index,
                                  uint8_t brightness, wledc_blend_t blend)
{
  if (!pal16x3) pal16x3 = fastled_palettes[0]; /* 回退 Party */

  if (blend == WLEDC_BLEND_LINEAR_NOWRAP) {
    index = (uint8_t)((index * 0xF0) >> 8); /* 重映射 255→240 防回绕 */
  }
  uint8_t hi4 = index >> 4;
  uint8_t lo4 = index & 0x0F;

  const uint8_t *entry = &pal16x3[hi4 * 3];
  uint8_t r1 = entry[0], g1 = entry[1], b1 = entry[2];
  uint8_t r2 = r1, g2 = g1, b2 = b1;

  if (lo4 != 0 && blend != WLEDC_BLEND_NONE) {
    if (hi4 == 15) {
      entry = &pal16x3[0]; /* 回绕到首项 */
    } else {
      entry += 3;          /* 下一停色点 */
    }
    r2 = entry[0]; g2 = entry[1]; b2 = entry[2];
    uint8_t f2 = (uint8_t)(lo4 << 4);
    uint8_t f1 = (uint8_t)(256 - f2);
    r1 = (uint8_t)(((r1 * f1) + (r2 * f2)) >> 8);
    g1 = (uint8_t)(((g1 * f1) + (g2 * f2)) >> 8);
    b1 = (uint8_t)(((b1 * f1) + (b2 * f2)) >> 8);
  }
  if (brightness != 0xFF) {
    if (brightness == 0) { r1 = g1 = b1 = 0; }
    else {
      uint16_t scale = (uint16_t)brightness + 1;
      r1 = (uint8_t)((r1 * scale) >> 8);
      g1 = (uint8_t)((g1 * scale) >> 8);
      b1 = (uint8_t)((b1 * scale) >> 8);
    }
  }
  return WLEDC_RGBW32(r1, g1, b1, 0);
}

/* ------------------------------------------------------------------ */
/* 自定义调色板注册表:ID 自 200 向下生长(200,199,...),              */
/* 槽位 slot = WLEDC_PALETTE_CUSTOM_ID_BASE - id,有效条件 slot < count。*/
/* ------------------------------------------------------------------ */
static const uint8_t *custom_palettes[WLEDC_PALETTE_CUSTOM_MAX];
static uint8_t custom_count_g = 0;

/* ------------------------------------------------------------------ */
/* 查询 ID 所属格式。                                                  */
/* ------------------------------------------------------------------ */
wledc_palette_fmt_t wledc_palette_get_format(wledc_palette_id_t id)
{
  if (id >= WLEDC_PALETTE_FIXED_COUNT) {
    /* 自定义区间:仅当槽位已注册时为渐变格式 */
    uint8_t slot = (uint8_t)(WLEDC_PALETTE_CUSTOM_ID_BASE - id);
    if (id <= WLEDC_PALETTE_CUSTOM_ID_BASE && slot < custom_count_g) {
      return WLEDC_PALETTE_FMT_GRADIENT;
    }
    return WLEDC_PALETTE_FMT_NONE;
  }
  if (id < WLEDC_PALETTE_DYNAMIC_COUNT) return WLEDC_PALETTE_FMT_NONE; /* 0-5 动态 */
  if (id < WLEDC_PALETTE_DYNAMIC_COUNT + WLEDC_PALETTE_FASTLED_COUNT) {
    return WLEDC_PALETTE_FMT_16X3; /* 6-12 */
  }
  return WLEDC_PALETTE_FMT_GRADIENT; /* 13-71 */
}

/* ------------------------------------------------------------------ */
/* 查询原始数据指针。固定 ID 查表,非法/越界回退 Party(ID 6)。       */
/* ------------------------------------------------------------------ */
const uint8_t *wledc_palette_get_data(wledc_palette_id_t id)
{
  if (id >= WLEDC_PALETTE_FIXED_COUNT) {
    uint8_t slot = (uint8_t)(WLEDC_PALETTE_CUSTOM_ID_BASE - id);
    if (id <= WLEDC_PALETTE_CUSTOM_ID_BASE && slot < custom_count_g) {
      return custom_palettes[slot];
    }
    return fastled_palettes[0]; /* 回退 Party */
  }
  if (id < WLEDC_PALETTE_DYNAMIC_COUNT) return NULL; /* 动态调色板无静态数据 */
  if (id < WLEDC_PALETTE_DYNAMIC_COUNT + WLEDC_PALETTE_FASTLED_COUNT) {
    return fastled_palettes[id - WLEDC_PALETTE_DYNAMIC_COUNT];
  }
  return gradient_palettes[id - WLEDC_PALETTE_DYNAMIC_COUNT - WLEDC_PALETTE_FASTLED_COUNT];
}

/* ------------------------------------------------------------------ */
/* 展开 16 停色点到 out(48 字节)。                                     */
/* ------------------------------------------------------------------ */
bool wledc_palette_get_entries(wledc_palette_id_t id,
                               uint8_t out[WLEDC_PALETTE_ENTRIES_BYTES])
{
  const uint8_t *src = wledc_palette_get_data(id);
  if (!src || !out) return false;

  if (wledc_palette_get_format(id) == WLEDC_PALETTE_FMT_16X3) {
    memcpy(out, src, WLEDC_PALETTE_ENTRIES_BYTES); /* 原生 16×3 直接拷贝 */
    return true;
  }
  return wledc_load_gradient_palette(src, out); /* 渐变数据展开 */
}

/* ------------------------------------------------------------------ */
/* 查询名称。固定查名称表;自定义返回 "Custom";非法回退 "Party"。      */
/* ------------------------------------------------------------------ */
const char *wledc_palette_name(wledc_palette_id_t id)
{
  if (id < WLEDC_PALETTE_FIXED_COUNT) return palette_names[id];
  if (id <= WLEDC_PALETTE_CUSTOM_ID_BASE &&
      (uint8_t)(WLEDC_PALETTE_CUSTOM_ID_BASE - id) < custom_count_g) {
    return "Custom";
  }
  return "Party"; /* 非法 ID 回退,与 WLED 行为一致 */
}

/* ------------------------------------------------------------------ */
/* 注册自定义渐变调色板,分配 ID 自 200 向下。                          */
/* ------------------------------------------------------------------ */
int wledc_palette_register(const uint8_t *stops16)
{
  if (!stops16) return WLEDC_ERR_NO_PALETTE;
  if (custom_count_g >= WLEDC_PALETTE_CUSTOM_MAX) return WLEDC_ERR_NO_PALETTE;

  custom_palettes[custom_count_g] = stops16;
  int id = WLEDC_PALETTE_CUSTOM_ID_BASE - custom_count_g;
  custom_count_g++;
  return id;
}

/* ------------------------------------------------------------------ */
/* 注销最后注册的自定义调色板(后进先出)。                             */
/* ------------------------------------------------------------------ */
bool wledc_palette_unregister_last(void)
{
  if (custom_count_g == 0) return false;
  custom_count_g--;
  custom_palettes[custom_count_g] = NULL;
  return true;
}

uint8_t wledc_palette_custom_count(void)
{
  return custom_count_g;
}