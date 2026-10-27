/*
 * zigbee_frame.h - CC2530 环境数据帧解析（纯 C，不依赖 ESP-IDF）
 * ===========================================================================
 * 单独抽出来的原因：
 *   这段逻辑是"能离线测的部分"。上板子跑一轮要接线、要开两个串口，一分钟；
 *   在本机 gcc 编一遍跑断言，一秒。所以把它和 ESP-IDF 的 UART 代码分开，
 *   改一行就能立刻验一遍，不用烧固件。
 *
 * 协议与 Linux 网关 gw_frame.h 完全一致，改这里必须两边一起改。
 * ===========================================================================
 */
#ifndef ZIGBEE_FRAME_H
#define ZIGBEE_FRAME_H

#include <stdint.h>
#include <stddef.h>

#define ZB_FRAME_LEN   24U
#define ZB_HEAD_LEN    4U
#define ZB_TYPE_ENV    0x65U    /* 'e' */

/* 解析器状态。调用方自己分配，不需要动态内存 */
typedef struct {
    uint8_t  buf[ZB_FRAME_LEN * 4];   /* 4 帧冗余，够处理"一帧半 + 新帧头" */
    size_t   len;

    uint32_t frames;    /* 校验通过的帧数 */
    uint32_t bad;       /* 帧头对上但字段越界的帧数 */
    uint32_t garbage;   /* 不属于任何一帧的字节数（含残帧） */
} zb_parser_t;

/* 出帧回调。raw 指向 24 字节，仅在回调期间有效 */
typedef void (*zb_frame_cb)(const uint8_t *raw, void *user);

void zb_parser_reset(zb_parser_t *p);

/* 喂任意长度、任意切分的数据，内部自动重同步 */
void zb_parser_feed(zb_parser_t *p, const uint8_t *data, size_t n,
                    zb_frame_cb cb, void *user);

/* ---- 字段读取小工具（全部按小端 / 定点，避免浮点） ---- */
uint16_t zb_u16le(const uint8_t *raw);
int      zb_frame_sane(const uint8_t *raw);   /* 1 = 字段合法 */
int      zb_temp_x10(const uint8_t *raw);     /* 191 = 19.1℃ */
int      zb_humi_x10(const uint8_t *raw);     /* 640 = 64.0% */
int      zb_lux_pct(const uint8_t *raw);      /* 0~100，和屏上口径一致 */
uint8_t  zb_node(const uint8_t *raw);
uint16_t zb_lux(const uint8_t *raw);
uint16_t zb_gas(const uint8_t *raw);
uint8_t  zb_pir(const uint8_t *raw);
uint8_t  zb_relay(const uint8_t *raw);

#endif /* ZIGBEE_FRAME_H */
