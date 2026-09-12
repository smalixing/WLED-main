/**
 * @file wled_c_segment.h
 * @brief 段(segment)结构与段管理 —— WLED Segment 类的纯 C 化。
 *
 * 对齐 WLED FX.h Segment 字段布局(design.md 3.4):
 *   colors×3 / start / stop / startY / stopY / offset / options /
 *   grouping / spacing / opacity / cct / mode / palette / speed /
 *   intensity / custom1-3 / blend_mode + 运行时 step / call /
 *   aux0 / aux1 / data / pixels。
 *
 * 与 WLED 的有意偏差(均注释于对应定义处):
 *   - custom3 为完整 uint8_t(WLED 为 5 位位域 + 3 个 check 位);
 *   - 2D XY(x,y) = x + y*vWidth 虚拟寻址/坐标变换由 wled_c_2d.h
 *     提供,段过渡子结构由 wled_c_transition.h 提供并内嵌于本结构;
 *   - name 字段未纳入(需求 5 未要求)。
 *
 * 运行时效果数据(data)按 WLED addUsedSegmentData 模式管理:
 * 效果首次运行时按需请求(wledc_segment_request_data),效果切换
 * 时统一重置(wledc_segment_mark_for_reset + reset_if_required),
 * 全局用量记账且向下 clamp 到 0。
 */
#ifndef WLED_C_SEGMENT_H
#define WLED_C_SEGMENT_H

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "wled_c_types.h"
#include "wled_c_transition.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 段默认主色:WLED DEFAULT_COLOR 0xFFAA00(暖橙)。 */
#define WLEDC_SEGMENT_DEFAULT_COLOR WLEDC_RGB32(255, 160, 0)

/** 段默认速度/强度(WLED 默认 128)。 */
#define WLEDC_SEGMENT_DEFAULT_SPEED    128
#define WLEDC_SEGMENT_DEFAULT_INTENSITY 128

/**
 * @brief 段结构(透明结构体,效果引擎经指针直接读写字段)。
 *
 * 持久字段(可序列化)在前,运行时状态在后。pixels 为该段的
 * 物理像素缓冲(逻辑颜色 0xWWRRGGBB,长度 = 段物理长度:
 * 1D 为 stop-start,2D 为宽×高);grouping/spacing 引起的
 * 虚拟长度折叠由效果引擎(任务 8/9)负责,pixels 恒为物理长度。
 */
typedef struct wledc_segment_s {
    /* ---- 持久字段(对齐 WLED Segment) ---- */
    uint32_t colors[WLEDC_NUM_COLORS];  /**< 主/次/三级颜色。 */
    uint16_t start;    /**< 起始全局像素(含)。 */
    uint16_t stop;     /**< 结束全局像素(不含);WLED 语义。 */
    uint16_t startY;   /**< 2D 起始行(含);1D 段恒为 0。 */
    uint16_t stopY;    /**< 2D 结束行(不含);1D 段恒为 0。 */
    uint16_t offset;   /**< 1D 效果绕回偏移。 */
    uint16_t options;  /**< SEG_OPT_* 位标志组合。 */
    uint8_t grouping;  /**< 像素分组(1 = 不分组)。 */
    uint8_t spacing;   /**< 组间间隔像素数。 */
    uint8_t opacity;   /**< 不透明度(0-255)。 */
    uint8_t cct;      /**< 色温(0-255,白色通道混色)。 */
    uint8_t mode;      /**< 效果 id(WLEDC_FX_COUNT 界定,任务 8)。 */
    uint8_t palette;  /**< 调色板 id(0-71 固定 / 自定义)。 */
    uint8_t speed;     /**< 效果速度(0-255)。 */
    uint8_t intensity; /**< 效果强度(0-255)。 */
    uint8_t custom1;   /**< 自定义滑条 1(0-255)。 */
    uint8_t custom2;   /**< 自定义滑条 2(0-255)。 */
    /** 自定义滑条 3。偏差:WLED 为 5 位 + check1-3 位域,此处为整字节。 */
    uint8_t custom3;
    uint8_t blend_mode; /**< 颜色混合模式(0 = 无混合)。 */

    /* ---- 运行时状态(不序列化) ---- */
    uint32_t step;     /**< 效果步进计数(帧号)。 */
    uint32_t call;     /**< 效果调用计数。 */
    uint16_t aux0;     /**< 效果私有辅助变量 0(SEGENV.aux0)。 */
    uint16_t aux1;     /**< 效果私有辅助变量 1(SEGENV.aux1)。 */
    uint8_t *data;     /**< 效果私有数据(lazy 分配,data_len 为容量)。 */
    uint16_t data_len; /**< data 已分配字节数(私有记账)。 */
    uint32_t *pixels;  /**< 段物理像素缓冲(懒初始化,随范围重分配)。 */
    uint16_t pixels_len; /**< pixels 元素数(私有记账)。 */
#ifdef WLEDC_ENABLE_TRANSITIONS
    /** 段过渡状态(见 wled_c_transition.h;非过渡期 active=false)。 */
    wledc_transition_t transition;
#endif
    bool active;       /**< 槽位占用标志(false = 空槽,可复用)。 */
} wledc_segment_t;

/* ------------------------------------------------------------------ */
/* 段生命周期                                                           */
/* ------------------------------------------------------------------ */

