/*
 * gw_alarm.c - 烟雾告警状态机（实现）
 */
#include <string.h>

#include "gw_alarm.h"

void gw_alarm_init(gw_alarm_t *a)
{
    if (a == NULL) {
        return;
    }
    memset(a, 0, sizeof(*a));
    a->state = ALARM_ST_NORMAL;
}

gw_alarm_ev_t gw_alarm_feed(gw_alarm_t *a, uint16_t gas_filtered,
                            const gw_baseline_t *b, int64_t ts)
{
    uint16_t      base;
    gw_alarm_ev_t ev = ALARM_EV_NONE;

    if (a == NULL) {
        return ALARM_EV_NONE;
    }

    a->gas  = gas_filtered;
    base    = gw_baseline_valid(b) ? gw_baseline_value(b) : 0;
    a->base = base;

    /* 基线不可信：不改状态，但把确认计数清掉。
     * 不清的话，传感器拔掉又插回来，残留的 confirm_cnt 会让它一上电就报警。 */
    if (base < ALARM_BASE_MIN) {
        a->pct         = 0;
        a->confirm_cnt = 0;
        a->clear_cnt   = 0;
        if (a->state == ALARM_ST_WARN) {
            a->state = ALARM_ST_NORMAL;
        }
        return ALARM_EV_NONE;
    }

    /* gas/base ×100，带四舍五入。
     * gas 最大 1023、base 最小 3，用 uint32 算不会溢出。 */
    a->pct = (uint16_t)(((uint32_t)gas_filtered * 100U + (uint32_t)base / 2U)
                        / (uint32_t)base);

    if (a->state == ALARM_ST_ALARM) {
        /* ---- 已在告警：只看什么时候能解除 ---- */
        a->active_sec = (ts > a->alarm_ts) ? (uint32_t)(ts - a->alarm_ts) : 0U;
        if (gas_filtered > a->peak_gas) {
            a->peak_gas = gas_filtered;
        }

        if (a->pct <= ALARM_CLR_PCT) {
            a->clear_cnt++;
            if (a->clear_cnt >= ALARM_CLEAR_N) {
                a->state       = ALARM_ST_NORMAL;
                a->confirm_cnt = 0;
                a->clear_cnt   = 0;
                a->last_sec    = a->active_sec;     /* 留给上层打日志/写库 */
                a->total_sec  += a->active_sec;
                a->active_sec  = 0;
                a->alarm_ts    = 0;
                ev             = ALARM_EV_END;
            }
        } else {
            a->clear_cnt = 0;       /* 回升了：重新计数 */
        }
        return ev;
    }

    /* ---- NORMAL / WARN：看要不要升级 ---- */
    if (a->pct >= ALARM_TRIG_PCT) {
        if (a->state == ALARM_ST_NORMAL) {
            a->state       = ALARM_ST_WARN;
            a->confirm_cnt = 1;
        } else if (a->confirm_cnt < 250) {     /* uint8 上限保护，理论上到不了 */
            a->confirm_cnt++;
        }

        if (a->confirm_cnt >= ALARM_CONFIRM_N) {
            a->state       = ALARM_ST_ALARM;
            a->confirm_cnt = 0;
            a->clear_cnt   = 0;
            a->alarm_ts    = ts;
            a->peak_gas    = gas_filtered;
            a->active_sec  = 0;
            a->total++;
            ev = ALARM_EV_BEGIN;
        }
    } else {
        /* 掉回触发线以下：退回正常，确认计数归零。
         * 这里**不需要**迟滞 —— 迟滞是为了防止"已报警"反复闪，
         * 还没报警就退回是没有副作用的。 */
        a->state       = ALARM_ST_NORMAL;
        a->confirm_cnt = 0;
    }

    return ev;
}

const char *gw_alarm_state_name(uint8_t st)
{
    switch (st) {
    case ALARM_ST_NORMAL: return "normal";
    case ALARM_ST_WARN:   return "warn";
    case ALARM_ST_ALARM:  return "alarm";
    default:              return "unknown";
    }
}
