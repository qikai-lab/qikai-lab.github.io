/*
 * gw_frame.c - st: 24 字节协议解析实现
 *
 * 核心是"滑动窗口 + 两级重同步"：
 *   1) 帧头（4 字节）不匹配        → 跳到下一个帧头，跳过中间的垃圾
 *   2) 攒够 24 字节但字段非法      → 同样跳到下一个帧头
 *   3) 不足 24 字节且中间冒出新帧头 → 残帧作废，从新帧头重来
 *
 * 为什么不用「逐字节 drop」：串口在射频干扰下经常出现整段垃圾，
 * 逐字节 drop 要循环几千次 memmove，浪费 CPU；直接跳到下一个帧头是 O(n) 一次搞定。
 * 只有在「后面根本没有帧头」时才退化成逐字节丢。
 */
#include "gw_frame.h"

#include <string.h>

/* 帧头 4 字节：'s' 't' ':' 'e' */
static int head_match(const uint8_t *p)
{
    return p[0] == 0x73U && p[1] == 0x74U && p[2] == 0x3AU && p[3] == GW_ENV_TYPE;
}

static uint16_t rd_u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

int gw_env_temp_x10(const gw_env_t *e)
{
    if (e == NULL) {
        return 0;
    }
    return (int)e->temp_int * 10 + (int)e->temp_dec;
}

int gw_env_humi_x10(const gw_env_t *e)
{
    if (e == NULL) {
        return 0;
    }
    return (int)e->hum_int * 10 + (int)e->hum_dec;
}

int gw_env_lux_pct(const gw_env_t *e)
{
    if (e == NULL) {
        return 0;
    }
    /* 1023 → 100%，四舍五入。和 CC2530 屏上 UI 的折算口径保持一致 */
    return ((int)e->lux * 100 + 511) / 1023;
}

int gw_env_sane(const uint8_t *f)
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
    /* 节点号：0 是非法值（固件里从 1 开始分配） */
    if (f[8] == 0U) {
        return 0;
    }
    /* 人体红外只有 0/1 两种取值 */
    if (rd_u16le(f + 12) > 1U) {
        return 0;
    }
    /* 光照 / 烟雾是 10 位 ADC */
    if (rd_u16le(f + 14) > 1023U) {
        return 0;
    }
    if (rd_u16le(f + 18) > 1023U) {
        return 0;
    }
    /* 继电器只有 0/1 */
    if (rd_u16le(f + 20) > 1U) {
        return 0;
    }
    return 1;
}

void gw_env_unpack(gw_env_t *out, const uint8_t *frame)
{
    if (out == NULL || frame == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));

    out->temp_dec = frame[4];
    out->temp_int = frame[5];
    out->hum_dec  = frame[6];
    out->hum_int  = frame[7];
    out->node     = frame[8];
    out->x        = (int8_t)frame[9];
    out->y        = (int8_t)frame[10];
    out->z        = (int8_t)frame[11];
    out->pir      = (uint8_t)rd_u16le(frame + 12);
    out->lux      = rd_u16le(frame + 14);
    /* frame[16..17] = rsv2，暂未使用 */
    out->gas      = rd_u16le(frame + 18);
    out->relay    = (uint8_t)rd_u16le(frame + 20);
    out->adc      = rd_u16le(frame + 22);

    memcpy(out->raw, frame, GW_ENV_FRAME_LEN);
}

void gw_parser_init(gw_parser_t *p, gw_env_cb_t cb, void *user)
{
    if (p == NULL) {
        return;
    }
    memset(p, 0, sizeof(*p));
    p->cb   = cb;
    p->user = user;
}

void gw_parser_reset(gw_parser_t *p)
{
    if (p == NULL) {
        return;
    }
    /* 只动缓冲区。统计量、回调、user 全部保留，见头文件里的说明。 */
    p->len = 0;
    memset(p->buf, 0, sizeof(p->buf));
}

/* 从缓冲区头部丢弃 n 字节 */
static void buf_drop_front(gw_parser_t *p, size_t n)
{
    if (n >= p->len) {
        p->len = 0;
        return;
    }
    memmove(p->buf, p->buf + n, p->len - n);
    p->len -= n;
}

/*
 * 在 buf[0..len) 里找第一个完整帧头的偏移。
 * 返回 SIZE_MAX 表示没找到。
 */
static size_t find_head(const uint8_t *buf, size_t len)
{
    size_t i;

    if (len < GW_ENV_HEAD_LEN) {
        return SIZE_MAX;
    }
    for (i = 0; i + GW_ENV_HEAD_LEN <= len; i++) {
        if (head_match(buf + i)) {
            return i;
        }
    }
    return SIZE_MAX;
}

/*
 * 重同步：从当前位置跳到下一个帧头（找不到就至少前进 1 字节，保证循环能退出）
 * dropped 返回实际丢掉的字节数
 */
static void resync(gw_parser_t *p, uint32_t *dropped)
{
    size_t skip;

    skip = find_head(p->buf + 1, p->len - 1);
    skip = (skip == SIZE_MAX) ? 1U : skip + 1U;
    if (skip > p->len) {
        skip = p->len;
    }
    buf_drop_front(p, skip);
    *dropped += (uint32_t)skip;
}

/* 尽量多地从缓冲区头部解出帧，直到解不动为止 */
static void parser_try(gw_parser_t *p)
{
    for (;;) {

        /* 连一个帧头都放不下：等更多字节 */
        if (p->len < GW_ENV_HEAD_LEN) {
            return;
        }

        /* 1. 帧头不对 → 跳到下一个可能的帧头 */
        if (!head_match(p->buf)) {
            resync(p, &p->stat_resync);
            continue;
        }

        /* 2. 头对了但还没攒够一整帧 */
        if (p->len < GW_ENV_FRAME_LEN) {
            /*
             * 半帧里已经冒出下一个帧头 → 说明这一帧缺字节了（串口丢字节），
             * 保留残帧没有意义，直接跳到新帧头。
             * 不做这一步的话，丢 1 个字节就得等下一帧来"凑够 24 字节"才判错，
             * 白白多丢一帧。
             */
            size_t next = find_head(p->buf + 1, p->len - 1);
            if (next != SIZE_MAX) {
                uint32_t dropped = 0;
                buf_drop_front(p, next + 1);
                dropped = (uint32_t)(next + 1);
                p->stat_resync += dropped;
                p->stat_short++;
                continue;
            }
            return;   /* 正常的半帧，等后续字节 */
        }

        /* 3. 攒够 24 字节，字段校验 */
        if (!gw_env_sane(p->buf)) {
            p->stat_bad++;
            resync(p, &p->stat_resync);
            continue;
        }

        /* 4. 通过，回调。先移出缓冲区再回调，避免回调里重入 */
        {
            gw_env_t e;
            gw_env_unpack(&e, p->buf);
            p->stat_ok++;
            buf_drop_front(p, GW_ENV_FRAME_LEN);

            if (p->cb != NULL) {
                p->cb(&e, p->user);
            }
        }
    }
}

void gw_parser_feed(gw_parser_t *p, const uint8_t *data, size_t n)
{
    size_t i;

    if (p == NULL || data == NULL) {
        return;
    }

    for (i = 0; i < n; i++) {
        /* 缓冲满了：丢掉最旧的一个字节，保证新数据一定能进来 */
        if (p->len >= sizeof(p->buf)) {
            buf_drop_front(p, 1);
            p->stat_resync++;
        }
        p->buf[p->len++] = data[i];

        if (p->len > p->stat_maxlen) {
            p->stat_maxlen = (uint32_t)p->len;
        }
        parser_try(p);
    }
}
