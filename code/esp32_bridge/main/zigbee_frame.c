/*
 * zigbee_frame.c - 24 字节定长帧的滑动窗口解析（纯 C，可在 PC 上编译）
 * ===========================================================================
 * 这个协议没有长度字段、没有 CRC、没有序号，所以解析器只能靠三件事：
 *   1) 4 字节帧头 's','t',':','e'（0x73 0x74 0x3A 0x65）
 *      全匹配的误判率约 1/2^32，实际可以忽略
 *   2) 帧长固定 24 字节 —— 长度靠"定长"确定，不靠字段
 *   3) 字段范围校验（温度 / 湿度 / 节点号 / pir / 继电器 / ADC 上限）
 *
 * 因为是定长 + 无 CRC，**丢一个字节就会把后面全部错位**，
 * 所以必须带"滑窗重同步"：一旦发现当前位置不可能是一帧的开头，
 * 就往前滑，重新找帧头，而不是继续把错位的数据当帧用。
 * ===========================================================================
 */
#include "zigbee_frame.h"

#include <string.h>

#define MAGIC0 0x73U    /* 's' */
#define MAGIC1 0x74U    /* 't' */
#define MAGIC2 0x3AU    /* ':' */
#define MAGIC3 0x65U    /* 'e' */

static inline int head_match(const uint8_t *p)
{
    return p[0] == MAGIC0 && p[1] == MAGIC1 && p[2] == MAGIC2 && p[3] == MAGIC3;
}

uint16_t zb_u16le(const uint8_t *raw)
{
    return (uint16_t)(raw[0] | ((uint16_t)raw[1] << 8));
}

int zb_frame_sane(const uint8_t *f)
{
    if (f == NULL || !head_match(f)) {
        return 0;
    }
    /* 温度：DHT11 量程 0~50℃，留到 60.9 当余量；小数位 0~9 */
    if (f[4] > 9U || f[5] > 60U) {
        return 0;
    }
    /* 湿度：0~100.9 %RH */
    if (f[6] > 9U || f[7] > 100U) {
        return 0;
    }
    /* 节点号从 1 开始分配，0 是非法值 */
    if (f[8] == 0U) {
        return 0;
    }
    /* 人体红外只有 0/1 */
    if (zb_u16le(f + 12) > 1U) {
        return 0;
    }
    /* 光照 / 烟雾是 10 位 ADC */
    if (zb_u16le(f + 14) > 1023U) {
        return 0;
    }
    if (zb_u16le(f + 18) > 1023U) {
        return 0;
    }
    /* 继电器只有 0/1 */
    if (zb_u16le(f + 20) > 1U) {
        return 0;
    }
    return 1;
}

int zb_temp_x10(const uint8_t *raw) { return (int)raw[5] * 10 + (int)raw[4]; }
int zb_humi_x10(const uint8_t *raw) { return (int)raw[7] * 10 + (int)raw[6]; }
uint8_t  zb_node(const uint8_t *raw)  { return raw[8]; }
uint16_t zb_lux(const uint8_t *raw)   { return zb_u16le(raw + 14); }
uint16_t zb_gas(const uint8_t *raw)   { return zb_u16le(raw + 18); }
uint8_t  zb_pir(const uint8_t *raw)   { return zb_u16le(raw + 12) ? 1U : 0U; }
uint8_t  zb_relay(const uint8_t *raw) { return zb_u16le(raw + 20) ? 1U : 0U; }

int zb_lux_pct(const uint8_t *raw)
{
    uint16_t lux = zb_lux(raw);
    if (lux > 1023U) {
        lux = 1023U;
    }
    /* 和 CC2530 屏上显示的口径一致：ADC / 1023 * 100，取整 */
    return (int)(((uint32_t)lux * 100U) / 1023U);
}

void zb_parser_reset(zb_parser_t *p)
{
    if (p == NULL) {
        return;
    }
    memset(p, 0, sizeof(*p));
}

void zb_parser_feed(zb_parser_t *p, const uint8_t *data, size_t n,
                    zb_frame_cb cb, void *user)
{
    size_t i;

    if (p == NULL || data == NULL || n == 0U) {
        return;
    }

    /* 缓冲区一定能装下：每次循环都会把成帧数据立刻消费掉。
       这里只是防一手调用方一次性喂超大数据。 */
    if (p->len + n > sizeof(p->buf)) {
        p->garbage += (uint32_t)p->len;
        p->len = 0;
        if (n > sizeof(p->buf)) {
            /* 单次喂进来的就超缓冲：只留最后 sizeof(buf) 字节 */
            p->garbage += (uint32_t)(n - sizeof(p->buf));
            data += (n - sizeof(p->buf));
            n = sizeof(p->buf);
        }
    }
    memcpy(p->buf + p->len, data, n);
    p->len += n;

    for (;;) {
        if (p->len < ZB_HEAD_LEN) {
            return;                       /* 不够判断帧头 */
        }

        /* 1. 找帧头 */
        int h = -1;
        for (i = 0; i + ZB_HEAD_LEN <= p->len; i++) {
            if (head_match(p->buf + i)) {
                h = (int)i;
                break;
            }
        }
        if (h < 0) {
            /* 一个都没有：只留最后 3 字节（可能是半个帧头） */
            size_t keep = p->len - (ZB_HEAD_LEN - 1U);
            p->garbage += (uint32_t)keep;
            memmove(p->buf, p->buf + keep, ZB_HEAD_LEN - 1U);
            p->len = ZB_HEAD_LEN - 1U;
            return;
        }
        if (h > 0) {
            p->garbage += (uint32_t)h;
            memmove(p->buf, p->buf + h, p->len - (size_t)h);
            p->len -= (size_t)h;
        }

        /* 2. 攒够一帧了吗 */
        if (p->len < ZB_FRAME_LEN) {
            return;
        }

        /* 3. 字段校验：不通过就说明这个"帧头"是假的（或前面丢过字节） */
        if (!zb_frame_sane(p->buf)) {
            p->bad++;
            p->garbage++;
            memmove(p->buf, p->buf + 1, p->len - 1U);
            p->len--;
            continue;
        }

        /* 4. 通过：先把数据移出缓冲，再回调，避免回调里读到被覆盖的内容 */
        {
            uint8_t frame[ZB_FRAME_LEN];
            memcpy(frame, p->buf, ZB_FRAME_LEN);
            memmove(p->buf, p->buf + ZB_FRAME_LEN, p->len - ZB_FRAME_LEN);
            p->len -= ZB_FRAME_LEN;
            p->frames++;
            if (cb != NULL) {
                cb(frame, user);
            }
        }
    }
}
