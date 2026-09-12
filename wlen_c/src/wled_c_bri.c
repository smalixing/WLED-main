/**
 * @file wled_c_bri.c
 * @brief 亮度状态机与夜灯（契约见 wled_c_bri.h）。
 *
 * WLED 参照：led.cpp（toggleOnOff / scaledBri / applyBri /
 * applyFinalBri / stateUpdated / handleTransitions / handleNightlight）。
 *
 * 不移植范围：
 *  - NL_MODE_SUN 日出/日落效果路径；
 *  - COLORFADE 的主色渐变（组件无全局主色，颜色归属段层）；
 *  - stateUpdated 的通知分发、接口刷新与 usermod 回调；
 *  - realtime 常驻层门控（无 realtime 层）。
 */

/*
 * SPDX-License-Identifier: GPL-3.0-only
 * This file is part of the WLED C rewrite (wled_c component).
 */

#include "wled_c/wled_c_bri.h"

#include "wled_c/wled_c_strip.h"

/* ------------------------------------------------------------------ */
/* 内部状态                                                             */
/* ------------------------------------------------------------------ */

static uint8_t s_bri = 128;           /* 全局亮度（WLED 默认 128） */
static uint8_t s_bri_old = 128;       /* 过渡起点亮度 */
static uint8_t s_bri_t = 128;        /* 过渡当前亮度（施加目标） */
static uint8_t s_bri_last = 128;      /* 关灯前亮度（恢复用） */
static bool s_state_changed = false;  /* 开关状态变化标记 */
static uint8_t s_bri_multiplier = 100;/* 亮度缩放百分比（WLED 默认 100） */

/* 过渡（对齐 led.cpp transitionDelay/transitionActive） */
static uint16_t s_transition_duration = 750; /* WLED 默认 750ms */
static bool s_transition_active = false;
static wledc_time_ms_t s_transition_start_time = 0;

/* 夜灯（对齐 led.cpp nightlight* 系列） */
static bool s_nl_active = false;       /* nightlightActive */
static bool s_nl_active_old = false;  /* nightlightActiveOld（兼任初始化标志） */
static wledc_nl_mode_t s_nl_mode = WLEDC_NL_MODE_SET;
static uint8_t s_nl_delay_mins = 60;  /* WLED 默认 60 分钟 */
static uint8_t s_nl_target_bri = 0;   /* nightlightTargetBri */
static wledc_time_ms_t s_nl_start_time = 0;
static wledc_time_ms_t s_nl_delay_ms = 0;
static wledc_time_ms_t s_nl_last_update = 0;
static uint8_t s_nl_bri_t = 0;        /* 夜灯起始亮度快照 briNlT */

/* ------------------------------------------------------------------ */
/* 内部工具（led.cpp applyBri / applyFinalBri / stateUpdated）          */
/* ------------------------------------------------------------------ */

/**
 * @brief 施加缩放后的过渡亮度（led.cpp applyBri）。
 *
 * realtime 门控不移植；direct=false 走常规帧节流路径。
 */
static void apply_bri(void)
{
    wledc_strip_set_brightness(s_bri_t, false);
}

/**
 * @brief 立即结束过渡：锚定 bri_old/bri_t 并触发输出（led.cpp applyFinalBri）。
 */
static void apply_final_bri(void)
{
    s_bri_old = s_bri;
    s_bri_t = s_bri;
    apply_bri();
    wledc_strip_trigger();
}

/**
 * @brief 亮度/开关状态变化处理（led.cpp stateUpdated 精简版）。
 * @param now      注入时间戳（millis 语义）
 * @param no_notify 对齐 CALL_MODE_NO_NOTIFY（跳过夜灯重锚定与目标达成判定）
 */
static void state_updated(wledc_time_ms_t now, bool no_notify)
{
    /* 夜灯进行中的手动调整：重锚定进度（led.cpp L94-98） */
    if (!no_notify && s_nl_active &&
        (s_nl_mode == WLEDC_NL_MODE_FADE || s_nl_mode == WLEDC_NL_MODE_COLORFADE)) {
        s_nl_bri_t = s_bri;
        s_nl_delay_ms -= (now - s_nl_start_time);
        s_nl_start_time = now;
    }

    /* 关灯时重置效果时基，保证下次开灯效果从头开始 */
    if (s_bri_t == 0u) {
        wledc_strip_reset_timebase(now); /* NOTIFICATION 门控无对应方，始终执行 */
    }

    if (s_bri > 0u) s_bri_last = s_bri;

    /* 到达目标亮度即视为夜灯完成（SUN 模式不移植） */
    if (!no_notify && s_bri == s_nl_target_bri) s_nl_active = false;

    if (s_transition_duration == 0u) {
        /* 无过渡：直接锚定施加 */
        s_transition_active = false;
        apply_final_bri();
    } else {
        if (s_transition_active) {
            /* 过渡中再次变化：压缩重启（以当前值为新起点） */
            s_bri_old = s_bri_t;
        } else if (s_bri != s_bri_old || s_state_changed) {
            /* 开关切换：效果帧同步淡入淡出 */
            wledc_strip_set_transition_mode(true, now);
        }
        s_transition_active = true;
        s_transition_start_time = now;
    }

    s_state_changed = false;
}

/* ------------------------------------------------------------------ */
/* 公开 API（wled_c_bri.h 契约）                                        */
/* ------------------------------------------------------------------ */

uint8_t wledc_bri(void)
{
    return s_bri;
}

