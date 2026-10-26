/*
 * test_chain.c - 端到端串联测试（M4）
 * ---------------------------------------------------------------------------
 * 前面几个测试各测一块，这个测试测**接线**：
 *   原始帧 → on_env → 中值滤波 → 告警状态机 → 基线冻结 → 快照 → 落库
 *
 * 做法是把 main.c 整个 include 进来（把 main 改名避开冲突），
 * 这样它的 static 函数 on_env 在这里可见，能直接喂假帧跑完整链路。
 * 白盒测试的常用手法，和 test_http.c 一样 —— 生产代码不会这么写。
 * ---------------------------------------------------------------------------
 */
#define main gw_main_unused
#include "main.c"
#undef main

static int fails = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  [OK]   %s\n", msg); } \
    else { printf("  [FAIL] %s\n", msg); fails++; } \
} while (0)

static void mk(gw_env_t *e, uint16_t gas)
{
    memset(e, 0, sizeof(*e));
    e->node     = 1;
    e->temp_int = 24;
    e->temp_dec = 5;
    e->hum_int  = 58;
    e->hum_dec  = 0;
    e->lux      = 500;
    e->gas      = gas;
    e->pir      = 0;
    e->relay    = 0;
}

int main(void)
{
    gw_ctx_t     ctx;
    gw_latest_t *L;
    gw_env_t     e;
    int          i;
    int          frames = 0;
    gw_alarm_row_t ar[4];
    int          n;

    remove("tests/_chain.db");
    remove("tests/_chain.db-wal");
    remove("tests/_chain.db-shm");

    /* ---- 完全按 main() 里的初始化顺序来 ---- */
    memset(&ctx, 0, sizeof(ctx));
    gw_latest_init();
    L = gw_latest();

    gw_medfilt_init(&ctx.med);
    gw_baseline_init(&ctx.base, GW_BASELINE_TAU);
    gw_alarm_init(&ctx.alarm);
    ctx.alarm_id = 0;
    ctx.quiet    = 1;                   /* 别把测试输出刷满屏 */

    ctx.db = gw_db_open("tests/_chain.db");
    if (ctx.db == NULL) {
        printf("打不开测试库\n");
        return 1;
    }
    gw_db_set_batch(ctx.db, 1000);      /* 别中途提交，最后一次性收 */

    printf("[1] 100 帧干净空气 gas=35\n");
    for (i = 0; i < 100; i++) {
        mk(&e, 35);
        on_env(&e, &ctx);
        frames++;
    }
    printf("     基线=%u 中值=%u 倍数=%u%% 状态=%s\n",
           (unsigned)L->gas_base, (unsigned)L->gas_med,
           (unsigned)L->gas_pct, gw_alarm_state_name(L->alarm_state));
    CHECK(L->gas_med == 35, "中值 = 35");
    CHECK(L->gas_base == 35, "基线 = 35");
    CHECK(L->gas_pct == 100, "倍数 = 100%");
    CHECK(L->alarm_state == ALARM_ST_NORMAL, "状态正常");
    CHECK(L->alarm_total == 0, "告警 0 次");

    printf("\n[2] 插一帧尖峰 900：中值滤波应该吃掉它\n");
    mk(&e, 900);
    on_env(&e, &ctx);
    frames++;
    printf("     原始=900 中值=%u 状态=%s\n",
           (unsigned)L->gas_med, gw_alarm_state_name(L->alarm_state));
    CHECK(L->gas_med == 35, "中值仍是 35（尖峰被挤掉）");
    CHECK(L->gas == 900, "原始值仍是 900（留证据）");
    CHECK(L->alarm_state == ALARM_ST_NORMAL, "没进 WARN");

    printf("\n[3] 连续 5 帧 120（3.4 倍）：应该确认告警\n");
    for (i = 0; i < 5; i++) {
        mk(&e, 120);
        on_env(&e, &ctx);
        frames++;
    }
    printf("     中值=%u 倍数=%u%% 状态=%s 累计=%u\n",
           (unsigned)L->gas_med, (unsigned)L->gas_pct,
           gw_alarm_state_name(L->alarm_state), (unsigned)L->alarm_total);
    CHECK(L->alarm_state == ALARM_ST_ALARM, "进入告警");
    CHECK(L->alarm_total == 1, "累计 1 次");
    CHECK(ctx.alarm_id > 0, "库里有对应记录，alarm_id 有效");

    printf("\n[4] 告警期间基线必须冻结\n");
    for (i = 0; i < 20; i++) {
        mk(&e, 120);
        on_env(&e, &ctx);
        frames++;
    }
    printf("     基线=%u（应仍为 35）峰值=%u\n",
           (unsigned)L->gas_base, (unsigned)L->alarm_peak);
    CHECK(L->gas_base == 35, "基线没被烟雾拉高");
    CHECK(L->alarm_peak >= 120, "峰值记录正确");
    CHECK(L->alarm_state == ALARM_ST_ALARM, "仍在告警");

    printf("\n[5] 回落 20 帧 gas=35：连续 10 帧后解除\n");
    for (i = 0; i < 20; i++) {
        mk(&e, 35);
        on_env(&e, &ctx);
        frames++;
    }
    printf("     状态=%s 累计=%u 上次持续=%u 秒\n",
           gw_alarm_state_name(L->alarm_state), (unsigned)L->alarm_total,
           (unsigned)ctx.alarm.last_sec);
    CHECK(L->alarm_state == ALARM_ST_NORMAL, "已解除");
    CHECK(ctx.alarm_id == 0, "alarm_id 已清空");

    printf("\n[6] 库里核对\n");
    gw_db_commit(ctx.db);
    printf("     env_data 行数=%lld  告警条数=%lld\n",
           (long long)gw_db_count_rows(ctx.db),
           (long long)gw_db_alarm_count(ctx.db));
    CHECK(gw_db_count_rows(ctx.db) == (int64_t)frames, "每帧都落库了");
    CHECK(gw_db_alarm_count(ctx.db) == 1, "库里有 1 条已结束的告警");

    n = gw_db_alarm_recent(ctx.db, 4, ar, 4);
    CHECK(n == 1, "查得到 1 条");
    if (n == 1) {
        printf("     id=%d start=%lld end=%lld peak=%d base=%d trig=%d\n",
               ar[0].id, (long long)ar[0].start_ts, (long long)ar[0].end_ts,
               ar[0].peak_gas, ar[0].base_gas, ar[0].trig_gas);
        CHECK(ar[0].end_ts > 0, "end_ts 已补上（不是进行中）");
        CHECK(ar[0].base_gas == 35, "记录了触发时的基线 35");
        CHECK(ar[0].trig_gas >= 120, "记录了触发时的 gas");
        CHECK(ar[0].peak_gas >= 120, "记录了峰值");
    }

    gw_db_close(ctx.db);
    remove("tests/_chain.db");

    printf("\n===== %s（失败 %d 项）=====\n", fails ? "有用例失败" : "全部通过", fails);
    return fails ? 1 : 0;
}
