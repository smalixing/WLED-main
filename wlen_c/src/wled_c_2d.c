/**
 * @file wled_c_2d.c
 * @brief 2D 段虚拟寻址与坐标变换实现(契约见 wled_c_2d.h)。
 *
 * WLED 参照:FX.h width()/height()/virtualWidth()/virtualHeight()/
 * XY()/setPixelColorXY()/getPixelColorXY()(L470-540)。
 */
#include "wled_c_2d.h"

#ifdef WLEDC_ENABLE_2D

/* ------------------------------------------------------------------ */
/* 内部几何                                                             */
/* ------------------------------------------------------------------ */

/** 物理宽度(列数,无 grouping 折叠)。 */
static uint16_t phys_width(const wledc_segment_t *seg)
{
    return (uint16_t)(seg->stop - seg->start);
}

/** 物理高度(行数)。 */
static uint16_t phys_height(const wledc_segment_t *seg)
{
    return (uint16_t)(seg->stopY - seg->startY);
}

/* ------------------------------------------------------------------ */
/* 虚拟几何                                                             */
/* ------------------------------------------------------------------ */

bool wledc_segment_is_2d(const wledc_segment_t *seg)
{
    return seg != NULL && seg->stopY > seg->startY;
}

esp_err_t wledc_segment_virtual_width(const wledc_segment_t *seg,
                                      uint16_t *out_vw)
{
    if (seg == NULL || out_vw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!wledc_segment_is_2d(seg)) {
        return WLEDC_ERR_NOT_2D;
    }
    uint16_t w = (seg->options & SEG_OPT_TRANSPOSE) ? phys_height(seg)
                                                    : phys_width(seg);
    if (seg->options & SEG_OPT_MIRROR) {
        w = (uint16_t)((w + 1u) / 2u); /* X 镜像复制半幅 */
    }
    *out_vw = w;
    return ESP_OK;
}

esp_err_t wledc_segment_virtual_height(const wledc_segment_t *seg,
                                        uint16_t *out_vh)
{
    if (seg == NULL || out_vh == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!wledc_segment_is_2d(seg)) {
        return WLEDC_ERR_NOT_2D;
    }
    if (seg->options & SEG_OPT_TRANSPOSE) {
        /* WLED:TRANSPOSE 下 vHeight = vWidth。 */
        return wledc_segment_virtual_width(seg, out_vh);
    }
    uint16_t h = phys_height(seg);
    if (seg->options & SEG_OPT_MIRROR_Y) {
        h = (uint16_t)((h + 1u) / 2u); /* Y 镜像复制半幅 */
    }
    *out_vh = h;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 像素读写(虚拟坐标)                                                 */
/* ------------------------------------------------------------------ */

esp_err_t wledc_segment_set_pixel_color_xy(wledc_segment_t *seg,
                                           uint16_t x, uint16_t y,
                                           uint32_t color)
{
    if (seg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t vw, vh;
    esp_err_t err = wledc_segment_virtual_width(seg, &vw);
    if (err != ESP_OK) {
        return err; /* 非 2D 段:WLEDC_ERR_NOT_2D */
    }
    if ((err = wledc_segment_virtual_height(seg, &vh)) != ESP_OK) {
        return err;
    }
    /* 无符号越界检查:越界静默丢弃(WLED 越界写为无操作)。 */
    if (x >= vw || y >= vh || seg->pixels == NULL) {
        return ESP_OK;
    }
    seg->pixels[wledc_2d_xy_index(x, y, vw)] = color;
    return ESP_OK;
}

uint32_t wledc_segment_get_pixel_color_xy(const wledc_segment_t *seg,
                                          uint16_t x, uint16_t y)
{
    uint16_t vw, vh;
    if (seg == NULL ||
        wledc_segment_virtual_width(seg, &vw) != ESP_OK ||
        wledc_segment_virtual_height(seg, &vh) != ESP_OK) {
        return 0;
    }
    if (x >= vw || y >= vh || seg->pixels == NULL) {
        return 0;
    }
    return seg->pixels[wledc_2d_xy_index(x, y, vw)];
}

#endif /* WLEDC_ENABLE_2D */