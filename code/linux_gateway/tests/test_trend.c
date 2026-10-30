/* gw_db_trend 的单元测试（1h / 24h 趋势曲线用的时间桶聚合）
 *
 * 为什么这个测试值得写这么细
 * --------------------------
 *   桶聚合错了不会崩，只会画出**看着像数据、其实在骗人的曲线**：
 *     · AVG 直接强转 → 整条线系统偏低 0.05 度，人看不出来但趋势是假的
 *     · 断档补 0     → 掉线那 5 分钟画成一条"温度 0 度"，会被当成设备故障
 *     · 平均代替峰值 → 烧张纸的 3 秒尖峰在 15 分钟桶里被抹平，告警曲线全平
 *   所以每一条都单独断言，而且尽量用能被整除的时间戳，让期望值能手算。
 *
 * 时间戳特意选 999900：同时能被 30 和 900 整除，
 * 桶起点就等于窗口起点，断言里不用再算偏移。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gw_db.h"
#include "gw_frame.h"

static int fails = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  [OK]   %s\n", msg); } \
    else { printf("  [FAIL] %s\n", msg); fails++; } \
} while (0)

#define T0      999900LL        /* 30 和 900 都能整除 */
#define BUCKET1 30              /* 1h  → 120 桶 */
#define BUCKET24 900            /* 24h → 96 桶 */

static void mk_env(gw_env_t *e, uint8_t node, uint8_t ti, uint8_t td,
                   uint16_t lux, uint16_t gas, uint8_t pir)
{
    memset(e, 0, sizeof(*e));
    e->node     = node;
    e->temp_int = ti; e->temp_dec = td;
    e->hum_int  = 50; e->hum_dec  = 0;
    e->lux      = lux;
    e->gas      = gas;
    e->pir      = pir;
    e->relay    = 0;
}

/* 每个场景一个干净的新库，避免上一组的残留影响断言 */
static gw_db_t *fresh(const char *path)
{
    char buf[128];

    remove(path);
    snprintf(buf, sizeof(buf), "%s-wal", path); remove(buf);
    snprintf(buf, sizeof(buf), "%s-shm", path); remove(buf);

    return gw_db_open(path);
}

