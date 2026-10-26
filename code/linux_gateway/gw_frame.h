/*
 * gw_frame.h - CC2530 环境数据帧解析（st: 协议，固定 24 字节）
 * ---------------------------------------------------------------------------
 * 这个协议不是我设计的，是 CC2530 固件里 EnvMsg 结构体的原样字节流：
 *
 *   偏移  长度  字段          说明
 *   ----  ----  ------------  --------------------------------------------
 *   0     3     's' 't' ':'   帧头（0x73 0x74 0x3A）
 *   3     1     'e'           帧类型 = 环境数据（0x65）
 *   4     1     temp_dec      温度小数位
 *   5     1     temp_int      温度整数位        → T = temp_int + temp_dec/10
 *   6     1     hum_dec       湿度小数位
 *   7     1     hum_int       湿度整数位        → H = hum_int + hum_dec/10
 *   8     1     node          节点号（1~255）
 *   9     3     x, y, z       三轴，固件未接，恒 0
 *   12    2     pir           人体红外（小端）0=无人 1=有人
 *   14    2     lux           光照 ADC（小端）0~1023
 *   16    2     rsv2          备用，恒 0
 *   18    2     gas           MQ-2 烟雾 ADC（小端）0~1023
 *   20    2     relay         继电器状态（小端，借用 rsv3）0=OFF 1=ON
 *   22    2     adc           未使用，恒 0
 *
 * 与旧版 A5 5A 协议的关键差异（面试会问为什么改）：
 *   1) 这个协议**没有长度字段、没有 CRC、没有序号** —— 帧长固定 24 字节，
 *      长度靠"定长"确定；校验只能靠字段合法性（见 gw_env_sane）。
 *   2) 帧头是 3 字节 ASCII "st:" 再加 1 字节类型，4 字节全匹配的误判率
 *      约 1/2^32，配合字段范围校验，实际误判可以忽略。
 *   3) 因为没有 CRC，**只能检错不能纠错**：校验不过就丢掉这一帧、
 *      滑到下一个帧头，绝不会把坏数据写进数据库。
 *   4) 如果以后要加 CRC，得同时改 CC2530 固件的 EnvMsg 结构体。
 *
 * 解析策略：滑动窗口重同步
 *   - 帧头不匹配 → 直接跳到下一个可能的帧头（不是逐字节丢，省 CPU）
 *   - 攒够 24 字节但字段非法 → 同样跳到下一个帧头
 *   - 不足 24 字节时，若中间已经冒出下一个帧头 → 说明前面这帧缺字节
 *     （串口丢字节），把残帧丢掉，从新帧头重新开始
 */
#ifndef GW_FRAME_H
#define GW_FRAME_H

#include <stdint.h>
#include <stddef.h>

#define GW_ENV_HEAD_LEN     4U      /* "st:" + 类型字节 */
#define GW_ENV_FRAME_LEN    24U     /* 整帧定长 */
#define GW_ENV_TYPE         0x65U   /* 'e' = 环境数据 */

/* 单节点数据结构（解析结果，已按字段拆好，业务层不用再碰偏移） */
typedef struct {
    uint8_t  node;          /* 节点号 */
    uint8_t  temp_int;      /* 温度整数部分 */
    uint8_t  temp_dec;      /* 温度小数部分 */
    uint8_t  hum_int;       /* 湿度整数部分 */
    uint8_t  hum_dec;       /* 湿度小数部分 */
    uint8_t  pir;           /* 0 = 无人, 1 = 有人 */
    uint8_t  relay;         /* 0 = OFF, 1 = ON */
    int8_t   x, y, z;       /* 三轴，当前恒 0 */
    uint16_t lux;           /* 光照原始 ADC 0~1023 */
    uint16_t gas;           /* 烟雾原始 ADC 0~1023 */
    uint16_t adc;           /* 未使用 */
    uint8_t  raw[GW_ENV_FRAME_LEN];   /* 原始 24 字节，入库排障用 */
} gw_env_t;

/* 温度 / 湿度按 x10 定点整数返回（21.1℃ → 211），避免浮点 */
int gw_env_temp_x10(const gw_env_t *e);
int gw_env_humi_x10(const gw_env_t *e);

/* 光照 ADC → 百分比（0~100），和 CC2530 屏上显示的口径一致 */
int gw_env_lux_pct(const gw_env_t *e);

/* 解析成功回调。回调里不要做耗时操作，会阻塞接收 */
typedef void (*gw_env_cb_t)(const gw_env_t *e, void *user);

typedef struct {
    /* 留 4 帧冗余，够放下「一帧半 + 新帧头」这种典型错位场景 */
    uint8_t      buf[GW_ENV_FRAME_LEN * 4];
    size_t       len;

    gw_env_cb_t  cb;
    void        *user;

    /* 统计量：这些是链路健康度指标，M3 会通过 HTTP 暴露出去 */
    uint32_t     stat_ok;         /* 成功解出的帧数 */
    uint32_t     stat_bad;        /* 字段校验失败（假帧头或坏数据） */
    uint32_t     stat_short;      /* 缺字节被下一个帧头顶掉的残帧 */
    uint32_t     stat_resync;     /* 为重新同步而丢弃的字节数 */
    uint32_t     stat_maxlen;     /* 缓冲区历史最高水位（调参用） */
} gw_parser_t;

void gw_parser_init(gw_parser_t *p, gw_env_cb_t cb, void *user);

/**
 * @brief 只清缓冲区、**保留统计量**，用于串口重连后丢弃残留半帧
 *
 * 为什么不能直接用 gw_parser_init()：那个会 memset 整个结构，
 * 把 stat_ok / stat_bad / stat_resync 一起清零 —— 重连后统计就"归零"了，
 * 看起来像链路突然变干净，实际只是数字被抹掉。
 * 串口拔插后，旧 fd 里可能还剩半帧字节，不清掉会和新数据拼成一个假帧
 * （虽然字段校验能拦住，但会多记一次 stat_bad，污染健康度指标）。
 */
void gw_parser_reset(gw_parser_t *p);

/**
 * @brief 向解析器喂任意长度的数据。内部处理粘包 / 半帧 / 帧头失步 / 丢字节
 */
void gw_parser_feed(gw_parser_t *p, const uint8_t *data, size_t n);

/**
 * @brief 校验 24 字节是不是一条合法帧（帧头 + 各字段范围）
 * @return 1 合法 / 0 非法
 * 供单元测试直接调用，也可以给上层做二次校验
 */
int gw_env_sane(const uint8_t *frame);

/**
 * @brief 把 24 字节原始帧拆成 gw_env_t（不校验，调用前先用 gw_env_sane）
 */
void gw_env_unpack(gw_env_t *out, const uint8_t *frame);

#endif /* GW_FRAME_H */
