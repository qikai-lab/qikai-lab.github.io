/*
 * gw_db.h - SQLite 本地落库（M2）
 * ---------------------------------------------------------------------------
 * 设计要点（面试会问）：
 *   1) WAL 模式 —— 读写不互斥，M3 的 HTTP 线程查询时不会卡住采集写入
 *   2) synchronous=NORMAL —— WAL 下只保证 checkpoint 时 fsync，
 *      掉电最多丢最后几条，换来约 10 倍写入速度。环境数据 1 秒 1 条，
 *      丢一两条无所谓，但绝不能用 MEMORY（整库都可能丢）。
 *   3) 显式事务 + 批量提交 —— 每条都 autocommit 会触发一次 WAL 提交，
 *      20 条一起提交摊薄开销。代价是崩溃最多丢 batch 条。
 *   4) 预处理语句复用 —— 不在循环里 sqlite3_prepare，避免反复解析 SQL。
 *   5) 存 temp_x10 / hum_x10（定点整数），不存浮点 —— 避免 21.1 存成
 *      21.099999 这类浮点误差，查询和绘图都干净。
 *   6) 额外存 raw 原始 24 字节 —— 出问题能回溯到串口原始数据，
 *      这是工业网关的常规做法（现场数据不可复现，必须留证据）。
 *
 * 表结构（M3 的 /api/history 直接查它）：
 *   env_data(id, ts, node, temp_x10, hum_x10, lux, gas, pir, relay, raw)
 *   ts = Unix 秒（wall clock，给人看的，不是单调时钟）
 *
 * M4 增加一张告警表：
 *   alarm(id, node, start_ts, end_ts, base_gas, trig_gas, peak_gas)
 *   end_ts 为 NULL 表示「还在告警中」。
 *
 *   为什么不把告警状态塞进 env_data 当一列：
 *   告警是**事件**，不是**采样**。一次告警横跨几十秒、几百条采样，
 *   写进每一行会重复几百遍，而且查「本月报警几次」要写 GROUP BY。
 *   单独一张表，一次告警一行，查询和展示都直接。
 */
#ifndef GW_DB_H
#define GW_DB_H

#include <stdint.h>

#include "gw_frame.h"

#define GW_DB_DEFAULT_BATCH   20      /* 攒够 20 条提交一次（约 20 秒） */

typedef struct gw_db gw_db_t;

/**
 * @brief 打开（或创建）数据库并建表
 * @param path  .db 文件路径
 * @return 句柄；失败返回 NULL（调用方应降级为「只打印不落库」，不能因此退出）
 */
gw_db_t *gw_db_open(const char *path);

/** 设置批量提交条数（0 或负数 = 每条立即提交） */
void gw_db_set_batch(gw_db_t *db, int batch);

/**
 * @brief 插入一条环境数据
 * @return 0 成功；-1 失败（可用 gw_db_last_error 取原因）
 *         失败不会让进程退出，主循环继续采集
 */
int gw_db_insert(gw_db_t *db, const gw_env_t *e, int64_t ts);

/** 强制提交当前事务（退出前、或想立刻看到数据时调） */
int gw_db_commit(gw_db_t *db);

int64_t gw_db_total_rows(gw_db_t *db);
int64_t gw_db_rows_of_node(gw_db_t *db, int node);

/**
 * @brief 统计总行数，**不触发事务提交**（给 HTTP 用）
 * @note  和 gw_db_total_rows 的区别：那个会先 commit 再数，
 *        HTTP 每 2 秒查一次会把批量提交废掉。这个纯查不提交。
 */
int64_t gw_db_count_rows(gw_db_t *db);

/** 最近一条的 ts，没有数据返回 -1 */
int64_t gw_db_last_ts(gw_db_t *db);

/*------------------------------------------------------------------------------
 * 历史查询（M3 的 /api/history 用）
 *
 * 为什么不返回 JSON、只返回行数组
 * -------------------------------
 * 数据库层只管「取数」，不做「格式化」。以后要换 HTTP 也好、换 MQTT 也好、
 * 导出 CSV 也好，都从同一个 gw_db_recent 拿数据，避免 JSON 逻辑散在各处。
 *----------------------------------------------------------------------------*/

/** 一行历史数据（字段和表一一对应，温度湿度仍是 ×10 定点） */
typedef struct {
    int64_t ts;
    int     node;
    int     temp_x10;
    int     hum_x10;
    int     lux;
    int     gas;
    int     pir;
    int     relay;
} gw_row_t;

/**
 * @brief 取最近 N 条，**按时间正序**输出（最老的在前，方便直接画曲线）
 * @param limit 最多取多少条（<=0 用默认 60，上限 200）
 * @param node  0 = 不限节点；否则只取该节点
 * @param out   调用方提供的数组
 * @param max   out 能放多少行
 * @return 实际写入的行数（失败返回 -1）
 * @note  内部是「先 DESC 取最新 N 条，再在内存里倒序」——
 *        比写嵌套子查询 + 外层 ASC 更直观，而且能复用同一套列绑定。
 */
