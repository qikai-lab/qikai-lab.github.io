/*
 * tests/test_parse.c - 解析层 + 落库层的离线单元测试
 * ---------------------------------------------------------------------------
 * 为什么要有这个文件：
 *   网关跑在 Ubuntu 上、数据来自 CC2530 硬件，一旦出问题很难复现
 *   （现场数据不可重复采集）。所以把「解析」和「落库」这两块纯逻辑
 *   抽成不依赖 epoll/串口的模块，用固定的字节向量做回归测试。
 *
 * 测试向量是**真实串口抓到的 208 字节**（凯哥 10-24 提供），
 * 其中 5 帧完整 24 字节、8 帧是复制时丢 0x00 的残帧 —— 正好拿来验证
 * 「串口丢字节能不能自动恢复」。
 *
 * 编译运行（Linux）：
 *     make test && ./tests/test_parse
 * 编译运行（Windows + MinGW，验证纯逻辑用）：
 *     gcc -std=c11 -Wall -Wextra -I. -I<sqlite3 头目录> \
 *         tests/test_parse.c gw_frame.c gw_db.c <sqlite3.dll> -o test_parse.exe
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "gw_frame.h"
#include "gw_db.h"

/* ------------------------------ 测试框架 ------------------------------ */

static int g_pass = 0;
static int g_fail = 0;

static void expect(int cond, const char *what)
{
    if (cond) {
        g_pass++;
        printf("  [ OK ] %s\n", what);
    } else {
        g_fail++;
        printf("  [FAIL] %s\n", what);
    }
}

static void expect_eq_int(int got, int want, const char *what)
{
    if (got == want) {
        g_pass++;
        printf("  [ OK ] %s（%d）\n", what, got);
    } else {
        g_fail++;
        printf("  [FAIL] %s：期望 %d，实际 %d\n", what, want, got);
    }
}

static void expect_eq_u32(uint32_t got, uint32_t want, const char *what)
{
    if (got == want) {
        g_pass++;
        printf("  [ OK ] %s（%u）\n", what, (unsigned)got);
    } else {
        g_fail++;
        printf("  [FAIL] %s：期望 %u，实际 %u\n", what,
               (unsigned)want, (unsigned)got);
    }
}

/* ---------------------------- 帧收集回调 ---------------------------- */

#define MAX_COLLECT 64

typedef struct {
    gw_env_t items[MAX_COLLECT];
    int      n;
} collector_t;

static void collect_cb(const gw_env_t *e, void *user)
{
    collector_t *c = (collector_t *)user;
    if (c->n < MAX_COLLECT) {
        c->items[c->n] = *e;
    }
    c->n++;
}

/* ------------------------------ 测试向量 ------------------------------ */

/* 两帧标准完整帧（按 EnvMsg 结构体手工构造）：
 *   A: 21.1℃ 51.0% node=1 pir=0 lux=858 gas=26 relay=0
 *   B: 21.0℃ 51.0% node=1 pir=1 lux=857 gas=29 relay=1
 */
