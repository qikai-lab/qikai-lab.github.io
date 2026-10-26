/*
 * gw_db.c - SQLite 落库实现
 */
#include "gw_db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#define GW_DB_ERRBUF   200

struct gw_db {
    sqlite3     *h;
    sqlite3_stmt *ins;

    int          batch;            /* 每多少条提交一次 */
    int          pending;          /* 当前事务里攒了多少条 */
    int          in_tx;

    uint32_t     err_count;
    char         errmsg[GW_DB_ERRBUF];
};

static const char *const k_schema =
    "CREATE TABLE IF NOT EXISTS env_data ("
    "  id       INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts       INTEGER NOT NULL,"                 /* Unix 秒 */
    "  node     INTEGER NOT NULL,"                 /* 节点号 */
    "  temp_x10 INTEGER NOT NULL,"                 /* 温度 x10，21.1℃ → 211 */
    "  hum_x10  INTEGER NOT NULL,"                 /* 湿度 x10 */
    "  lux      INTEGER NOT NULL,"                 /* 光照 ADC 0~1023 */
    "  gas      INTEGER NOT NULL,"                 /* 烟雾 ADC 0~1023 */
    "  pir      INTEGER NOT NULL,"                 /* 0/1 */
    "  relay    INTEGER NOT NULL,"                 /* 0/1 */
    "  raw      BLOB"                             /* 原始 24 字节，排障用 */
    ");"
    /* M3 的两个查询：最近 N 条（按 id 倒序）、某时间段曲线（按 ts 范围） */
    "CREATE INDEX IF NOT EXISTS idx_env_ts      ON env_data(ts);"
    "CREATE INDEX IF NOT EXISTS idx_env_node_ts ON env_data(node, ts);"
    /* M4：告警事件表。一次告警一行，end_ts 为 NULL = 还在告警中 */
    "CREATE TABLE IF NOT EXISTS alarm ("
    "  id       INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  node     INTEGER NOT NULL,"
    "  start_ts INTEGER NOT NULL,"
    "  end_ts   INTEGER,"                          /* NULL = 进行中 */
    "  base_gas INTEGER,"                          /* 触发时的基线 */
    "  trig_gas INTEGER,"                          /* 触发那一帧的 gas */
    "  peak_gas INTEGER"                           /* 期间峰值 */
    ");"
    "CREATE INDEX IF NOT EXISTS idx_alarm_start ON alarm(start_ts);";

static void set_err(gw_db_t *db, const char *what)
{
    db->err_count++;
    snprintf(db->errmsg, sizeof(db->errmsg), "%s: %s", what,
             db->h ? sqlite3_errmsg(db->h) : "no handle");
}

static int exec_sql(gw_db_t *db, const char *sql)
{
    char *emsg = NULL;

    if (sqlite3_exec(db->h, sql, NULL, NULL, &emsg) != SQLITE_OK) {
        snprintf(db->errmsg, sizeof(db->errmsg), "exec: %s",
                 emsg ? emsg : "unknown");
        sqlite3_free(emsg);
        db->err_count++;
        return -1;
    }
    return 0;
}