int gw_db_recent(gw_db_t *db, int limit, int node, gw_row_t *out, int max);

/*------------------------------------------------------------------------------
 * 历史趋势：按时间桶聚合（页面 1h / 24h 曲线用）
 *
 * 为什么不复用 gw_db_recent
 * -------------------------
 *   recent 是「最近 N 条原始行」。1 秒 1 帧的话，1 小时是 3600 行、
 *   24 小时是 86400 行 —— 网页画不了这么多点，HTTP 响应也塞不下。
 *   趋势图要的是「每 N 秒一个平均」：**点数只跟窗口/桶宽有关，
 *   跟采样率无关**，掉帧也不会让点数暴涨。
 *
 * 为什么在 SQL 里聚合、而不是取回来在内存里算
 * ------------------------------------------
 *   1) 86400 行搬到用户态再分组，光拷贝就比 SQL 慢一个量级；
 *   2) 聚合后只剩 96~120 行，静态数组就够，不用动态分配；
 *   3) WHERE ts 范围走 idx_env_ts 索引，执行计划是确定的。
 *----------------------------------------------------------------------------*/

/** 一个时间桶的聚合结果（温度湿度仍是 ×10 定点） */
typedef struct {
    int64_t ts;         /* 桶起点（ts - ts % bucket） */
    int     n;          /* 桶里有多少原始行：掉线时这个数会明显变小 */
    int     temp_x10;   /* 平均温度 ×10 */
    int     hum_x10;    /* 平均湿度 ×10 */
    int     tmin_x10;   /* 桶内最低温 —— 曲线可以画波动带 */
    int     tmax_x10;   /* 桶内最高温 */
    int     lux;        /* 平均光照 */
    int     gas;        /* 平均烟雾 */
    int     gas_max;    /* 桶内最高烟雾：告警看峰值，平均值会把尖峰抹平 */
    int     pir;        /* 桶内「有人」帧数，0 = 一直没人 */
} gw_trend_t;

/**
 * @brief 把 [from_ts, to_ts] 按 bucket_sec 切桶聚合，**时间正序**输出
 * @param bucket_sec 桶宽（秒），<=0 按 60 处理
 * @param node       0 = 不限节点；否则只统计该节点
 * @param out        调用方提供的数组
 * @param max        out 能放多少桶（超出部分丢弃）
 * @return 实际桶数（1h/24h 通常是 120 / 96）；失败返回 -1
 * @note   **只返回有数据的桶，中间断档不补 0**。
 *         补 0 会在曲线上画出一个假的「温度 0 度 / 光照 0」，
 *         比留个缺口更容易让人误判成设备故障。
 *         前端靠相邻桶的时间差是否超过 2.5 倍桶宽来决定「把线断开」。
 */
int gw_db_trend(gw_db_t *db, int64_t from_ts, int64_t to_ts, int bucket_sec,
                int node, gw_trend_t *out, int max);

/*------------------------------------------------------------------------------
 * M4：告警事件表
 *----------------------------------------------------------------------------*/

/** 一行告警记录 */
typedef struct {
    int     id;
    int     node;
    int64_t start_ts;       /* 告警开始 */
    int64_t end_ts;         /* 结束；0 = 还在告警中 */
    int     base_gas;       /* 触发时的基线 */
    int     trig_gas;       /* 触发那一帧的 gas */
    int     peak_gas;       /* 期间最高 gas */
} gw_alarm_row_t;

/**
 * @brief 开始一次告警，插一行
 * @return 新行 id（>0）；失败返回 0
 * @note   id 在 gw_db_alarm_end 时用来定位。存在内存里，进程重启就丢，
 *         但重启后本来也没有告警在跑（启动时会清理遗留的未结束记录）。
 */
int gw_db_alarm_begin(gw_db_t *db, int node, int64_t ts,
                      int base_gas, int trig_gas);

/** @brief 结束一次告警：补 end_ts 和 peak_gas。失败返回 -1 */
int gw_db_alarm_end(gw_db_t *db, int id, int64_t ts, int peak_gas);

/**
 * @brief 最近 N 条告警，**时间倒序**（最新的在前）
 * @note  和 gw_db_recent 相反 —— 历史曲线要正序画图，
 *        告警列表人看的是"最近出过什么事"，必须最新在上。
 */
int gw_db_alarm_recent(gw_db_t *db, int limit, gw_alarm_row_t *out, int max);

/** @brief 告警总次数（不含还在进行中的） */
int64_t gw_db_alarm_count(gw_db_t *db);

const char *gw_db_last_error(const gw_db_t *db);
uint32_t    gw_db_err_count(const gw_db_t *db);

void gw_db_close(gw_db_t *db);

#endif /* GW_DB_H */
