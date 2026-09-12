/**
 * @file wled_c_bus.c
 * @brief 总线抽象与 BusManager 实现(任务 5,design.md 3.3)。
 *
 * 数据流(与任务 4 头文件约定的分层一致):
 *   上层写入逻辑色(0xWWRRGGBB) → 按 start/length/reversed 路由到
 *   本地像素缓冲 → show() 批量完成总线级亮度缩放与 12 种颜色
 *   排列重排 → 生成目标序字节流 → wledc_rmt_show 输出。
 *
 * RMT 初始化映射:wledc_bus_config_t → wledc_rmt_config_t
 *   (gpio = pin,length,length,rgbw = (type == WS2812_RGBW))。
 *
 * 线程安全:暂不加锁(任务 12 统一互斥锁)。
 */
#include "wled_c_bus.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 颜色排列映射表                                                       */
/* ------------------------------------------------------------------ */

/**
 * 每种 color_order 输出 4 个通道时的逻辑通道下标
 * (0=R, 1=G, 2=B, 3=W;RGB 总线仅使用前 3 列)。
 * 表按 wledc_color_order_t 枚举值直接索引,enum 定义见
 * wled_c_types.h(0..5 = RGB 序,6..11 = RGBW 序)。
 */
static const uint8_t s_order_map[WLEDC_COLOR_ORDER_COUNT][4] = {
    [WLEDC_ORDER_GRB]  = {1, 0, 2, 3},  /* G R B [W] */
    [WLEDC_ORDER_RGB]  = {0, 1, 2, 3},  /* R G B [W] */
    [WLEDC_ORDER_BRG]  = {2, 0, 1, 3},  /* B R G [W] */
    [WLEDC_ORDER_RBG]  = {0, 2, 1, 3},  /* R B G [W] */
    [WLEDC_ORDER_GBR]  = {1, 2, 0, 3},  /* G B R [W] */
    [WLEDC_ORDER_BGR]  = {2, 1, 0, 3},  /* B G R [W] */
    [WLEDC_ORDER_GRBW] = {1, 0, 2, 3},  /* G R B W  */
    [WLEDC_ORDER_RGBW] = {0, 1, 2, 3},  /* R G B W  */
    [WLEDC_ORDER_BRGW] = {2, 0, 1, 3},  /* B R G W  */
    [WLEDC_ORDER_RBGW] = {0, 2, 1, 3},  /* R B G W  */
    [WLEDC_ORDER_GBRW] = {1, 2, 0, 3},  /* G B R W  */
    [WLEDC_ORDER_BGRW] = {2, 1, 0, 3},  /* B G R W  */
};

/* ------------------------------------------------------------------ */
/* BusManager 内部状态                                                  */
/* ------------------------------------------------------------------ */

/** 槽位池:静态数组,ID 即下标(最多 WLEDC_MAX_BUSES = 4)。 */
static wledc_bus_t s_buses[WLEDC_MAX_BUSES];

/* ------------------------------------------------------------------ */
/* 内部工具                                                            */
/* ------------------------------------------------------------------ */

static wledc_bus_t *bus_find(wledc_bus_id_t id)
{
    if (id >= WLEDC_MAX_BUSES || !s_buses[id].active) {
        return NULL;
    }
    return &s_buses[id];
}

/**
 * 四舍五入的 8 位亮度缩放:(v * b + 127) / 255。
 * 255*255 → 255,b=255 时恒等,优于简单右移的系统性偏暗。
 */
static uint8_t bus_scale8(uint8_t v, uint8_t b)
{
    return (uint8_t)(((uint16_t)v * (uint16_t)b + 127U) / 255U);
}

/* ------------------------------------------------------------------ */
/* 总线生命周期                                                         */
/* ------------------------------------------------------------------ */