/**
 * @brief 添加一个 1D 段(1D 语义:startY=0, stopY=0)。
 *
 * 新段默认:主色 0xFFAA00、mode=0(纯色)、palette=0、speed/
 * intensity=128、opacity=255、grouping=1、spacing=0、options 含
 * SEG_OPT_ON。
 *
 * @param start 起始全局像素(含)
 * @param stop  结束全局像素(不含,必须 > start 且 ≤ 65535)
 * @param out_id 输出段 ID(可 NULL)
 * @return ESP_OK;ESP_ERR_INVALID_ARG 参数非法;
 *         WLEDC_ERR_NO_SEGMENT 无空槽;ESP_ERR_NO_MEM 像素缓冲分配失败
 */
esp_err_t wledc_segment_add(uint16_t start, uint16_t stop,
                            wledc_segment_id_t *out_id);

/**
 * @brief 删除一个段,释放其像素缓冲与效果数据。
 * @param id 段 ID
 * @return ESP_OK;WLEDC_ERR_NO_SEGMENT 段不存在
 */
esp_err_t wledc_segment_remove(wledc_segment_id_t id);

/**
 * @brief 按越界安全的 ID 获取段实例(仅活跃段)。
 * @return 段指针;ID 非法/未占用时 NULL
 */
wledc_segment_t *wledc_segment_get(wledc_segment_id_t id);

/**
 * @brief 获取全部段槽位(用于效果引擎遍历)。
 * @param count 输出槽位总数 = WLEDC_MAX_SEGMENTS(可 NULL)
 * @return 内部静态数组首地址,活跃项以 seg->active 标记,请勿长期持有
 */
wledc_segment_t *wledc_segment_get_all(uint8_t *count);

/** 当前活跃段数量(0..WLEDC_MAX_SEGMENTS)。 */
uint8_t wledc_segment_count(void);

/* ------------------------------------------------------------------ */
/* 段修改                                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief 修改段 1D 范围(像素缓冲随之重分配,触发运行时重置)。
 * @return ESP_OK;ESP_ERR_INVALID_ARG / WLEDC_ERR_NO_SEGMENT;ESP_ERR_NO_MEM
 */
esp_err_t wledc_segment_set_range(wledc_segment_id_t id,
                                   uint16_t start, uint16_t stop);

/**
 * @brief 修改段 2D 尺寸(以段当前 start/startY 为左上角)。
 *
 * 宽 = stop-start,高 = stopY-startY;pixels 重分配为宽×高。
 * XY 寻址映射本身属任务 7(2D 支持)。
 *
 * @param width  列数(≥ 1)
 * @param height 行数(≥ 1)
 * @return ESP_OK;ESP_ERR_INVALID_ARG(width/height 为 0 或乘积超限)/
 *         WLEDC_ERR_NO_SEGMENT;ESP_ERR_NO_MEM
 */
esp_err_t wledc_segment_set_dimensions(wledc_segment_id_t id,
                                        uint16_t width, uint16_t height);

/**
 * @brief 切换段效果(mode 变化时统一重置运行时:step/call/aux0/aux1
 *        清零,data 释放)。
 * @param mode 效果 id(有效性由效果引擎在运行时界定)
 * @return ESP_OK;WLEDC_ERR_NO_SEGMENT
 */
esp_err_t wledc_segment_set_mode(wledc_segment_id_t id, wledc_fx_id_t mode);

/**
 * @brief 设置段调色板(不触发运行时重置,与 WLED 语义一致)。
 * @return ESP_OK;WLEDC_ERR_NO_SEGMENT
 */
esp_err_t wledc_segment_set_palette(wledc_segment_id_t id,
                                    wledc_palette_id_t palette);

/**
 * @brief 整体覆盖段选项位(SEG_OPT_* 组合)。
 *
 * 若传入值含 SEG_OPT_RESET,下一帧 reset_if_required 即触发重置。
 * @return ESP_OK;WLEDC_ERR_NO_SEGMENT
 */
esp_err_t wledc_segment_set_options(wledc_segment_id_t id, uint16_t options);

/* ------------------------------------------------------------------ */
/* 运行时数据管理(效果引擎使用,对齐 addUsedSegmentData 模式)          */
/* ------------------------------------------------------------------ */

/**
 * @brief 请求段效果数据缓冲(容量语义:只增不减)。
 *
 * 已有容量足够则直接返回现有指针;否则 realloc 到 len 字节并计入
 * 全局用量(s_used_segment_data,只增加增量)。
 *
 * @param seg 活跃段指针(wledc_segment_get 的返回值)
 * @param len 需要的字节数
 * @return 数据指针;seg 为 NULL 或内存不足时 NULL
 */
uint8_t *wledc_segment_request_data(wledc_segment_t *seg, uint16_t len);

/**
 * @brief 标记段需要重置(options |= SEG_OPT_RESET)。
 *
 * 效果切换 / 范围变化等统一走此入口,由效果引擎在下一帧
 * 调用 wledc_segment_reset_if_required 消费。
 */
void wledc_segment_mark_for_reset(wledc_segment_t *seg);

/**
 * @brief 若段携带 SEG_OPT_RESET 标志则执行重置并清除标志。
 *
 * 重置内容:step/call/aux0/aux1 清零,data 释放并从全局用量
 * 记账中扣减(clamp ≥ 0)。pixels 缓冲保留(范围变化时已重分配)。
 */
void wledc_segment_reset_if_required(wledc_segment_t *seg);

/** 当前全部段效果数据总用量(字节,诊断用)。 */
unsigned wledc_segment_used_data(void);

#ifdef __cplusplus
}
#endif

#endif /* WLED_C_SEGMENT_H */