/* 测试：gw_filter（中值滤波 + 自适应基线）与 gw_alarm（状态机）
 * 这两个模块不依赖 sqlite / socket，MinGW 直接编。 */
#include <stdio.h>
#include <string.h>

#include "gw_filter.h"
#include "gw_alarm.h"

static int fails = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  [OK]   %s\n", msg); } \
    else { printf("  [FAIL] %s\n", msg); fails++; } \
} while (0)

static void t_medfilt(void)
{
    gw_medfilt_t f;
    uint16_t     v;

    printf("\n[1] 中值滤波：孤立尖峰必须被吃掉\n");
    gw_medfilt_init(&f);
    gw_medfilt_push(&f, 22);
    gw_medfilt_push(&f, 22);
    gw_medfilt_push(&f, 22);
    gw_medfilt_push(&f, 300);          /* 一帧尖峰 */
    v = gw_medfilt_push(&f, 22);
    printf("     窗口 22 22 22 300 22 -> 中值 %u\n", (unsigned)v);
    CHECK(v == 22, "孤立尖峰被挤出（22 而不是 300）");

    printf("\n[2] 中值滤波：真实烟雾（连续高值）必须透传\n");
    gw_medfilt_init(&f);
    gw_medfilt_push(&f, 22);
    gw_medfilt_push(&f, 22);
    v = gw_medfilt_push(&f, 300);
    v = gw_medfilt_push(&f, 300);
    v = gw_medfilt_push(&f, 300);
    printf("     窗口 22 22 300 300 300 -> 中值 %u\n", (unsigned)v);
    CHECK(v == 300, "持续烟雾 3 帧后就报 300");

    printf("\n[3] 中值滤波：窗口没满时返回已填数据的中间值，不能是 0\n");
    gw_medfilt_init(&f);
    v = gw_medfilt_push(&f, 40);
    CHECK(v == 40, "第 1 帧就返回 40（不是 0）");
    v = gw_medfilt_push(&f, 42);
    CHECK(v == 42, "第 2 帧返回较大的那个（42），仍不是 0");
    v = gw_medfilt_push(&f, 41);
    CHECK(v == 41, "第 3 帧返回正中间 41");

    printf("\n[4] 中值滤波：交替抖动（22/23 反复）不能偏\n");
    gw_medfilt_init(&f);
    {
        int i;
        for (i = 0; i < 20; i++) {
            gw_medfilt_push(&f, (uint16_t)((i % 2) ? 23 : 22));
        }
        v = gw_medfilt_value(&f);
        printf("     抖动 20 帧后中值 %u\n", (unsigned)v);
        CHECK(v == 22 || v == 23, "中值稳定在 22/23，没被拉走");
    }
}

static void t_baseline(void)
{
    gw_baseline_t b;
    int           i;

    printf("\n[5] 基线：第一帧直接取该值\n");
    gw_baseline_init(&b, 10);
    CHECK(gw_baseline_valid(&b) == 0, "初始化后 valid=0");
    gw_baseline_update(&b, 35, 0);
    CHECK(gw_baseline_valid(&b) == 1, "喂一帧后 valid=1");
    CHECK(gw_baseline_value(&b) == 35, "基线 = 35");

    printf("\n[6] 基线：从 35 收敛到 100（tau=10，跑 200 帧）\n");
    for (i = 0; i < 200; i++) {
        gw_baseline_update(&b, 100, 0);
    }
    printf("     200 帧后基线 = %u\n", (unsigned)gw_baseline_value(&b));
    CHECK(gw_baseline_value(&b) >= 99 && gw_baseline_value(&b) <= 101,
          "基线收敛到 100 附近");

    printf("\n[7] 基线：整数死区检查（tau=600，只涨 1 个单位）\n");
    gw_baseline_init(&b, 600);
    gw_baseline_update(&b, 35, 0);       /* base = 35.00 */
    /* 差值只有 1.00，单帧 step = 100/600 = 0。
     * 没有余数累积的话基线永远不动 —— 这里跑 600 帧必须涨到 36。 */
    for (i = 0; i < 600; i++) {
        gw_baseline_update(&b, 36, 0);
    }
    printf("     600 帧后基线 = %u（应为 36）\n", (unsigned)gw_baseline_value(&b));
    CHECK(gw_baseline_value(&b) == 36, "余数累积生效，没有卡在 35");

    printf("\n[8] 基线：freeze 时不更新\n");
    gw_baseline_init(&b, 10);
    gw_baseline_update(&b, 35, 0);
    for (i = 0; i < 100; i++) {
        gw_baseline_update(&b, 800, 1);  /* 告警中，冻结 */
    }
    printf("     冻结喂 100 帧 800 后基线 = %u\n", (unsigned)gw_baseline_value(&b));
    CHECK(gw_baseline_value(&b) == 35, "基线被冻在 35，没被烟雾拉走");
}

