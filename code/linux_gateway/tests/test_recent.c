/* 临时测试：验证 gw_db_recent 的 SQL / 排序 / 过滤 / 上限
 * 只在本机 MinGW + sqlite3.dll 上跑，验证完就删。 */
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

static void mk_env(gw_env_t *e, uint8_t node, uint8_t ti, uint8_t td,
                   uint8_t hi, uint8_t hd, uint16_t lux, uint16_t gas,
                   uint8_t pir, uint8_t relay)
{
    memset(e, 0, sizeof(*e));
    e->node = node;
    e->temp_int = ti; e->temp_dec = td;
    e->hum_int = hi;  e->hum_dec = hd;
    e->lux = lux;     e->gas = gas;
    e->pir = pir;     e->relay = relay;
}

int main(void)
{
    gw_db_t  *db;
    gw_env_t  e;
    gw_row_t  rows[64];
    int       n;
    int       i;

    remove("tests/_tmp.db");
    remove("tests/_tmp.db-wal");
    remove("tests/_tmp.db-shm");

    db = gw_db_open("tests/_tmp.db");
    if (db == NULL) {
        printf("打不开测试库\n");
        return 1;
    }
    gw_db_set_batch(db, 5);

    /* 插 10 条：ts = 1000..1009，温度 100..109（x10 定点），节点交替 1/2 */
    for (i = 0; i < 10; i++) {
        mk_env(&e, (uint8_t)((i % 2) ? 2 : 1), (uint8_t)i, 0,
               50, 0, (uint16_t)(500 + i), (uint16_t)(30 + i),
               (uint8_t)(i % 2), 0);
        if (gw_db_insert(db, &e, 1000 + i) != 0) {
            printf("插入第 %d 条失败: %s\n", i, gw_db_last_error(db));
            return 1;
        }
    }
    gw_db_commit(db);

    printf("总行数: %lld\n", (long long)gw_db_count_rows(db));

    printf("\n[1] limit=5，应该拿到 ts=1005..1009，且时间正序\n");
    n = gw_db_recent(db, 5, 0, rows, 64);
    CHECK(n == 5, "返回 5 行");
    CHECK(rows[0].ts == 1005, "第一行 ts=1005（最老的在前）");
    CHECK(rows[4].ts == 1009, "最后一行 ts=1009（最新的在后）");
    CHECK(rows[0].temp_x10 == 50, "第一行温度=50（i=5 -> 5.0C，×10 定点）");
    CHECK(rows[0].hum_x10 == 500, "第一行湿度=500（50.0%）");
    for (i = 0; i < n; i++) {
        printf("     ts=%lld node=%d temp=%d hum=%d lux=%d gas=%d pir=%d\n",
               (long long)rows[i].ts, rows[i].node, rows[i].temp_x10,
               rows[i].hum_x10, rows[i].lux, rows[i].gas, rows[i].pir);
    }

    printf("\n[2] limit=0（用默认 60），10 条全拿到\n");
    n = gw_db_recent(db, 0, 0, rows, 64);
    CHECK(n == 10, "返回 10 行");
    CHECK(rows[0].ts == 1000, "第一行 ts=1000");
    CHECK(rows[9].ts == 1009, "最后一行 ts=1009");

    printf("\n[3] node=2 过滤，只有 5 条（i=1,3,5,7,9）\n");
    n = gw_db_recent(db, 60, 2, rows, 64);
    CHECK(n == 5, "返回 5 行");
    {
        int all2 = 1;
        for (i = 0; i < n; i++) {
            if (rows[i].node != 2) { all2 = 0; }
        }
        CHECK(all2, "全部 node=2");
    }

    printf("\n[4] limit=100 但数组只能放 3 个 -> 最多 3 行\n");
    n = gw_db_recent(db, 100, 0, rows, 3);
    CHECK(n == 3, "被 max 参数卡到 3 行");
    CHECK(rows[2].ts == 1009, "拿的是最新 3 条（1007..1009 的末条）");

    printf("\n[5] 空库查询\n");
    {
        gw_db_t *db2 = gw_db_open("tests/_tmp2.db");
        n = gw_db_recent(db2, 10, 0, rows, 64);
        CHECK(n == 0, "空库返回 0 行而不是 -1");
        gw_db_close(db2);
        remove("tests/_tmp2.db");
    }

    printf("\n[6] 批量提交不能被 recent 打断\n");
    {
        int64_t before = gw_db_count_rows(db);
        (void)gw_db_recent(db, 60, 0, rows, 64);
        CHECK(gw_db_count_rows(db) == before, "查 3 次后行数不变");
    }

    printf("\n[7] 告警表：begin / end / recent（M4）\n");
    {
        gw_alarm_row_t ar[8];
        int            id1;
        int            id2;
        int            m;

        id1 = gw_db_alarm_begin(db, 1, 2000, 40, 105);
        id2 = gw_db_alarm_begin(db, 1, 2100, 42, 110);
        printf("     id1=%d id2=%d\n", id1, id2);
        CHECK(id1 > 0 && id2 > 0, "插入返回有效 id");
        CHECK(id2 == id1 + 1, "id 递增 1");

        CHECK(gw_db_alarm_end(db, id1, 2030, 300) == 0, "结束第一条");

        m = gw_db_alarm_recent(db, 10, ar, 8);
        CHECK(m == 2, "取到 2 条");
        CHECK(ar[0].id == id2, "第 0 条是最新的（倒序，和 history 相反）");
        CHECK(ar[0].end_ts == 0, "最新那条 end_ts=0（进行中）");
        CHECK(ar[1].end_ts == 2030, "旧那条 end_ts=2030");
        CHECK(ar[1].peak_gas == 300, "peak_gas 被 end 更新成 300");
        CHECK(ar[1].base_gas == 40 && ar[1].trig_gas == 105,
              "基线和触发值都存对了");

        CHECK(gw_db_alarm_count(db) == 1, "已结束的告警只有 1 条");

        m = gw_db_alarm_recent(db, 10, ar, 1);
        CHECK(m == 1, "max=1 时被数组卡到 1 条");

        /* 收尾，避免遗留"进行中"影响下一次跑 */
        CHECK(gw_db_alarm_end(db, id2, 2130, 400) == 0, "结束第二条");
        gw_db_commit(db);
        CHECK(gw_db_alarm_count(db) == 2, "提交后两条都算已结束");
    }

    gw_db_close(db);
    remove("tests/_tmp.db");
    remove("tests/_tmp.db-wal");
    remove("tests/_tmp.db-shm");

    printf("\n[8] 上电清理遗留的「进行中」告警（模拟 kill -9 后重启）\n");
    {
        gw_alarm_row_t ar[4];
        gw_db_t       *db3;
        int            ida;
        int            m;

        remove("tests/_tmp3.db");

        db3 = gw_db_open("tests/_tmp3.db");
        ida = gw_db_alarm_begin(db3, 1, 5000, 40, 120);
        CHECK(ida > 0, "造一条进行中的告警");
        gw_db_commit(db3);
        gw_db_close(db3);        /* 关键：故意不调 alarm_end */

        db3 = gw_db_open("tests/_tmp3.db");
        CHECK(gw_db_alarm_count(db3) == 1, "重新打开后它被收尾（count=1）");
        m = gw_db_alarm_recent(db3, 4, ar, 4);
        CHECK(m == 1, "记录还在，没被删");
        CHECK(ar[0].end_ts == ar[0].start_ts, "end_ts 补成 start_ts，时长记 0");
        gw_db_close(db3);

        remove("tests/_tmp3.db");
    }

    printf("\n===== %s（失败 %d 项）=====\n", fails ? "有用例失败" : "全部通过", fails);
    return fails ? 1 : 0;
}
