/*
 * test_frame.c - 解析层的离线断言（纯 C，PC 上 gcc 直接跑，不需要 ESP32）
 * ===========================================================================
 * 编译：
 *   gcc -std=c11 -Wall -Wextra -I../main -o test_frame.exe test_frame.c ../main/zigbee_frame.c
 * 运行：
 *   ./test_frame.exe
 *
 * 覆盖的场景全部来自真实会遇到的串口情况：
 *   上电毛刺、逐字节到达、丢字节、半帧、假帧头、随机切分、真实抓包
 * ===========================================================================
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "zigbee_frame.h"

static int g_pass;
static int g_fail;

static void check(int cond, const char *what, long got, long want)
{
    if (cond) {
        g_pass++;
        printf("  [ OK ] %s（%ld）\n", what, got);
    } else {
        g_fail++;
        printf("  [FAIL] %s（得到 %ld，期望 %ld）\n", what, got, want);
    }
}

/* 造一帧：温度 19.1℃、湿度 64.0%、节点 1、光照 577、烟雾 24 */
static void make_frame(uint8_t *f, int temp_dec, int temp_int,
                       int hum_dec, int hum_int, uint8_t node,
                       uint16_t pir, uint16_t lux, uint16_t gas, uint16_t rly)
{
    memset(f, 0, ZB_FRAME_LEN);
    f[0] = 0x73; f[1] = 0x74; f[2] = 0x3A; f[3] = 0x65;
    f[4] = (uint8_t)temp_dec;
    f[5] = (uint8_t)temp_int;
    f[6] = (uint8_t)hum_dec;
    f[7] = (uint8_t)hum_int;
    f[8] = node;
    f[12] = (uint8_t)(pir & 0xFF);
    f[13] = (uint8_t)(pir >> 8);
    f[14] = (uint8_t)(lux & 0xFF);
    f[15] = (uint8_t)(lux >> 8);
    f[18] = (uint8_t)(gas & 0xFF);
    f[19] = (uint8_t)(gas >> 8);
    f[20] = (uint8_t)(rly & 0xFF);
    f[21] = (uint8_t)(rly >> 8);
}

/* 回调里把帧拷贝出来，方便断言 */
typedef struct {
    uint8_t frames[32][ZB_FRAME_LEN];
    int     n;
} capture_t;

static void cap_cb(const uint8_t *raw, void *user)
{
    capture_t *c = (capture_t *)user;
    if (c->n < 32) {
        memcpy(c->frames[c->n], raw, ZB_FRAME_LEN);
    }
    c->n++;
}