/* 跑一个场景：base 固定 40，gas 序列，看状态怎么走 */
static void scenario(const char *name, const uint16_t *gas, int n,
                     uint8_t want_state, uint32_t want_total,
                     gw_alarm_ev_t want_ev)
{
    gw_baseline_t b;
    gw_alarm_t    a;
    gw_alarm_ev_t ev = ALARM_EV_NONE;
    int           i;

    gw_baseline_init(&b, 600);
    gw_baseline_update(&b, 40, 0);       /* 基线锁在 40 */
    gw_alarm_init(&a);

    for (i = 0; i < n; i++) {
        gw_alarm_ev_t e = gw_alarm_feed(&a, gas[i], &b, 1000 + i);

        if (e != ALARM_EV_NONE) {
            ev = e;
        }
        /* 模拟 main.c 的顺序：先喂告警，再按新状态决定要不要冻结基线 */
        gw_baseline_update(&b, gas[i], a.state == ALARM_ST_ALARM);
    }

    printf("     %-42s state=%-6s total=%u pct=%u\n",
           name, gw_alarm_state_name(a.state),
           (unsigned)a.total, (unsigned)a.pct);
    CHECK(a.state == want_state, "状态正确");
    CHECK(a.total == want_total, "告警次数正确");
    if (want_ev != ALARM_EV_NONE) {
        CHECK(ev == want_ev, "事件正确");
    }
}