uint8_t wledc_bri_old(void)
{
    return s_bri_old;
}

uint8_t wledc_bri_t(void)
{
    return s_bri_t;
}

uint8_t wledc_bri_last(void)
{
    return s_bri_last;
}

bool wledc_bri_on(void)
{
    return s_bri > 0u;
}

void wledc_bri_mark_state_changed(void)
{
    s_state_changed = true;
}

void wledc_toggle_on_off(void)
{
    /* led.cpp toggleOnOff()：仅翻转状态；调用方需再以
     * wledc_set_brightness(wledc_bri(), now) 触发施加（WLED 中由
     * button.cpp 等调用方调用 stateUpdated）。 */
    if (s_bri == 0u) {
        s_bri = s_bri_last;
        wledc_strip_restart_runtime();
    } else {
        s_bri_last = s_bri;
        s_bri = 0u;
    }
    s_state_changed = true;
}

void wledc_set_brightness(uint8_t brightness, wledc_time_ms_t now)
{
    s_bri = brightness;
    state_updated(now, false);
}

void wledc_handle_transitions(wledc_time_ms_t now)
{
    if (s_transition_active && s_transition_duration > 0u) {
        int ti = (int)(now - s_transition_start_time);
        int tr = (int)s_transition_duration;
        if (ti / tr) { /* 过渡时长耗尽：结束所有段过渡并锚定 */
            wledc_strip_set_transition_mode(false, now);
            s_transition_active = false;
            apply_final_bri();
            return;
        }
        uint8_t bri_to = s_bri_t;
        int delta_bri = (int)s_bri - (int)s_bri_old;
        s_bri_t = (uint8_t)(s_bri_old + (delta_bri * ti / tr));
        if (bri_to != s_bri_t) apply_bri(); /* 值变化才施加 */
    }
}

void wledc_handle_nightlight(wledc_time_ms_t now)
{
    /* 10Hz 限流（含 millis 回绕防护） */
    if (now < 100u && s_nl_last_update > 0u) s_nl_last_update = 0u;
    if (now - s_nl_last_update < 100u) return;
    s_nl_last_update = now;

    if (s_nl_active) {
        if (!s_nl_active_old) { /* 首次执行：快照起始状态 */
            s_nl_start_time = now;
            s_nl_delay_ms = (wledc_time_ms_t)s_nl_delay_mins * 60000u;
            s_nl_active_old = true;
            s_nl_bri_t = s_bri;
        }

        /* 夜灯进度（0.0-1.0+） */
        float nper = (float)(now - s_nl_start_time) / (float)s_nl_delay_ms;

        if (s_nl_mode == WLEDC_NL_MODE_FADE || s_nl_mode == WLEDC_NL_MODE_COLORFADE) {
            /* 亮度线性插值（COLORFADE 的主色插值不移植，颜色归段层） */
            s_bri = (uint8_t)(s_nl_bri_t + (s_nl_target_bri - s_nl_bri_t) * nper);

            /* WLED issue #5620：临时禁用过渡直接施加，避免每 100ms
             * 重启 750ms 过渡导致最终停在目标值之外 */
            uint16_t transition_duration = wledc_transition();
            wledc_set_transition(0u);
            state_updated(now, true); /* colorUpdated(NO_NOTIFY) 亮度等价 */
            wledc_set_transition(transition_duration);
        }

        if (nper >= 1.0f) { /* 时长耗尽：结束 */
            s_nl_active = false;
            if (s_nl_mode == WLEDC_NL_MODE_SET) {
                s_bri = s_nl_target_bri;
                state_updated(now, true); /* colorUpdated(NO_NOTIFY) */
            }
            /* 关灯时恢复夜灯起始亮度，保证 toggle 点亮可恢复 */
            if (s_bri == 0u) s_bri_last = s_nl_bri_t;
            s_nl_active_old = false;
        }
    } else if (s_nl_active_old) { /* 提前反初始化：状态被外部清除 */
        s_nl_active_old = false;
    }
}

esp_err_t wledc_nightlight_begin(wledc_nl_mode_t mode, uint8_t delay_min, uint8_t target_bri)
{
    if (delay_min == 0u) return ESP_ERR_INVALID_ARG;
    if (mode != WLEDC_NL_MODE_SET && mode != WLEDC_NL_MODE_FADE &&
        mode != WLEDC_NL_MODE_COLORFADE) {
        return ESP_ERR_INVALID_ARG;
    }
    s_nl_mode = mode;
    s_nl_delay_mins = delay_min;
    s_nl_target_bri = target_bri;
    s_nl_active = true;
    return ESP_OK;
}

bool wledc_nightlight_active(void)
{
    return s_nl_active;
}

void wledc_set_transition(uint16_t duration_ms)
{
    s_transition_duration = duration_ms;
}

uint16_t wledc_transition(void)
{
    return s_transition_duration;
}

esp_err_t wledc_set_bri_multiplier(uint8_t percent)
{
    if (percent < 1u || percent > 100u) return ESP_ERR_INVALID_ARG;
    s_bri_multiplier = percent;
    return ESP_OK;
}

uint8_t wledc_bri_multiplier(void)
{
    return s_bri_multiplier;
}

uint8_t wledc_scaled_bri(uint8_t in)
{
    unsigned val = ((unsigned)in * (unsigned)s_bri_multiplier) / 100u;
    if (val > 255u) val = 255u;
    return (uint8_t)val;
}