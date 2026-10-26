/*
 * gw_latest.h - 进程内的「最新一帧」快照（M3 实时数据源）
 * ---------------------------------------------------------------------------
 * 为什么需要它（这是 M3 架构的核心决定）
 * --------------------------------------
 * 数据有两条消费路径，对时效的要求完全不同：
 *
 *   网页要「现在多少度」 → 必须是**刚才那一帧**，差 1 秒都能看出来
 *   网页要「最近 1 小时怎么走的」 → 从 SQLite 查，晚几秒无所谓
 *
 * 如果实时值也从 SQLite 读，会有一个致命的时间差：
 *   gw_db 是攒 20 条提交一次事务（为省 fsync），未提交的数据
 *   别的连接**看不见**。1 秒 1 帧 → 网页上的「最新温度」最多滞后 19 秒。
 *
 * 所以：**实时走内存，历史走库。**
 * 解析出一帧就更新这里的快照，HTTP 直接读全局变量 —— 同一个进程、
 * 同一个线程，没有锁、没有拷贝、没有延迟。
 *
 * 内存占用：一个结构体几十字节，全局一份。
 * ---------------------------------------------------------------------------
 */
#ifndef GW_LATEST_H
#define GW_LATEST_H

#include <stdint.h>

#include "gw_frame.h"

typedef struct {
    /* ---- 最新一帧：/api/latest 直接读这些字段 ---- */
    int      valid;         /* 0 = 还没收到过任何帧（网页显示 "--"） */
    int64_t  ts;            /* 收到这一帧的 Unix 秒 */
    uint8_t  node;          /* 节点号 */
    int      temp_x10;      /* 温度 ×10（195 = 19.5C），避免浮点 */
    int      hum_x10;       /* 湿度 ×10 */
    uint16_t lux;           /* 光照 ADC 0~1023 */
    uint16_t gas;           /* 烟雾 ADC 0~1023 */
    uint8_t  lux_pct;       /* 光照折算成 0~100 的相对亮度（方向已判定） */
    uint8_t  pir;           /* 人体红外 0/1 */
    uint8_t  relay;         /* 继电器 0/1 */

    /* ---- M4：数据加工结果（main.c 算好写进来，http_srv 只读） ----
     * gas 是**原始值**，gas_med 是**中值滤波后**的值，两者都要暴露：
     * 网页上并排显示才能看出滤波到底做了什么，光给一个数说服不了人。 */
    uint16_t gas_med;       /* 中值滤波后的 gas */
    uint16_t gas_base;      /* 当前自适应基线 */
    uint16_t gas_pct;       /* gas_med / base ×100，125 = 1.25 倍 */
    uint8_t  alarm_state;   /* ALARM_ST_NORMAL / WARN / ALARM */
    uint16_t alarm_peak;    /* 本次告警峰值 */
    uint32_t alarm_total;   /* 本次运行累计告警次数 */
    uint32_t alarm_active_sec;  /* 本次告警已持续秒数 */

    /* ---- 运行统计：/api/stats，也给日志里的统计行用 ---- */
    int64_t  started_ts;    /* 进程启动时间，算 uptime */
    uint32_t frames;        /* 解析成功的帧数 */
    uint32_t bad;           /* 坏帧（字段校验不过） */
    uint32_t shortf;        /* 残帧（不足 24 字节就断流） */
    uint32_t resync;        /* 重同步时跳过的字节数 */
    uint32_t db_ok;         /* 入库成功条数（本次运行） */
    uint32_t db_fail;       /* 入库失败条数 */
    uint32_t reconnects;    /* 串口重连次数 */
    uint32_t db_rows;       /* 库内总行数，每 5 秒刷新一次（不做实时 COUNT） */
    uint8_t  serial_ok;     /* 1 = 串口已打开；0 = 当前未连接（进程仍在跑，每秒重试） */
} gw_latest_t;

/** @brief 清零并记录启动时间（main 里调用一次） */
void gw_latest_init(void);

/** @brief 取全局快照（永远不返回 NULL） */
gw_latest_t *gw_latest(void);

/**
 * @brief 解析成功一帧时调用：更新快照
 * @param e  解析结果
 * @param ts 收到这一帧的 Unix 秒（由调用方传，保证和落库用的是同一个时间）
 * @note  同时会把 frames 加 1。**不要在别处再重复加**，否则统计会翻倍。
 */
void gw_latest_on_frame(const gw_env_t *e, int64_t ts);

#endif /* GW_LATEST_H */