gw_db_t *gw_db_open(const char *path)
{
    gw_db_t *db;
    const char *sql =
        "INSERT INTO env_data(ts,node,temp_x10,hum_x10,lux,gas,pir,relay,raw)"
        " VALUES(?,?,?,?,?,?,?,?,?)";

    if (path == NULL) {
        return NULL;
    }

    db = (gw_db_t *)calloc(1, sizeof(*db));
    if (db == NULL) {
        return NULL;
    }
    db->batch = GW_DB_DEFAULT_BATCH;

    if (sqlite3_open_v2(path, &db->h,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        NULL) != SQLITE_OK) {
        /* open 失败时 sqlite3_errmsg 仍然可读 */
        fprintf(stderr, "[db] 打开 %s 失败: %s\n", path,
                db->h ? sqlite3_errmsg(db->h) : "unknown");
        if (db->h) {
            sqlite3_close(db->h);
        }
        free(db);
        return NULL;
    }

    /* 忙等 3 秒：M3 的 HTTP 查询线程在写时不会立刻报 SQLITE_BUSY */
    sqlite3_busy_timeout(db->h, 3000);

    if (exec_sql(db, "PRAGMA journal_mode=WAL;")   != 0 ||
        exec_sql(db, "PRAGMA synchronous=NORMAL;") != 0 ||
        exec_sql(db, "PRAGMA foreign_keys=ON;")    != 0 ||
        exec_sql(db, k_schema)                     != 0) {
        fprintf(stderr, "[db] 初始化失败: %s\n", db->errmsg);
        sqlite3_close(db->h);
        free(db);
        return NULL;
    }

    /* 上电清理：上个进程若被 kill -9，会留下 end_ts IS NULL 的「进行中」记录。
     * 那个状态机已经不在了，这些记录永远不会被收尾，网页上会一直挂着一条
     * 假的告警。把 end_ts 补成 start_ts（时长记 0），至少让它变成历史记录。 */
    if (exec_sql(db, "UPDATE alarm SET end_ts = start_ts WHERE end_ts IS NULL;") == 0) {
        int n = sqlite3_changes(db->h);

        if (n > 0) {
            printf("[db] 清理 %d 条未收尾的告警记录（上次进程没正常退出）\n", n);
        }
    }

    if (sqlite3_prepare_v2(db->h, sql, -1, &db->ins, NULL) != SQLITE_OK) {
        set_err(db, "prepare insert");
        fprintf(stderr, "[db] 预处理插入语句失败: %s\n", db->errmsg);
        sqlite3_close(db->h);
        free(db);
        return NULL;
    }

    /* 开一个长事务，后面按 batch 提交 */
    if (exec_sql(db, "BEGIN;") == 0) {
        db->in_tx = 1;
    }
    return db;
}

void gw_db_set_batch(gw_db_t *db, int batch)
{
    if (db != NULL) {
        db->batch = (batch > 0) ? batch : 1;
    }
}

static int db_commit_locked(gw_db_t *db, const char *sql)
{
    if (exec_sql(db, sql) != 0) {
        db->in_tx = 0;
        return -1;
    }
    db->pending = 0;
    return 0;
}

int gw_db_commit(gw_db_t *db)
{
    if (db == NULL) {
        return -1;
    }
    if (!db->in_tx) {
        return 0;
    }
    if (db_commit_locked(db, "COMMIT;") != 0) {
        return -1;
    }
    /* 立刻开下一个事务，保持「批量提交」的节奏 */
    if (exec_sql(db, "BEGIN;") == 0) {
        db->in_tx = 1;
    }
    return 0;
}

int gw_db_insert(gw_db_t *db, const gw_env_t *e, int64_t ts)
{
    int rc;

    if (db == NULL || e == NULL || db->ins == NULL) {
        return -1;
    }

    sqlite3_bind_int64(db->ins, 1, ts);
    sqlite3_bind_int  (db->ins, 2, (int)e->node);
    sqlite3_bind_int  (db->ins, 3, gw_env_temp_x10(e));
    sqlite3_bind_int  (db->ins, 4, gw_env_humi_x10(e));
    sqlite3_bind_int  (db->ins, 5, (int)e->lux);
    sqlite3_bind_int  (db->ins, 6, (int)e->gas);
    sqlite3_bind_int  (db->ins, 7, (int)e->pir);
    sqlite3_bind_int  (db->ins, 8, (int)e->relay);
    /* TRANSIENT：让 SQLite 自己拷一份，调用方栈上的 e 随时可以被回收 */
    sqlite3_bind_blob (db->ins, 9, e->raw, (int)GW_ENV_FRAME_LEN,
                       SQLITE_TRANSIENT);

    rc = sqlite3_step(db->ins);
    if (rc != SQLITE_DONE) {
        set_err(db, "insert");
        sqlite3_reset(db->ins);
        sqlite3_clear_bindings(db->ins);
        return -1;
    }
    sqlite3_reset(db->ins);
    sqlite3_clear_bindings(db->ins);

    db->pending++;
    if (db->in_tx && db->pending >= db->batch) {
        return gw_db_commit(db);
    }
    return 0;
}

