/**
 * @file wled_c_strip.h
 * @brief 灯带核心:帧率调度与总线编排(任务 11,design.md 3.4)。
 *
 * 重写自 wled00/FX_fcn.cpp 的 WS2812FX 服务循环(service/setBrightness/
 * restartRuntime/setTransitionMode)及 wled00/FX.h 相关常量。
 *
 * 职责:
 *  1. 帧率调度 —— wledc_strip_service() 按 target FPS 与最小帧间隔
 *     (WLEDC_MIN_FRAME_DELAY_MS)双重门控决定是否渲染当前帧;
 *  2. 段渲染编排 —— 遍历活跃段:过渡推进(wled_c_transition)→
 *     效果运行(wledc_fx_run)→ 亮度/CCT 输出混合 → 写入总线像素缓冲;
 *  3. 总线输出 —— wledc_bus_show() 一次性刷新所有活跃总线;
 *  4. 时间轴 —— 维护 timebase 偏移,供"效果从头重播"复位使用。
 *
 * 与 WLED 的差异:
 *  - now 由调用方注入(任务 12 的刷新任务统一取 esp_timer 毫秒),
 *    本模块不直接依赖定时器,便于宿主测试注入时间轴;
 *  - 单线程组件:省略 suspend/waitForIt/_isServicing 服务保护;
 *  - 亮度缩放只做一次:set_brightness 内经 wledc_scaled_bri()
 *    (wled_c_bri)换算后下发总线,调用方传原始亮度;
 *  - gammaCorrectBri(亮度伽马)不移植,伽马仅在颜色路径(wled_c_color)生效;
 *  - WLED 的 beginDraw/modeBlend/oldSegment 像素混合路径不移植:过渡
 *    期的平滑由 wled_c_transition 的参数插值保证,service 不做像素混合。
 */

#ifndef WLED_C_STRIP_H
#define WLED_C_STRIP_H

#include <stdbool.h>
#include <stdint.h>

#include "wled_c/wled_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 两帧之间的最小间隔(ms)。
 *
 * 对应 wled00/FX.h 的 MIN_FRAME_DELAY(classic ESP32 取 2):限制
 * service() 的重绘频率,给网络/系统任务留出 CPU 时间。组件统一面向
 * ESP32 家族,取经典值 2。
 */
#define WLEDC_MIN_FRAME_DELAY_MS 2U

/** target FPS 取 0 表示不限帧率(wled00/FX.h 的 FPS_UNLIMITED)。 */
#define WLEDC_FPS_UNLIMITED 0U

/* ------------------------------------------------------------------ */
/* 主循环                                                              */
/* ------------------------------------------------------------------ */

/**
 * @brief 灯带服务主循环:外部 tick 每次调用一次。
 *
 * 门控(wled00/FX_fcn.cpp service() 同语义):
 *  1. elapsed = now - last_service_show;
 *     time_to_show = elapsed >= frametime;
 *  2. triggered(强制刷新标志)或 target_fps 为 UNLIMITED 时视为可显示;
 *  3. 不满足 time_to_show 直接返回;
 *  4. elapsed <= WLEDC_MIN_FRAME_DELAY_MS 时返回(限流)。
 *
 * 通过门控后遍历全部段:active 段先推进过渡,再运行效果
 * (wledc_fx_run 内部已含 reset_if_required,此处不重复调用);
 * 存在活跃段则记录本次输出时刻并 wledc_bus_show()。
 *
 * @param now 当前毫秒时间(调用方注入,如 esp_timer_get_time()/1000)
 */
void wledc_strip_service(wledc_time_ms_t now);

/**
 * @brief 置强制刷新标志:下一帧无视帧率门控立即计算。
 *
 * 对应 WS2812FX::trigger()。
 */
void wledc_strip_trigger(void);

/** 查询强制刷新标志是否置位(对应 needsUpdate())。 */
bool wledc_strip_triggered(void);

/* ------------------------------------------------------------------ */
/* 运行时状态                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief 请求全部段在下一帧重置运行时状态并从头重播效果。
 *
 * 对应 WS2812FX::restartRuntime():单线程组件省略 suspend/waitForIt
 * 包裹,直接对全部段执行 mark_for_reset + reset_if_required。
 * 开灯路径(wled_c_bri 的 toggle)在亮度为 0 时调用。
 */
void wledc_strip_restart_runtime(void);

/**
 * @brief 为全部段启动/停止过渡模式。
 *
 * 对应 WS2812FX::setTransitionMode():
 *  - t == true:全部段以当前过渡时长(wledc_transition(),wled_c_bri)
 *    启动过渡,现值被快照为混合起点;
 *  - t == false:全部段以时长 0 启动过渡,立即结束并定格当前值。
 *
 * @param t   过渡模式开关
 * @param now 当前毫秒时间(过渡起点计时)
 */
void wledc_strip_set_transition_mode(bool t, wledc_time_ms_t now);

/* ------------------------------------------------------------------ */
/* 时间轴                                                              */
/* ------------------------------------------------------------------ */

/**
 * @brief 复位效果时间轴:下一个动画帧的效果时间从 0 重新开始。
 *
 * 对应 WS2812FX::resetTimebase():timebase = 0 - now。
 * WLED 在亮度归零时调用,使重新点亮后效果从头重播。
 *
 * @param now 当前毫秒时间
 */
void wledc_strip_reset_timebase(wledc_time_ms_t now);

/**
 * @brief 效果时间轴当前值(now + timebase)。
 *
 * service() 用同一时间轴驱动效果;外部模块需要"效果视角"的时间
 * (如过渡起点记录)时应使用本值而非原始 now。
 *
 * @param now 当前毫秒时间
 */
wledc_time_ms_t wledc_strip_now(wledc_time_ms_t now);

/* ------------------------------------------------------------------ */
/* 亮度与帧率                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief 设置灯带亮度并下发全部活跃总线。
 *
 * 对应 WS2812FX::setBrightness(b, direct):
 *  1. 亮度未变化时直接返回;
 *  2. 新亮度为 0 时解冻全部段(freeze = false);
 *  3. 经 wledc_scaled_bri()(briMultiplier,见 wled_c_bri)换算后对
 *     全部活跃总线 wledc_bus_set_brightness —— 缩放仅在此处做一次,
 *     调用方(如 bri 模块的过渡路径)传原始亮度;
 *  4. direct == false 且距上次输出超过 min(frametime, 帧时间)时
 *     自动 trigger() 一次,让新亮度尽快上屏。
 *
 * 注意:WLED 的 gammaCorrectBri(亮度伽马)不移植。
 *
 * @param b      原始亮度 0-255(未缩放)
 * @param direct true:立即输出语义(不自动 trigger)
 */
void wledc_strip_set_brightness(uint8_t b, bool direct);

/** 当前灯带亮度(set_brightness 最后一次设置的原始值)。 */
uint8_t wledc_strip_brightness(void);

/**
 * @brief 设置目标帧率。
 *
 * 对应 WS2812FX::setTargetFps():帧时间 = fps > 0 ? 1000/fps :
 * WLEDC_MIN_FRAME_DELAY_MS。默认 WLEDC_FPS(42)。
 */
void wledc_strip_set_target_fps(uint8_t fps);

/** 当前帧时间(ms):fps > 0 时为 1000/fps,否则为最小帧间隔。 */
uint16_t wledc_strip_frametime(void);

#ifdef __cplusplus
}
#endif
#endif /* WLED_C_STRIP_H */