static void t_alarm(void)
{
    uint16_t seq[64];
    int      i;

    printf("\n[9] 告警：基线 40，阈值 2.5 倍 = 100\n");

    printf("\n  (a) 全程 45（1.1 倍）：不报\n");
    for (i = 0; i < 20; i++) { seq[i] = 45; }
    scenario("clean 45 x20", seq, 20, ALARM_ST_NORMAL, 0, ALARM_EV_NONE);

    printf("\n  (b) 单帧尖峰 300 后立刻回落：最多到 WARN，不确认\n");
    seq[0] = 300;
    seq[1] = 300;
    for (i = 2; i < 20; i++) { seq[i] = 42; }
    scenario("spike 300 x2 then 42", seq, 20, ALARM_ST_NORMAL, 0, ALARM_EV_NONE);

    printf("\n  (c) 连续 3 帧 300：确认告警\n");
    for (i = 0; i < 3; i++)  { seq[i] = 300; }
    for (i = 3; i < 20; i++) { seq[i] = 300; }
    scenario("300 x20", seq, 20, ALARM_ST_ALARM, 1, ALARM_EV_BEGIN);

    printf("\n  (d) 告警后回落到 42（1.05 倍）10 帧：解除\n");
    for (i = 0; i < 5; i++)  { seq[i] = 300; }
    for (i = 5; i < 25; i++) { seq[i] = 42; }
    scenario("300 x5 then 42 x20", seq, 25, ALARM_ST_NORMAL, 1, ALARM_EV_END);

    printf("\n  (e) 迟滞：告警后回落到 70（1.75 倍，在 1.5~2.5 死区）不解除\n");
    for (i = 0; i < 5; i++)  { seq[i] = 300; }
    for (i = 5; i < 40; i++) { seq[i] = 70; }
    scenario("300 x5 then 70 x35 (dead zone)", seq, 40,
             ALARM_ST_ALARM, 1, ALARM_EV_NONE);

    printf("\n  (f) 迟滞：回落到 55（1.375 倍）够低了，解除\n");
    for (i = 0; i < 5; i++)  { seq[i] = 300; }
    for (i = 5; i < 30; i++) { seq[i] = 55; }
    scenario("300 x5 then 55 x25", seq, 30, ALARM_ST_NORMAL, 1, ALARM_EV_END);

    printf("\n[10] 告警：基线不可信（sensor 读 0）不能误报\n");
    {
        gw_baseline_t b;
        gw_alarm_t    a;

        gw_baseline_init(&b, 600);
        gw_alarm_init(&a);
        /* 基线一直没建立，pct 恒为 0，进来 1000 也不该报 */
        for (i = 0; i < 50; i++) {
            gw_alarm_feed(&a, 1000, &b, 1000 + i);
        }
        CHECK(a.state == ALARM_ST_NORMAL, "基线无效时不报警");
        CHECK(a.pct == 0, "pct 为 0，没有除零");

        /* 基线 = 0（gas 一直是 0，传感器没接），之后突然 500 也不能报 */
        gw_baseline_update(&b, 0, 0);
        for (i = 0; i < 50; i++) {
            gw_alarm_feed(&a, 500, &b, 2000 + i);
        }
        printf("     基线=%u 时喂 500 x50，state=%s\n",
               (unsigned)gw_baseline_value(&b), gw_alarm_state_name(a.state));
        CHECK(a.state == ALARM_ST_NORMAL, "基线 0 时不报警");
    }

    printf("\n[11] 告警：peak_gas 记录期间最高值\n");
    {
        gw_baseline_t b;
        gw_alarm_t    a;

        gw_baseline_init(&b, 600);
        gw_baseline_update(&b, 40, 0);
        gw_alarm_init(&a);

        gw_alarm_feed(&a, 300, &b, 1000);
        gw_alarm_feed(&a, 300, &b, 1001);
        gw_alarm_feed(&a, 300, &b, 1002);        /* 这里 BEGIN，peak=300 */
        gw_alarm_feed(&a, 800, &b, 1003);
        gw_alarm_feed(&a, 650, &b, 1004);
        CHECK(a.state == ALARM_ST_ALARM, "在告警中");
        CHECK(a.peak_gas == 800, "peak_gas 记住了 800");
        printf("     active_sec=%u peak_gas=%u base=%u\n",
               (unsigned)a.active_sec, (unsigned)a.peak_gas, (unsigned)a.base);
        CHECK(a.active_sec == 2, "从 ts=1002 到 1004，持续 2 秒");
    }

    printf("\n[12] 告警：两轮完整告警，total 累加\n");
    {
        gw_baseline_t b;
        gw_alarm_t    a;

        gw_baseline_init(&b, 600);
        gw_baseline_update(&b, 40, 0);
        gw_alarm_init(&a);

        for (i = 0; i < 5; i++)  { gw_alarm_feed(&a, 300, &b, 1000 + i); }
        for (i = 0; i < 15; i++) { gw_alarm_feed(&a, 40,  &b, 1010 + i); }
        CHECK(a.total == 1, "第一轮告警计数 1");
        for (i = 0; i < 5; i++)  { gw_alarm_feed(&a, 300, &b, 1030 + i); }
        for (i = 0; i < 15; i++) { gw_alarm_feed(&a, 40,  &b, 1040 + i); }
        CHECK(a.total == 2, "第二轮后计数 2");
        printf("     total=%u total_sec=%u\n",
               (unsigned)a.total, (unsigned)a.total_sec);
        CHECK(a.total_sec > 0, "累计告警时长 > 0");
    }
}

int main(void)
{
    t_medfilt();
    t_baseline();
    t_alarm();

    printf("\n===== %s（失败 %d 项）=====\n", fails ? "有用例失败" : "全部通过", fails);
    return fails ? 1 : 0;
}
