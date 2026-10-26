/*
 * gw_alarm.h - 烟雾告警状态机（M4）
 * ---------------------------------------------------------------------------
 * 判定为什么不用「gas > 600 就报警」
 * ----------------------------------
 * MQ-2 的 ADC 原始值没有绝对意义。它受四个东西影响：
 *   温度、湿度、加热丝老化、传感器本身的个体差异。
 * 同一个房间，冬天洁净空气读 35，夏天可能读到 60；
 * 换一块 MQ-2 模块，同样空气可能读 80。
 *
 * 所以判定**只用相对值**：gas 涨到基线的多少倍。
 * 基线由 gw_filter.c 里的自适应基线提供，会跟着环境慢慢漂。
 * 好处是换板子、换季节都不用改代码；代价是必须有两个模块配合。
 *
 * 三档状态 + 双阈值（迟滞）
 * -------------------------
 *   NORMAL ──gas≥2.5倍──> WARN ──连续3帧──> ALARM
 *      ^                                      │
 *      └──────连续10帧≤1.5倍──────────────────┘
 *
 * 为什么要有 WARN 这一档
 *   单帧越线很可能是干扰（水汽、开关电源的瞬时负载）。加一档「疑似」
 *   要求连续 3 帧确认，把偶发尖峰挡在门外。3 秒的延迟对烟雾告警无所谓，
 *   但能大幅降低误报。
 *
 * 为什么要迟滞（触发 2.5 倍 / 解除 1.5 倍）
 *   如果进出都用同一个阈值，gas 在 2.5 倍附近抖动时会疯狂反复报。
 *   把解除线拉低到 1.5 倍，中间 1.5~2.5 倍是「死区」，
 *   一旦报警就必须明显降下来才解除。
 *----------------------------------------------------------------------------
 */
#ifndef GW_ALARM_H
#define GW_ALARM_H

#include <stdint.h>

#include "gw_filter.h"

/* ---- 状态 ---- */
#define ALARM_ST_NORMAL     0   /* 正常 */
#define ALARM_ST_WARN       1   /* 疑似：已越触发线，正在连续确认 */
#define ALARM_ST_ALARM      2   /* 告警：已确认 */

/* ---- 参数（都按基线百分比，不按绝对值） ---- */
#define ALARM_TRIG_PCT      250     /* ≥ 基线 250% 算疑似烟雾 */
#define ALARM_CLR_PCT       150     /* ≤ 基线 150% 才考虑解除 */
#define ALARM_CONFIRM_N     3       /* 连续 3 帧越线 → 确认 */
#define ALARM_CLEAR_N       10      /* 连续 10 帧回落 → 解除 */

/* 基线低于这个值时认为不可信（传感器没接、ADC 读 0）。
 * gas 也是 0，比值无意义，必须跳过判定，否则 0/0 会除零。 */
#define ALARM_BASE_MIN      3

typedef struct {
    /* ---- 对外展示 ---- */
    uint8_t  state;         /* ALARM_ST_xxx */
    uint8_t  confirm_cnt;   /* 连续越线帧数 */
    uint8_t  clear_cnt;     /* 连续回落帧数 */
    uint16_t gas;           /* 滤波后的 gas（不是原始值） */
    uint16_t base;          /* 当前基线 */
    uint16_t pct;           /* gas / base ×100，网页显示"现在 1.4 倍" */
    uint16_t peak_gas;      /* 本次告警期间的最高 gas */

    /* ---- 计数 ---- */
    int64_t  alarm_ts;      /* 本次告警开始时间；0 = 没在告警 */
    uint32_t active_sec;    /* 本次告警已持续秒数（告警中才有值） */
    uint32_t last_sec;      /* 最近一次告警的持续秒数（解除后保留） */
    uint32_t total;         /* 累计告警次数（本次运行） */
    uint32_t total_sec;     /* 累计告警总时长（本次运行） */
} gw_alarm_t;

/** @brief 状态机返回的事件，让调用方决定要不要写库 */
typedef enum {
    ALARM_EV_NONE = 0,      /* 状态没变 */
    ALARM_EV_BEGIN,         /* 刚刚进入告警（写一条 OPEN 记录） */
    ALARM_EV_END            /* 刚刚解除（补上 END 记录） */
} gw_alarm_ev_t;

/** @brief 清零 */
void gw_alarm_init(gw_alarm_t *a);

/**
 * @brief 喂一帧，推进状态机
 * @param a             状态机
 * @param gas_filtered  中值滤波后的 gas（**不要传原始值**，见 gw_filter.h）
 * @param b             自适应基线（上一步算出来的）
 * @param ts            这一帧的 Unix 秒
 * @return 状态变化事件
 * @note  调用顺序必须是「先喂告警、再更新基线」，
 *        这样判触发时用的是烟雾到来之前的基线。
 *        否则基线会被这一帧的高值带跑，2.5 倍永远够不着。
 */
gw_alarm_ev_t gw_alarm_feed(gw_alarm_t *a, uint16_t gas_filtered,
                            const gw_baseline_t *b, int64_t ts);

/** @brief "normal" / "warn" / "alarm"，给 JSON 用 */
const char *gw_alarm_state_name(uint8_t st);

#endif /* GW_ALARM_H */