esp_err_t wledc_bus_add(const wledc_bus_config_t *cfg, wledc_bus_id_t *out_id)
{
    if (cfg == NULL || out_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 配置校验(design.md 3.3 + 头文件约定)。 */
    if (cfg->length == 0 || cfg->length > WLEDC_MAX_LEDS_PER_BUS) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((int)cfg->pin < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->color_order >= WLEDC_COLOR_ORDER_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 色序/类型约束:RGBW 总线必须使用带 W 通道的 6..11,
     * RGB 总线必须使用 0..5(否则输出通道数不匹配)。 */
    if (cfg->type == WLEDC_BUS_WS2812_RGBW) {
        if (cfg->color_order <= WLEDC_ORDER_BGR) {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (cfg->type == WLEDC_BUS_WS2812) {
        if (cfg->color_order > WLEDC_ORDER_BGR) {
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    wledc_bus_id_t id;
    for (id = 0; id < WLEDC_MAX_BUSES; id++) {
        if (!s_buses[id].active) {
            break;
        }
    }
    if (id == WLEDC_MAX_BUSES) {
        return WLEDC_ERR_NO_BUS;
    }

    wledc_bus_t *bus = &s_buses[id];
    bus->channels = (cfg->type == WLEDC_BUS_WS2812_RGBW) ? 4 : 3;

    /* 像素缓冲(length × 4 字节逻辑色)与输出字节缓冲。 */
    bus->pixels = calloc(cfg->length, sizeof(uint32_t));
    bus->out = malloc((size_t)cfg->length * bus->channels);
    if (bus->pixels == NULL || bus->out == NULL) {
        free(bus->pixels);
        free(bus->out);
        bus->pixels = NULL;
        bus->out = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* RMT 驱动初始化:wledc_bus_config_t → wledc_rmt_config_t 映射。 */
    wledc_rmt_config_t rmt_cfg = {
        .gpio = cfg->pin,
        .length = cfg->length,
        .rgbw = (cfg->type == WLEDC_BUS_WS2812_RGBW),
    };
    esp_err_t err = wledc_rmt_init(&bus->rmt, &rmt_cfg);
    if (err != ESP_OK) {
        free(bus->pixels);
        free(bus->out);
        bus->pixels = NULL;
        bus->out = NULL;
        return err;
    }

    bus->config = *cfg;   /* 配置快照 */
    bus->active = true;
    *out_id = id;
    return ESP_OK;
}

esp_err_t wledc_bus_remove(wledc_bus_id_t id)
{
    wledc_bus_t *bus = bus_find(id);
    if (bus == NULL) {
        return WLEDC_ERR_NO_BUS;
    }

    wledc_rmt_deinit(&bus->rmt);   /* 通道/编码器/句柄清零 */
    free(bus->pixels);
    free(bus->out);

    memset(bus, 0, sizeof(*bus));  /* active 归 false */
    return ESP_OK;
}

wledc_bus_t *wledc_bus_get(wledc_bus_id_t id)
{
    return bus_find(id);
}

wledc_bus_t *wledc_bus_get_all(uint8_t *count)
{
    if (count != NULL) {
        uint8_t n = 0;
        for (int i = 0; i < WLEDC_MAX_BUSES; i++) {
            if (s_buses[i].active) {
                n++;
            }
        }
        *count = n;
    }
    return s_buses;
}

uint16_t wledc_bus_total_pixels(void)
{
    uint16_t max_end = 0;
    for (int i = 0; i < WLEDC_MAX_BUSES; i++) {
        if (s_buses[i].active) {
            uint32_t end = (uint32_t)s_buses[i].config.start + s_buses[i].config.length;
            if (end > max_end) {
                max_end = (uint16_t)end;
            }
        }
    }
    return max_end;
}

/* ------------------------------------------------------------------ */
/* 像素接口:全局索引路由                                               */
/* ------------------------------------------------------------------ */

esp_err_t wledc_bus_set_pixel_color(wledc_bus_id_t id, uint16_t index, uint32_t color)
{
    wledc_bus_t *bus = bus_find(id);
    if (bus == NULL) {
        return WLEDC_ERR_NO_BUS;
    }

    uint32_t local = (uint32_t)index - bus->config.start;
    if (local >= bus->config.length) {
        return ESP_ERR_INVALID_ARG;  /* 越出该总线范围 */
    }
    if (bus->config.reversed) {
        local = bus->config.length - 1U - local;
    }
    bus->pixels[local] = color;
    return ESP_OK;
}

uint32_t wledc_bus_get_pixel_color(wledc_bus_id_t id, uint16_t index)
{
    wledc_bus_t *bus = bus_find(id);
    if (bus == NULL) {
        return WLEDC_BLACK;
    }

    uint32_t local = (uint32_t)index - bus->config.start;
    if (local >= bus->config.length) {
        return WLEDC_BLACK;
    }
    if (bus->config.reversed) {
        local = bus->config.length - 1U - local;
    }
    return bus->pixels[local];
}

esp_err_t wledc_bus_set_brightness(wledc_bus_id_t id, uint8_t brightness)
{
    wledc_bus_t *bus = bus_find(id);
    if (bus == NULL) {
        return WLEDC_ERR_NO_BUS;
    }
    bus->config.brightness = brightness;
    return ESP_OK;
}

esp_err_t wledc_bus_clear(wledc_bus_id_t id, uint32_t color)
{
    wledc_bus_t *bus = bus_find(id);
    if (bus == NULL) {
        return WLEDC_ERR_NO_BUS;
    }
    for (uint16_t i = 0; i < bus->config.length; i++) {
        bus->pixels[i] = color;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 输出:亮度缩放 + 色序重排 → RMT                                       */
/* ------------------------------------------------------------------ */

esp_err_t wledc_bus_show(void)
{
    esp_err_t first_err = ESP_OK;

    for (int i = 0; i < WLEDC_MAX_BUSES; i++) {
        wledc_bus_t *bus = &s_buses[i];
        if (!bus->active) {
            continue;
        }

        const wledc_bus_config_t *cfg = &bus->config;
        const uint8_t ch = bus->channels;
        const bool scale = (cfg->brightness != 255);
        const uint8_t *map = s_order_map[cfg->color_order];

        /* 批量重排 + 缩放:一像素一通道循环,写出 length*ch 字节。 */
        for (uint16_t p = 0; p < cfg->length; p++) {
            uint32_t c = bus->pixels[p];
            uint8_t chan[4] = {
                WLEDC_R(c), WLEDC_G(c), WLEDC_B(c), WLEDC_W(c),
            };
            if (scale) {
                chan[0] = bus_scale8(chan[0], cfg->brightness);
                chan[1] = bus_scale8(chan[1], cfg->brightness);
                chan[2] = bus_scale8(chan[2], cfg->brightness);
                chan[3] = bus_scale8(chan[3], cfg->brightness);
            }
            uint8_t *dst = &bus->out[(size_t)p * ch];
            for (uint8_t k = 0; k < ch; k++) {
                dst[k] = chan[map[k]];
            }
        }

        /* 交底层 RMT 输出;上一帧未完成 → 该总线跳帧。 */
        esp_err_t err = wledc_rmt_show(&bus->rmt, bus->out,
                                      (size_t)cfg->length * ch);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT && first_err == ESP_OK) {
            first_err = err;  /* 跳帧不算失败,记录首个真实错误 */
        }
    }
    return first_err;
}