static const uint8_t k_frame_a[GW_ENV_FRAME_LEN] = {
    0x73, 0x74, 0x3A, 0x65, 0x01, 0x15, 0x00, 0x33,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0x03,
    0x00, 0x00, 0x1A, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t k_frame_b[GW_ENV_FRAME_LEN] = {
    0x73, 0x74, 0x3A, 0x65, 0x00, 0x15, 0x00, 0x33,
    0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x59, 0x03,
    0x00, 0x00, 0x1D, 0x00, 0x01, 0x00, 0x00, 0x00,
};

/* 真实串口抓包：208 字节 / 13 个帧头 / 5 帧完整 / 8 帧残缺 */
static const uint8_t k_real_208[] = {
    0x73, 0x74, 0x3A, 0x65, 0x01, 0x15, 0x33, 0x01, 0x01, 0x59, 0x03, 0x1C,
    0x73, 0x74, 0x3A, 0x65, 0x15, 0x33, 0x01, 0x5A, 0x03, 0x1A, 0x73, 0x74,
    0x3A, 0x65, 0x15, 0x33, 0x01, 0x01, 0x5A, 0x03, 0x19, 0x73, 0x74, 0x3A,
    0x65, 0x01, 0x15, 0x33, 0x01, 0x5A, 0x03, 0x19, 0x73, 0x74, 0x3A, 0x65,
    0x01, 0x15, 0x33, 0x01, 0x5A, 0x03, 0x19, 0x73, 0x74, 0x3A, 0x65, 0x15,
    0x33, 0x01, 0x01, 0x59, 0x03, 0x1D, 0x73, 0x74, 0x3A, 0x65, 0x15, 0x33,
    0x01, 0x5A, 0x03, 0x1B, 0x73, 0x74, 0x3A, 0x65, 0x01, 0x15, 0x33, 0x01,
    0x01, 0x5A, 0x03, 0x1A, 0x73, 0x74, 0x3A, 0x65, 0x01, 0x15, 0x00, 0x33,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0x03, 0x00, 0x00, 0x1A, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x73, 0x74, 0x3A, 0x65, 0x00, 0x15, 0x00, 0x33,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0x03, 0x00, 0x00, 0x19, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x73, 0x74, 0x3A, 0x65, 0x00, 0x15, 0x00, 0x33,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x59, 0x03, 0x00, 0x00, 0x1C, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x73, 0x74, 0x3A, 0x65, 0x00, 0x15, 0x00, 0x33,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0x03, 0x00, 0x00, 0x1A, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x73, 0x74, 0x3A, 0x65, 0x00, 0x15, 0x00, 0x33,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x59, 0x03, 0x00, 0x00, 0x1A, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

/* ------------------------------ 用例 ------------------------------ */

/* 1. 组装 + 解析两帧：字段取值必须逐项对得上 */
static void test_basic_two_frames(void)
{
    gw_parser_t  p;
    collector_t  c;
    uint8_t      buf[GW_ENV_FRAME_LEN * 2];

    printf("\n[用例 1] 两帧完整数据，一次喂完\n");
    memset(&c, 0, sizeof(c));
    gw_parser_init(&p, collect_cb, &c);

    memcpy(buf, k_frame_a, GW_ENV_FRAME_LEN);
    memcpy(buf + GW_ENV_FRAME_LEN, k_frame_b, GW_ENV_FRAME_LEN);
    gw_parser_feed(&p, buf, sizeof(buf));

    expect_eq_int(c.n, 2, "解出帧数");
    if (c.n >= 2) {
        expect_eq_int(gw_env_temp_x10(&c.items[0]), 211, "帧A 温度 x10");
        expect_eq_int(gw_env_humi_x10(&c.items[0]), 510, "帧A 湿度 x10");
        expect_eq_int(c.items[0].lux, 858,             "帧A 光照");
        expect_eq_int(c.items[0].gas, 26,              "帧A 烟雾");
        expect_eq_int(c.items[0].pir, 0,               "帧A 人体=无人");
        expect_eq_int(c.items[0].relay, 0,             "帧A 继电器=OFF");
        expect_eq_int(c.items[0].node, 1,              "帧A 节点号");
        expect_eq_int(gw_env_lux_pct(&c.items[0]), 84, "帧A 光照百分比");

        expect_eq_int(gw_env_temp_x10(&c.items[1]), 210, "帧B 温度 x10");
        expect_eq_int(c.items[1].pir, 1,                 "帧B 人体=有人");
        expect_eq_int(c.items[1].relay, 1,               "帧B 继电器=ON");
        expect_eq_int(c.items[1].gas, 29,                "帧B 烟雾");
    }
    expect_eq_u32(p.stat_bad, 0, "坏帧数");
    expect_eq_u32(p.stat_short, 0, "残帧数");
}

/* 2. 逐字节喂：验证半帧状态机（串口 read 返回 1 字节的极端情况） */
static void test_byte_by_byte(void)
{
    gw_parser_t p;
    collector_t c;
    uint8_t     buf[GW_ENV_FRAME_LEN * 2];
    size_t      i;

    printf("\n[用例 2] 逐字节喂入（每次 1 字节）\n");
    memset(&c, 0, sizeof(c));
    gw_parser_init(&p, collect_cb, &c);

    memcpy(buf, k_frame_a, GW_ENV_FRAME_LEN);
    memcpy(buf + GW_ENV_FRAME_LEN, k_frame_b, GW_ENV_FRAME_LEN);
    for (i = 0; i < sizeof(buf); i++) {
        gw_parser_feed(&p, &buf[i], 1);
    }

    expect_eq_int(c.n, 2, "解出帧数");
    expect_eq_int(gw_env_temp_x10(&c.items[0]), 211, "第 1 帧温度");
    expect_eq_int(gw_env_temp_x10(&c.items[1]), 210, "第 2 帧温度");
}

/* 3. 前面有垃圾字节：验证帧头重同步 */
static void test_noise_prefix(void)
{
    gw_parser_t p;
    collector_t c;
    uint8_t     buf[8 + GW_ENV_FRAME_LEN];

    printf("\n[用例 3] 帧前有 8 字节垃圾（串口上电毛刺）\n");
    memset(&c, 0, sizeof(c));
    gw_parser_init(&p, collect_cb, &c);

    memset(buf, 0xAA, 8);
    memcpy(buf + 8, k_frame_a, GW_ENV_FRAME_LEN);
    gw_parser_feed(&p, buf, sizeof(buf));

    expect_eq_int(c.n, 1, "解出帧数");
    expect_eq_u32(p.stat_resync, 8, "丢弃的垃圾字节数");
    expect_eq_int(gw_env_temp_x10(&c.items[0]), 211, "温度仍然正确");
}

/* 4. 帧内丢 1 字节：残帧必须被丢掉，后面的真帧必须能救回来 */
static void test_dropped_byte(void)
{
    gw_parser_t p;
    collector_t c;
    uint8_t     buf[GW_ENV_FRAME_LEN * 2];
    size_t      n = 0;
    int         i;

    printf("\n[用例 4] 第一帧中间丢 1 字节（串口丢字节）\n");
    memset(&c, 0, sizeof(c));
    gw_parser_init(&p, collect_cb, &c);

    for (i = 0; i < (int)GW_ENV_FRAME_LEN; i++) {
        if (i == 10) {
            continue;   /* 故意漏掉第 10 个字节 */
        }
        buf[n++] = k_frame_a[i];
    }
    memcpy(buf + n, k_frame_b, GW_ENV_FRAME_LEN);
    n += GW_ENV_FRAME_LEN;

    gw_parser_feed(&p, buf, n);

    expect_eq_int(c.n, 1, "只解出 1 帧（坏的那帧被丢）");
    /*
     * 这里到底记在哪个计数器上，取决于丢字节后残帧的长度：
     *   - 残帧 23 字节 + 下一帧头首字节 → 凑满 24 字节的"假帧"，
     *     走到字段校验那一步才被拒 → 记 stat_bad
     *   - 残帧只有 10~12 字节 → 半帧里就冒出下一个帧头 → 记 stat_short
     * 两种都是"丢一帧、后续恢复"，都正确，所以这里断言合计。
     * （用例 6 的真实抓包走的是 stat_short 分支）
     */
    expect_eq_int((int)(p.stat_bad + p.stat_short), 1, "坏帧+残帧合计计数");
    expect_eq_int(gw_env_temp_x10(&c.items[0]), 210, "救回来的是第二帧（21.0℃）");
}

/* 4b. 残帧只有 12 字节（真实抓包里最常见的样子）：走 short 分支 */
static void test_truncated_frame(void)
{
    gw_parser_t p;
    collector_t c;
    uint8_t     buf[GW_ENV_FRAME_LEN * 2];
    size_t      n = 0;
    int         i;

    printf("\n[用例 4b] 第一帧被截断成 12 字节（半帧里就冒出下一个帧头）\n");
    memset(&c, 0, sizeof(c));
    gw_parser_init(&p, collect_cb, &c);

    for (i = 0; i < 12; i++) {
        buf[n++] = k_frame_a[i];
    }
    memcpy(buf + n, k_frame_b, GW_ENV_FRAME_LEN);
    n += GW_ENV_FRAME_LEN;

    gw_parser_feed(&p, buf, n);

    expect_eq_int(c.n, 1, "只解出 1 帧");
    expect_eq_u32(p.stat_short, 1, "残帧计数");
    expect_eq_u32(p.stat_bad, 0, "没有误判成坏帧");
    expect_eq_int(gw_env_temp_x10(&c.items[0]), 210, "留下的是完整的第二帧");
}

/* 5. 假帧头：帧头对但字段非法，必须被拒绝，不能污染数据 */
static void test_fake_header(void)
{
    gw_parser_t p;
    collector_t c;
    uint8_t     buf[GW_ENV_FRAME_LEN * 2];

    printf("\n[用例 5] 假帧头（字段违法：温度小数位=153）\n");
    memset(&c, 0, sizeof(c));
    gw_parser_init(&p, collect_cb, &c);

    memcpy(buf, k_frame_a, GW_ENV_FRAME_LEN);
    buf[4] = 0x99;   /* temp_dec = 153，不可能的值 */
    memcpy(buf + GW_ENV_FRAME_LEN, k_frame_b, GW_ENV_FRAME_LEN);

    gw_parser_feed(&p, buf, sizeof(buf));

    expect_eq_int(c.n, 1, "只接受后面那帧合法数据");
    expect_eq_u32(p.stat_bad, 1, "坏帧计数");
    expect_eq_int(gw_env_temp_x10(&c.items[0]), 210, "留下的值是第二帧");
}

/* 6. 真实 208 字节抓包：5 帧完整 + 8 帧残缺 */
static void test_real_capture(void)
{
    gw_parser_t p;
    collector_t c;
    int         k;

    printf("\n[用例 6] 真实串口抓包 208 字节（5 完整 + 8 残缺）\n");
    memset(&c, 0, sizeof(c));
    gw_parser_init(&p, collect_cb, &c);

    gw_parser_feed(&p, k_real_208, sizeof(k_real_208));

    expect_eq_u32(p.stat_ok, 5, "成功解析帧数");
    expect_eq_u32(p.stat_short, 8, "残缺帧数");
    expect_eq_u32(p.stat_bad, 0, "坏帧数");
    expect_eq_u32(p.stat_resync, 88, "残缺字节被丢弃（12+10+11+11+11+11+10+12）");

    if (c.n == 5) {
        static const int want_temp[5] = { 211, 210, 210, 210, 210 };
        static const int want_lux[5]  = { 858, 858, 857, 858, 857 };
        static const int want_gas[5]  = { 26,  25,  28,  26,  26  };
        int all_temp = 1, all_hum = 1, all_lux = 1, all_gas = 1, all_node = 1;

        for (k = 0; k < 5; k++) {
            printf("         帧%d: %d.%dC %d.%d%% lux=%u gas=%u\n", k + 1,
                   c.items[k].temp_int, c.items[k].temp_dec,
                   c.items[k].hum_int,  c.items[k].hum_dec,
                   (unsigned)c.items[k].lux, (unsigned)c.items[k].gas);
            if (gw_env_temp_x10(&c.items[k]) != want_temp[k]) all_temp = 0;
            if (gw_env_humi_x10(&c.items[k]) != 510)          all_hum  = 0;
            if ((int)c.items[k].lux != want_lux[k])           all_lux  = 0;
            if ((int)c.items[k].gas != want_gas[k])           all_gas  = 0;
            if (c.items[k].node != 1)                         all_node = 0;
        }
        expect(all_temp, "5 帧温度全部对得上（211/210/210/210/210）");
        expect(all_hum,  "5 帧湿度全部 = 51.0%");
        expect(all_lux,  "5 帧光照全部对得上（858/858/857/858/857）");
        expect(all_gas,  "5 帧烟雾全部对得上（26/25/28/26/26）");
        expect(all_node, "5 帧节点号全部 = 1");
    }
}

/* 7. 完整帧被拆成任意切片喂：验证粘包处理 */
static void test_random_split(void)
{
    gw_parser_t p;
    collector_t c;
    uint8_t     buf[GW_ENV_FRAME_LEN * 3];
    size_t      off = 0;
    static const size_t cuts[] = { 3, 7, 1, 13, 5, 2, 9, 4, 8, 6, 11 };

    printf("\n[用例 7] 三帧被切成 11 段随机喂入\n");
    memset(&c, 0, sizeof(c));
    gw_parser_init(&p, collect_cb, &c);

    memcpy(buf,                     k_frame_a, GW_ENV_FRAME_LEN);
    memcpy(buf + GW_ENV_FRAME_LEN,  k_frame_b, GW_ENV_FRAME_LEN);
    memcpy(buf + GW_ENV_FRAME_LEN * 2, k_frame_a, GW_ENV_FRAME_LEN);

    for (size_t i = 0; i < sizeof(cuts) / sizeof(cuts[0]) && off < sizeof(buf); i++) {
        size_t take = cuts[i];
        if (off + take > sizeof(buf)) {
            take = sizeof(buf) - off;
        }
        gw_parser_feed(&p, buf + off, take);
        off += take;
    }
    if (off < sizeof(buf)) {
        gw_parser_feed(&p, buf + off, sizeof(buf) - off);
    }

    expect_eq_int(c.n, 3, "解出帧数");
}

/* 8. SQLite 落库：插 3 条，读回条数与数值 */
static void test_sqlite(void)
{
    gw_parser_t  p;
    collector_t  c;
    gw_db_t     *db;
    const char  *path = "test_gw.db";
    int64_t      rows;
    uint8_t      buf[GW_ENV_FRAME_LEN * 3];

    printf("\n[用例 8] SQLite 落库（WAL + 批量提交）\n");
    remove(path);
    remove("test_gw.db-wal");
    remove("test_gw.db-shm");

    memset(&c, 0, sizeof(c));
    gw_parser_init(&p, collect_cb, &c);
    memcpy(buf,                        k_frame_a, GW_ENV_FRAME_LEN);
    memcpy(buf + GW_ENV_FRAME_LEN,     k_frame_b, GW_ENV_FRAME_LEN);
    memcpy(buf + GW_ENV_FRAME_LEN * 2, k_frame_a, GW_ENV_FRAME_LEN);
    gw_parser_feed(&p, buf, sizeof(buf));

    db = gw_db_open(path);
    expect(db != NULL, "打开数据库");
    if (db == NULL) {
        return;
    }
    gw_db_set_batch(db, 20);   /* 3 条不到一批，靠 close 时提交 */

    for (int i = 0; i < c.n; i++) {
        char what[32];
        snprintf(what, sizeof(what), "第 %d 条入库", i + 1);
        expect_eq_int(gw_db_insert(db, &c.items[i], 1700000000 + i), 0, what);
    }

    rows = gw_db_total_rows(db);
    expect_eq_int((int)rows, 3, "库内总条数");
    expect_eq_int((int)gw_db_rows_of_node(db, 1), 3, "节点 1 的条数");
    expect_eq_int((int)gw_db_last_ts(db), 1700000002, "最后一条时间戳");

    gw_db_close(db);

    /* 重新打开，确认数据真的落到磁盘了（不是只在内存） */
    db = gw_db_open(path);
    expect(db != NULL, "重新打开数据库");
    if (db != NULL) {
        expect_eq_int((int)gw_db_total_rows(db), 3, "重开后条数仍是 3");
        gw_db_close(db);
    }

    remove(path);
    remove("test_gw.db-wal");
    remove("test_gw.db-shm");
}

/* ------------------------------ 入口 ------------------------------ */

int main(void)
{
    printf("========================================\n");
    printf(" 网关解析层 / 落库层 单元测试\n");
    printf(" 协议: st: 固定 %u 字节\n", (unsigned)GW_ENV_FRAME_LEN);
    printf("========================================\n");

    test_basic_two_frames();
    test_byte_by_byte();
    test_noise_prefix();
    test_dropped_byte();
    test_truncated_frame();
    test_fake_header();
    test_real_capture();
    test_random_split();
    test_sqlite();

    printf("\n========================================\n");
    printf(" 通过 %d 项，失败 %d 项 —— %s\n",
           g_pass, g_fail, (g_fail == 0) ? "全部通过" : "有用例失败");
    printf("========================================\n");
    return (g_fail == 0) ? 0 : 1;
}