int main(void)
{
    gw_db_t   *db;
    gw_env_t   e;
    gw_trend_t rows[160];
    int        n;
    int        i;

    printf("=== gw_db_trend 分桶聚合 ===\n");

    /*------------------------------------------------------------------
     * [1] 干净的 1 小时：每秒 1 帧，共 3600 帧 → 30 秒桶 → 120 个桶
     *     温度在 20.0 / 20.1 之间交替（×10 = 200 / 201），每桶 30 帧
     *     一半一半，平均值恰好是 200.5 —— 用来看"四舍五入还是截断"：
     *     正确结果 201，如果 (int) 强转就会变成 200，整条线偏低。
     *     gas 在 i=1234 那一帧跳到 500，用来验证"峰值不被平均抹平"。
     *----------------------------------------------------------------*/
    printf("\n[1] 1 小时 / 每 30 秒一桶 → 120 桶\n");
    db = fresh("tests/_trend1.db");
    if (db == NULL) {
        printf("打不开测试库\n");
        return 1;
    }
    gw_db_set_batch(db, 0);          /* 每条立即提交，下面 count 才准 */

    for (i = 0; i < 3600; i++) {
        mk_env(&e, 1, 20, (uint8_t)(i % 2),
              (uint16_t)(i % 100),
              (uint16_t)((i == 1234) ? 500 : 30),
              (uint8_t)(i % 2));
        if (gw_db_insert(db, &e, T0 + i) != 0) {
            printf("插入第 %d 条失败: %s\n", i, gw_db_last_error(db));
            return 1;
        }
    }

    n = gw_db_trend(db, T0, T0 + 3599, BUCKET1, 0, rows, 128);
    CHECK(n == 120, "3600 帧 / 30 秒桶 = 120 个桶");
    CHECK(n > 0 && rows[0].ts == T0, "第一个桶起点 = 窗口起点 999900");
    CHECK(n > 0 && rows[119].ts == T0 + 3570, "最后一个桶起点 = 999900+3570");
    CHECK(n > 0 && rows[0].n == 30, "第一个桶里有 30 帧");
    CHECK(n > 0 && rows[0].temp_x10 == 201, "AVG 200.5 四舍五入成 201（不是截断成 200）");
    CHECK(n > 0 && rows[0].hum_x10 == 500, "湿度恒定 50.0% → 500");
    CHECK(n > 0 && rows[0].tmin_x10 == 200, "桶内最低温 200（20.0 度）");
    CHECK(n > 0 && rows[0].tmax_x10 == 201, "桶内最高温 201（20.1 度）");
    CHECK(n > 0 && rows[0].pir == 15, "30 帧里半数 pir=1 → 15");

    /* gas 尖峰落在哪个桶：ts = T0+1234 = 1001134，桶起点 1001130，
     * 相对窗口偏移 1230，第 1230/30 = 41 个桶 */
    CHECK(n > 41 && rows[41].gas_max == 500, "尖峰桶 MAX(gas)=500 被保住");
    CHECK(n > 41 && rows[41].gas == 46, "同桶 AVG(gas)=(29*30+500)/30=45.7→46（被平均拉低，所以要看 MAX）");

    /* 时间正序 + 单调 */
    {
        int mono = 1;
        for (i = 1; i < n; i++) {
            if (rows[i].ts <= rows[i - 1].ts) {
                mono = 0;
            }
        }
        CHECK(mono, "桶严格按时间正序、无重复");
    }

    printf("     前 3 桶: ");
    for (i = 0; i < 3 && i < n; i++) {
        printf("[ts=%lld n=%d t=%.1f] ", (long long)rows[i].ts, rows[i].n,
               rows[i].temp_x10 / 10.0);
    }
    printf("\n");

    /*------------------------------------------------------------------
     * [2] 时间窗裁剪：只问中间 10 分钟，不能把窗口外的行算进来
     *----------------------------------------------------------------*/
    printf("\n[2] 只取窗口中间 600 秒 → 20 个桶\n");
    n = gw_db_trend(db, T0 + 600, T0 + 1199, BUCKET1, 0, rows, 128);
    CHECK(n == 20, "600 秒 / 30 秒 = 20 个桶");
    CHECK(n > 0 && rows[0].ts == T0 + 600, "第一个桶从 T0+600 开始");
    CHECK(n > 0 && rows[0].n == 30, "边界桶完整（左闭）");
    CHECK(n > 0 && rows[19].ts == T0 + 1170, "最后桶 = T0+1170");

    /*------------------------------------------------------------------
     * [3] max 上限：数组只有 5 个槽时不能越界写
     *----------------------------------------------------------------*/
    printf("\n[3] 传 max=5，只允许写 5 个桶\n");
    n = gw_db_trend(db, T0, T0 + 3599, BUCKET1, 0, rows, 5);
    CHECK(n == 5, "被 max 截住，返回 5");

    gw_db_close(db);

    /*------------------------------------------------------------------
     * [4] 断档不补 0 —— CH340 掉线就长这样
     *     造 10 分钟数据，中间挖掉**整 10 个桶**（120~419 秒），
     *     期望 20 个桶里只剩 10 个，而且绝不能出现 n=0 的桶
     *     （补 0 会画出"温度 0 度"，比留缺口更误导）
     *----------------------------------------------------------------*/
    printf("\n[4] 中间 300 秒无数据 → 桶变少、且不补 0\n");
    db = fresh("tests/_trend2.db");
    if (db == NULL) {
        return 1;
    }
    gw_db_set_batch(db, 0);
    for (i = 0; i < 600; i++) {
        if (i >= 120 && i < 420) {
            continue;               /* 第 120~419 秒：串口掉了，一帧都没有 */
        }
        mk_env(&e, 1, 20, 0, 0, 30, 0);
        if (gw_db_insert(db, &e, T0 + i) != 0) {
            return 1;
        }
    }
    n = gw_db_trend(db, T0, T0 + 599, BUCKET1, 0, rows, 128);
    CHECK(n == 10, "600 秒里只有 300 秒有数据 → 10 个桶（不是 20）");

    {
        int zero = 0;
        int maxgap = 0;
        for (i = 0; i < n; i++) {
            if (rows[i].n == 0) {
                zero++;
            }
            if (i > 0) {
                int gap = (int)(rows[i].ts - rows[i - 1].ts);
                if (gap > maxgap) {
                    maxgap = gap;
                }
            }
        }
        CHECK(zero == 0, "没有 n=0 的空桶（补 0 会画出假的 0 度）");
        /* 最后一个有数据的桶起点 90，下一个有数据的桶起点 420，
         * 起点差 330 秒（= 300 秒断档 + 前后桶内各自剩下的余量） */
        CHECK(maxgap == 330, "最大桶间距 330 秒 —— 前端据此断线，缺口看得见");
        printf("     最大间距 %d 秒（断档 300 秒，前端按 > %.0f 秒断开）\n",
               maxgap, BUCKET1 * 2.5);
    }
    gw_db_close(db);

    /*------------------------------------------------------------------
     * [5] 节点过滤：两个节点混在一个库里，node=2 只能看到 2 号的数据
     *----------------------------------------------------------------*/
    printf("\n[5] node 过滤\n");
    db = fresh("tests/_trend3.db");
    if (db == NULL) {
        return 1;
    }
    gw_db_set_batch(db, 0);
    for (i = 0; i < 60; i++) {
        mk_env(&e, 1, 20, 0, 0, 30, 0);          /* 1 号：温度 20.0 */
        gw_db_insert(db, &e, T0 + i);
        mk_env(&e, 2, 30, 0, 0, 90, 0);          /* 2 号：温度 30.0 */
        gw_db_insert(db, &e, T0 + i);
    }
    n = gw_db_trend(db, T0, T0 + 59, BUCKET1, 2, rows, 128);
    CHECK(n == 2, "60 秒 → 2 个桶");
    CHECK(n > 0 && rows[0].n == 30, "每桶 30 帧（只算 2 号，不是 60）");
    CHECK(n > 0 && rows[0].temp_x10 == 300, "看到的是 2 号的 30.0 度，不是 1 号的 20.0");
    CHECK(n > 0 && rows[0].gas == 90, "gas 也是 2 号的");

    n = gw_db_trend(db, T0, T0 + 59, BUCKET1, 0, rows, 128);
    CHECK(n > 0 && rows[0].n == 60, "node=0 → 两个节点一起算，每桶 60 帧");

    n = gw_db_trend(db, T0, T0 + 59, BUCKET1, 9, rows, 128);
    CHECK(n == 0, "node=9 不存在 → 0 个桶（空数组，不是报错）");
    gw_db_close(db);

    /*------------------------------------------------------------------
     * [6] 24 小时 / 15 分钟桶 → 96 个桶
     *     数据很稀（每 30 秒 1 帧，共 2880 帧），验证点数只跟窗口有关
     *----------------------------------------------------------------*/
    printf("\n[6] 24 小时 / 每 900 秒一桶 → 96 桶\n");
    db = fresh("tests/_trend4.db");
    if (db == NULL) {
        return 1;
    }
    gw_db_set_batch(db, 0);
    for (i = 0; i < 2880; i++) {
        mk_env(&e, 1, 20, 0, 0, 30, 0);
        if (gw_db_insert(db, &e, T0 + (int64_t)i * 30) != 0) {
            return 1;
        }
    }
    n = gw_db_trend(db, T0, T0 + 86399, BUCKET24, 0, rows, 128);
    CHECK(n == 96, "86400 秒 / 900 秒 = 96 个桶（不是 2880 个原始点）");
    CHECK(n > 0 && rows[0].n == 30, "每桶 30 帧（每 30 秒一帧 × 900 秒）");
    gw_db_close(db);

    /*------------------------------------------------------------------
     * [7] 边界：空库、bucket<=0、from>to
     *----------------------------------------------------------------*/
    printf("\n[7] 边界情况\n");
    db = fresh("tests/_trend5.db");
    if (db == NULL) {
        return 1;
    }
    n = gw_db_trend(db, T0, T0 + 3599, BUCKET1, 0, rows, 128);
    CHECK(n == 0, "空库 → 0 个桶（不崩）");

    mk_env(&e, 1, 20, 0, 0, 30, 0);
    gw_db_insert(db, &e, T0 + 5);

    n = gw_db_trend(db, T0, T0 + 59, 0, 0, rows, 128);
    CHECK(n == 1, "bucket=0 → 按 60 秒兜底，1 个桶");

    n = gw_db_trend(db, T0 + 100, T0 + 50, BUCKET1, 0, rows, 128);
    CHECK(n == 0, "from > to → 0 个桶（不崩）");

    n = gw_db_trend(db, T0, T0 + 59, BUCKET1, 0, rows, 0);
    CHECK(n == -1, "max=0 → -1（参数错，不是静默返回 0）");
    gw_db_close(db);

    printf("\n=== %s（失败 %d 项）===\n", fails ? "有失败" : "全部通过", fails);
    return fails ? 1 : 0;
}