static int64_t query_int64(gw_db_t *db, const char *sql, int bind_node)
{
    sqlite3_stmt *st = NULL;
    int64_t       v  = -1;

    if (db == NULL || db->h == NULL) {
        return -1;
    }
    if (sqlite3_prepare_v2(db->h, sql, -1, &st, NULL) != SQLITE_OK) {
        set_err(db, "prepare query");
        return -1;
    }
    if (bind_node > 0) {
        sqlite3_bind_int(st, 1, bind_node);
    }
    if (sqlite3_step(st) == SQLITE_ROW) {
        v = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return v;
}

int64_t gw_db_total_rows(gw_db_t *db)
{
    /* 先提交，否则当前事务里的行数看不全（WAL 下本连接可见，但保险起见） */
    if (db != NULL && db->in_tx && db->pending > 0) {
        gw_db_commit(db);
    }
    return query_int64(db, "SELECT COUNT(*) FROM env_data;", 0);
}

int64_t gw_db_rows_of_node(gw_db_t *db, int node)
{
    if (db != NULL && db->in_tx && db->pending > 0) {
        gw_db_commit(db);
    }
    return query_int64(db, "SELECT COUNT(*) FROM env_data WHERE node=?1;", node);
}

int64_t gw_db_last_ts(gw_db_t *db)
{
    if (db != NULL && db->in_tx && db->pending > 0) {
        gw_db_commit(db);
    }
    return query_int64(db, "SELECT MAX(ts) FROM env_data;", 0);
}

/*------------------------------------------------------------------------------
 * M3：给 HTTP 用的两个查询
 *
 * 和上面几个函数的关键区别：**这里绝对不 commit**。
 *
 *   gw_db_total_rows() 开头有一句「pending > 0 就先 commit」，
 *   如果 HTTP 每 2 秒查一次行数，就等于每 2 秒提交一次事务，
 *   批量提交（20 条一次）彻底失效，WAL 的 fsync 从 20 秒一次变成 2 秒一次。
 *
 *   而同一连接读**自己事务里未提交的行**是可见的（SQLite 保证），
 *   所以不 commit 也照样能查到刚刚写入的数据。
 *----------------------------------------------------------------------------*/

int64_t gw_db_count_rows(gw_db_t *db)
{
    return query_int64(db, "SELECT COUNT(*) FROM env_data;", 0);
}

int gw_db_recent(gw_db_t *db, int limit, int node, gw_row_t *out, int max)
{
    static const char *SQL =
        "SELECT ts,node,temp_x10,hum_x10,lux,gas,pir,relay FROM env_data "
        "WHERE (?1 = 0 OR node = ?1) ORDER BY id DESC LIMIT ?2;";

    sqlite3_stmt *st = NULL;
    int           n  = 0;

    if (db == NULL || db->h == NULL || out == NULL || max <= 0) {
        return -1;
    }
    if (limit <= 0) {
        limit = 60;
    }
    if (limit > 200) {
        limit = 200;
    }
    if (limit > max) {
        limit = max;
    }

    if (sqlite3_prepare_v2(db->h, SQL, -1, &st, NULL) != SQLITE_OK) {
        set_err(db, "prepare recent");
        return -1;
    }
    sqlite3_bind_int(st, 1, node);
    sqlite3_bind_int(st, 2, limit);

    while ((sqlite3_step(st) == SQLITE_ROW) && (n < max)) {
        out[n].ts       = sqlite3_column_int64(st, 0);
        out[n].node     = sqlite3_column_int(st, 1);
        out[n].temp_x10 = sqlite3_column_int(st, 2);
        out[n].hum_x10  = sqlite3_column_int(st, 3);
        out[n].lux      = sqlite3_column_int(st, 4);
        out[n].gas      = sqlite3_column_int(st, 5);
        out[n].pir      = sqlite3_column_int(st, 6);
        out[n].relay    = sqlite3_column_int(st, 7);
        n++;
    }
    sqlite3_finalize(st);

    /* SQL 是 DESC（最新在前），倒过来变成时间正序，前端可以直接画曲线 */
    {
        int i;
        int j = n - 1;

        for (i = 0; i < j; i++, j--) {
            gw_row_t t = out[i];
            out[i]     = out[j];
            out[j]     = t;
        }
    }
    return n;
}

/*------------------------------------------------------------------------------
 * M4：告警事件
 *
 * 全部不复用预处理语句 —— 告警是稀有事件（一天几次），
 * prepare/finalize 的开销完全无所谓，换来的是这段代码永远不会因为
 * 「上一个函数 reset 忘了」而出错。
 *----------------------------------------------------------------------------*/

int gw_db_alarm_begin(gw_db_t *db, int node, int64_t ts,
                      int base_gas, int trig_gas)
{
    static const char *SQL =
        "INSERT INTO alarm(node,start_ts,end_ts,base_gas,trig_gas,peak_gas)"
        " VALUES(?1,?2,NULL,?3,?4,?4);";     /* peak 先等于 trig，后面再抬 */

    sqlite3_stmt *st = NULL;
    int           id = 0;

    if (db == NULL || db->h == NULL) {
        return 0;
    }
    if (sqlite3_prepare_v2(db->h, SQL, -1, &st, NULL) != SQLITE_OK) {
        set_err(db, "prepare alarm_begin");
        return 0;
    }
    sqlite3_bind_int  (st, 1, node);
    sqlite3_bind_int64(st, 2, ts);
    sqlite3_bind_int  (st, 3, base_gas);
    sqlite3_bind_int  (st, 4, trig_gas);

    if (sqlite3_step(st) == SQLITE_DONE) {
        id = (int)sqlite3_last_insert_rowid(db->h);
    } else {
        set_err(db, "alarm_begin");
    }
    sqlite3_finalize(st);
    return id;
}

int gw_db_alarm_end(gw_db_t *db, int id, int64_t ts, int peak_gas)
{
    static const char *SQL =
        "UPDATE alarm SET end_ts=?1, peak_gas=?2 WHERE id=?3;";

    sqlite3_stmt *st = NULL;
    int           rc = -1;

    if (db == NULL || db->h == NULL || id <= 0) {
        return -1;
    }
    if (sqlite3_prepare_v2(db->h, SQL, -1, &st, NULL) != SQLITE_OK) {
        set_err(db, "prepare alarm_end");
        return -1;
    }
    sqlite3_bind_int64(st, 1, ts);
    sqlite3_bind_int  (st, 2, peak_gas);
    sqlite3_bind_int  (st, 3, id);

    if (sqlite3_step(st) == SQLITE_DONE) {
        rc = 0;
    } else {
        set_err(db, "alarm_end");
    }
    sqlite3_finalize(st);
    return rc;
}

int gw_db_alarm_recent(gw_db_t *db, int limit, gw_alarm_row_t *out, int max)
{
    static const char *SQL =
        "SELECT id,node,start_ts,end_ts,base_gas,trig_gas,peak_gas FROM alarm"
        " ORDER BY id DESC LIMIT ?1;";

    sqlite3_stmt *st = NULL;
    int           n  = 0;

    if (db == NULL || db->h == NULL || out == NULL || max <= 0) {
        return -1;
    }
    if (limit <= 0) {
        limit = 20;
    }
    if (limit > 100) {
        limit = 100;
    }
    if (limit > max) {
        limit = max;
    }

    if (sqlite3_prepare_v2(db->h, SQL, -1, &st, NULL) != SQLITE_OK) {
        set_err(db, "prepare alarm_recent");
        return -1;
    }
    sqlite3_bind_int(st, 1, limit);

    while ((sqlite3_step(st) == SQLITE_ROW) && (n < max)) {
        out[n].id       = sqlite3_column_int(st, 0);
        out[n].node     = sqlite3_column_int(st, 1);
        out[n].start_ts = sqlite3_column_int64(st, 2);
        /* end_ts 可能是 NULL（进行中），column_int64 对 NULL 返回 0，正合用 */
        out[n].end_ts   = (sqlite3_column_type(st, 3) == SQLITE_NULL)
                          ? 0 : sqlite3_column_int64(st, 3);
        out[n].base_gas = sqlite3_column_int(st, 4);
        out[n].trig_gas = sqlite3_column_int(st, 5);
        out[n].peak_gas = sqlite3_column_int(st, 6);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

int64_t gw_db_alarm_count(gw_db_t *db)
{
    return query_int64(db, "SELECT COUNT(*) FROM alarm WHERE end_ts IS NOT NULL;", 0);
}

const char *gw_db_last_error(const gw_db_t *db)
{
    return (db != NULL) ? db->errmsg : "db is NULL";
}
uint32_t gw_db_err_count(const gw_db_t *db)
{
    return (db != NULL) ? db->err_count : 0;
}

void gw_db_close(gw_db_t *db)
{
    if (db == NULL) {
        return;
    }

    if (db->in_tx) {
        /* 收尾必须提交，否则最后不到 batch 条的数据全丢 */
        if (exec_sql(db, "COMMIT;") != 0) {
            exec_sql(db, "ROLLBACK;");
        }
        db->in_tx = 0;
    }

    if (db->ins != NULL) {
        sqlite3_finalize(db->ins);
    }
    if (db->h != NULL) {
        sqlite3_close(db->h);
    }
    free(db);
}