int main(void)
{
    zb_parser_t p;
    capture_t   cap;
    uint8_t     a[ZB_FRAME_LEN], b[ZB_FRAME_LEN], buf[128];
    size_t      i, k;

    printf("========================================\n");
    printf(" ESP32 解析层离线测试（st: 24 字节）\n");
    printf("========================================\n");

    /* ---------------- 用例 1：两帧一次喂完 ---------------- */
    printf("\n[用例 1] 两帧完整数据，一次喂完\n");
    make_frame(a, 1, 19, 0, 64, 1, 0, 577, 26, 0);
    make_frame(b, 0, 21, 0, 51, 1, 1, 858, 29, 1);
    memcpy(buf, a, ZB_FRAME_LEN);
    memcpy(buf + ZB_FRAME_LEN, b, ZB_FRAME_LEN);

    zb_parser_reset(&p);
    memset(&cap, 0, sizeof(cap));
    zb_parser_feed(&p, buf, ZB_FRAME_LEN * 2, cap_cb, &cap);

    check(cap.n == 2, "解出帧数", cap.n, 2);
    check(zb_temp_x10(cap.frames[0]) == 191, "帧A 温度 x10", zb_temp_x10(cap.frames[0]), 191);
    check(zb_humi_x10(cap.frames[0]) == 640, "帧A 湿度 x10", zb_humi_x10(cap.frames[0]), 640);
    check(zb_lux(cap.frames[0]) == 577, "帧A 光照", zb_lux(cap.frames[0]), 577);
    check(zb_gas(cap.frames[0]) == 26, "帧A 烟雾", zb_gas(cap.frames[0]), 26);
    check(zb_lux_pct(cap.frames[0]) == 56, "帧A 光照百分比", zb_lux_pct(cap.frames[0]), 56);
    check(zb_pir(cap.frames[1]) == 1, "帧B 有人", zb_pir(cap.frames[1]), 1);
    check(zb_relay(cap.frames[1]) == 1, "帧B 继电器 ON", zb_relay(cap.frames[1]), 1);
    check(p.bad == 0, "坏帧数", (long)p.bad, 0);
    check(p.garbage == 0, "无效字节数", (long)p.garbage, 0);

    /* ---------------- 用例 2：逐字节到达 ---------------- */
    printf("\n[用例 2] 逐字节喂入（每次 1 字节，模拟慢速串口）\n");
    zb_parser_reset(&p);
    memset(&cap, 0, sizeof(cap));
    for (i = 0; i < ZB_FRAME_LEN * 2; i++) {
        zb_parser_feed(&p, buf + i, 1, cap_cb, &cap);
    }
    check(cap.n == 2, "解出帧数", cap.n, 2);
    check(zb_temp_x10(cap.frames[1]) == 210, "第 2 帧温度 x10", zb_temp_x10(cap.frames[1]), 210);

    /* ---------------- 用例 3：帧前有垃圾字节 ---------------- */
    printf("\n[用例 3] 帧前有 8 字节垃圾（串口上电毛刺）\n");
    memset(buf, 0xAA, 8);
    memcpy(buf + 8, a, ZB_FRAME_LEN);
    zb_parser_reset(&p);
    memset(&cap, 0, sizeof(cap));
    zb_parser_feed(&p, buf, 8 + ZB_FRAME_LEN, cap_cb, &cap);
    check(cap.n == 1, "解出帧数", cap.n, 1);
    check(p.garbage == 8, "丢弃的垃圾字节数", (long)p.garbage, 8);
    check(zb_temp_x10(cap.frames[0]) == 191, "温度仍然正确", zb_temp_x10(cap.frames[0]), 191);

    /* ---------------- 用例 4：第一帧中间丢 1 字节 ---------------- */
    printf("\n[用例 4] 第一帧中间丢 1 字节（串口丢字节）\n");
    memcpy(buf, a, 10);                          /* 只取前 10 字节 */
    memcpy(buf + 10, b, ZB_FRAME_LEN);           /* 紧接着完整第二帧 */
    zb_parser_reset(&p);
    memset(&cap, 0, sizeof(cap));
    zb_parser_feed(&p, buf, 10 + ZB_FRAME_LEN, cap_cb, &cap);
    check(cap.n == 1, "只解出 1 帧（坏的那帧被丢）", cap.n, 1);
    check(zb_temp_x10(cap.frames[0]) == 210, "救回来的是第二帧", zb_temp_x10(cap.frames[0]), 210);

    /* ---------------- 用例 5：假帧头（字段越界） ---------------- */
    printf("\n[用例 5] 假帧头（帧头对得上，但温度小数位 = 153）\n");
    make_frame(buf, 153, 19, 0, 64, 1, 0, 577, 26, 0);
    memcpy(buf + ZB_FRAME_LEN, b, ZB_FRAME_LEN);
    zb_parser_reset(&p);
    memset(&cap, 0, sizeof(cap));
    zb_parser_feed(&p, buf, ZB_FRAME_LEN * 2, cap_cb, &cap);
    check(cap.n == 1, "只接受后面那帧合法数据", cap.n, 1);
    check(p.bad >= 1, "坏帧计数", (long)p.bad, 1);
    check(zb_temp_x10(cap.frames[0]) == 210, "留下的是第二帧", zb_temp_x10(cap.frames[0]), 210);

    /* ---------------- 用例 6：真实串口抓包 ---------------- */
    printf("\n[用例 6] 真实串口抓包（od 抓到的两帧）\n");
    {
        static const uint8_t real[48] = {
            0x73,0x74,0x3A,0x65, 0x01,0x13, 0x00,0x40, 0x01, 0x00,0x00,0x00,
            0x00,0x00, 0x41,0x02, 0x00,0x00, 0x18,0x00, 0x00,0x00, 0x00,0x00,
            0x73,0x74,0x3A,0x65, 0x05,0x13, 0x00,0x4C, 0x01, 0x00,0x00,0x00,
            0x00,0x00, 0x41,0x02, 0x00,0x00, 0x18,0x00, 0x00,0x00, 0x00,0x00
        };
        zb_parser_reset(&p);
        memset(&cap, 0, sizeof(cap));
        zb_parser_feed(&p, real, sizeof(real), cap_cb, &cap);
        check(cap.n == 2, "解出帧数", cap.n, 2);
        check(zb_temp_x10(cap.frames[0]) == 191, "帧1 温度 19.1", zb_temp_x10(cap.frames[0]), 191);
        check(zb_humi_x10(cap.frames[0]) == 640, "帧1 湿度 64.0", zb_humi_x10(cap.frames[0]), 640);
        check(zb_lux(cap.frames[0]) == 577, "帧1 光照 577", zb_lux(cap.frames[0]), 577);
        check(zb_gas(cap.frames[0]) == 24, "帧1 烟雾 24", zb_gas(cap.frames[0]), 24);
        check(zb_temp_x10(cap.frames[1]) == 195, "帧2 温度 19.5", zb_temp_x10(cap.frames[1]), 195);
        check(zb_humi_x10(cap.frames[1]) == 760, "帧2 湿度 76.0", zb_humi_x10(cap.frames[1]), 760);
        check(p.bad == 0, "坏帧数", (long)p.bad, 0);
        check(p.garbage == 0, "无效字节数", (long)p.garbage, 0);
    }

    /* ---------------- 用例 7：随机切分 ---------------- */
    printf("\n[用例 7] 三帧被切成 11 段随机喂入\n");
    {
        static const size_t cuts[12] = { 0, 7, 15, 23, 24, 31, 40, 48, 55, 63, 70, 72 };
        make_frame(buf,          1, 19, 0, 64, 1, 0, 577, 26, 0);
        make_frame(buf + 24,     2, 20, 1, 65, 1, 1, 578, 27, 1);
        make_frame(buf + 48,     3, 21, 2, 66, 2, 0, 579, 28, 0);
        zb_parser_reset(&p);
        memset(&cap, 0, sizeof(cap));
        for (k = 0; k + 1 < 12; k++) {
            zb_parser_feed(&p, buf + cuts[k], cuts[k + 1] - cuts[k], cap_cb, &cap);
        }
        check(cap.n == 3, "解出帧数", cap.n, 3);
        check(zb_node(cap.frames[2]) == 2, "第 3 帧节点号", zb_node(cap.frames[2]), 2);
    }

    /* ---------------- 用例 8：一直没有帧头 ---------------- */
    printf("\n[用例 8] 全是垃圾，一个帧头都没有\n");
    memset(buf, 0x5A, 60);
    zb_parser_reset(&p);
    memset(&cap, 0, sizeof(cap));
    zb_parser_feed(&p, buf, 60, cap_cb, &cap);
    check(cap.n == 0, "解出帧数", cap.n, 0);
    check(p.bad == 0, "坏帧数（没有帧头就不算坏帧）", (long)p.bad, 0);
    check(p.len == 3, "缓冲区只留 3 字节（半个帧头的余量）", (long)p.len, 3);

    /* ---------------- 用例 9：字段边界 ---------------- */
    printf("\n[用例 9] 字段边界：刚好合法 / 刚好越界\n");
    make_frame(buf, 9, 60, 9, 100, 255, 1, 1023, 1023, 1);
    check(zb_frame_sane(buf) == 1, "上限值全部合法", zb_frame_sane(buf), 1);
    make_frame(buf, 0, 61, 0, 64, 1, 0, 577, 26, 0);
    check(zb_frame_sane(buf) == 0, "温度 61℃ 越界", zb_frame_sane(buf), 0);
    make_frame(buf, 0, 19, 0, 101, 1, 0, 577, 26, 0);
    check(zb_frame_sane(buf) == 0, "湿度 101% 越界", zb_frame_sane(buf), 0);
    make_frame(buf, 0, 19, 0, 64, 0, 0, 577, 26, 0);
    check(zb_frame_sane(buf) == 0, "节点号 0 非法", zb_frame_sane(buf), 0);
    make_frame(buf, 0, 19, 0, 64, 1, 2, 577, 26, 0);
    check(zb_frame_sane(buf) == 0, "pir=2 非法", zb_frame_sane(buf), 0);
    make_frame(buf, 0, 19, 0, 64, 1, 0, 1024, 26, 0);
    check(zb_frame_sane(buf) == 0, "光照 1024 越界", zb_frame_sane(buf), 0);

    printf("\n========================================\n");
    if (g_fail == 0) {
        printf(" 通过 %d 项，失败 0 项 —— 全部通过\n", g_pass);
    } else {
        printf(" 通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    }
    printf("========================================\n");

    return g_fail == 0 ? 0 : 1;
}
