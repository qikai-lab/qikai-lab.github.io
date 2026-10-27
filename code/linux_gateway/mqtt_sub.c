/*
 * mqtt_sub.c - MQTT 订阅 → SQLite 落库（第 4 步 · 阶段 B 的服务端）
 * ===========================================================================
 * 它和现有的 `gw`（串口网关）是**两条独立的数据链路**：
 *
 *   gw        : /dev/ttyUSB0 → 串口 → env_data        （阶段 3，已在跑）
 *   mqtt_sub  : mosquitto    → MQTT → env_data_mqtt   （阶段 4，本文件）
 *
 * 为什么要分成两张表而不是直接写同一张：
 *   1) 两条链路的**数据完整性不一样**。串口链路是「本地直连、几乎不丢」，
 *      MQTT 链路有 QoS1 重传（可能重复）、有断线丢包（可能缺失）。
 *      混在一张表里就再也分不清「这条是准的还是有疑问的」。
 *   2) 调试期两条链路同时跑，正好能**交叉验证**：同一时刻两边的
 *      温湿度、lux、gas 应该几乎一样。等你确认 MQTT 链路可信了，
 *      再决定要不要合并。
 *   3) 出问题能立刻定位是哪一段坏了 —— 这张表空但那张表有数，
 *      就是 ESP32/WiFi/MQTT 的问题；两张表都空，才是节点或协调器的问题。
 *
 * 三个实现上的取舍：
 *
 *   a) **手写 JSON 解析，不引 cJSON**。
 *      payload 是我们自己发的固定格式（见 mqtt_pub.c），字段名写死，
 *      用 strstr + atoi 十几行就够了。引 cJSON 要多装一个依赖、
 *      多一层 malloc，对固定格式没有任何好处。
 *
 *   b) **回调里只入缓冲，不做 SQLite IO**。
 *      mosquitto 的回调跑在网络线程里，在里面写库会拖慢收包、甚至丢消息。
 *      回调只负责解析 + 塞进环形缓冲，主循环攒够 20 条再一次性提交。
 *      这和 gw_db.c 的批量提交是同一个思路。
 *
 *   c) **不做去重，但把重复统计出来**。
 *      QoS1 是「至少一次」，网络抖动时 broker 可能重发同一条。
 *      严格去重要设备端带上递增序号（seq），那是下一步的事。
 *      现在先按 (node, ts) 统计 1 秒内到达超过 1 条的情况，
 *      至少让人知道「有没有在重复」。
 * ===========================================================================
 */

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <mosquitto.h>
#include <sqlite3.h>

/* ------------------------------ 配置 ------------------------------ */
#define DEF_BROKER      "127.0.0.1"
#define DEF_PORT        1883
#define DEF_TOPIC       "zigbee/+/env"      /* + 通配一个层级：zigbee/node1/env */
#define DEF_DB          "env_mqtt.db"
#define DEF_BATCH       20                  /* 攒够 20 条提交一次 */

#define BUF_CAP         256                 /* 待写缓冲条数（约 4 分钟） */
#define STAT_PERIOD_S   5                   /* 统计打印周期 */

/* ------------------------------ 待写缓冲 ------------------------------ */
typedef struct {
    long long ts;
    int node, temp_x10, hum_x10, lux, gas, pir, relay;
} rec_t;

static rec_t s_buf[BUF_CAP];
static int   s_cnt = 0;

/* 统计 */
static unsigned long s_rx      = 0;   /* 收到的消息条数 */
static unsigned long s_bad     = 0;   /* 解析失败的条数 */
static unsigned long s_written = 0;   /* 成功入库的条数 */
static unsigned long s_dup     = 0;   /* 同一 node 同一秒内出现第 2 条及以后 */

static volatile sig_atomic_t s_quit = 0;

/* 用于判重：记录上一个 (node, ts) */
static int       s_last_node = -1;
static long long s_last_ts   = 0;

static sqlite3      *s_db  = NULL;
static sqlite3_stmt *s_ins = NULL;

static const char *const k_schema =
    "CREATE TABLE IF NOT EXISTS env_data_mqtt ("
    "  id       INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts       INTEGER NOT NULL,"                 /* 服务端收到时刻（Unix 秒） */
    "  node     INTEGER NOT NULL,"
    "  temp_x10 INTEGER NOT NULL,"                 /* 19.4℃ → 194，和 env_data 同口径 */
    "  hum_x10  INTEGER NOT NULL,"
    "  lux      INTEGER NOT NULL,"
    "  gas      INTEGER NOT NULL,"
    "  pir      INTEGER NOT NULL,"
    "  relay    INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_mqtt_ts      ON env_data_mqtt(ts);"
    "CREATE INDEX IF NOT EXISTS idx_mqtt_node_ts ON env_data_mqtt(node, ts);";

/* ------------------------------ 工具函数 ------------------------------ */

/**
 * 从固定格式 JSON 里取一个整数字段。
 * 输入形如 {"node":1,"temp":194,...}，找 "key": 后面的数字。
 * 找不到返回 def —— 调用方据此判断字段缺失。
 */
static int json_get_int(const char *json, const char *key, int def)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\":", key);

    const char *p = strstr(json, pat);
    if (!p) {
        return def;
    }
    p += strlen(pat);
    while (*p == ' ') {
        p++;
    }
    /* 只接受数字和负号开头，避免把后面的垃圾当成 0 */
    if (*p != '-' && (*p < '0' || *p > '9')) {
        return def;
    }
    return atoi(p);
}

static void on_sigint(int sig)
{
    (void)sig;
    s_quit = 1;
}

/* ------------------------------ 数据库 ------------------------------ */
static int db_open(const char *path)
{
    if (sqlite3_open(path, &s_db) != SQLITE_OK) {
        fprintf(stderr, "[DB] 打开失败: %s\n",
                s_db ? sqlite3_errmsg(s_db) : "unknown");
        return -1;
    }

    /* 和 gw_db.c 一致：WAL 让读写不互斥，NORMAL 换写入速度 */
    char *err = NULL;
    sqlite3_exec(s_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(s_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

    if (sqlite3_exec(s_db, k_schema, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "[DB] 建表失败: %s\n", err ? err : "unknown");
        sqlite3_free(err);
        return -1;
    }

    const char *sql =
        "INSERT INTO env_data_mqtt"
        "(ts,node,temp_x10,hum_x10,lux,gas,pir,relay) VALUES(?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(s_db, sql, -1, &s_ins, NULL) != SQLITE_OK) {
        fprintf(stderr, "[DB] 预处理失败: %s\n", sqlite3_errmsg(s_db));
        return -1;
    }

    sqlite3_exec(s_db, "BEGIN;", NULL, NULL, NULL);
    return 0;
}

/** 把缓冲里的记录一次性写进库里 */
static void db_flush(void)
{
    if (s_cnt == 0 || !s_db || !s_ins) {
        return;
    }

    for (int i = 0; i < s_cnt; i++) {
        const rec_t *r = &s_buf[i];
        sqlite3_bind_int64(s_ins, 1, r->ts);
        sqlite3_bind_int  (s_ins, 2, r->node);
        sqlite3_bind_int  (s_ins, 3, r->temp_x10);
        sqlite3_bind_int  (s_ins, 4, r->hum_x10);
        sqlite3_bind_int  (s_ins, 5, r->lux);
        sqlite3_bind_int  (s_ins, 6, r->gas);
        sqlite3_bind_int  (s_ins, 7, r->pir);
        sqlite3_bind_int  (s_ins, 8, r->relay);

        if (sqlite3_step(s_ins) == SQLITE_DONE) {
            s_written++;
        } else {
            fprintf(stderr, "[DB] 插入失败: %s\n", sqlite3_errmsg(s_db));
        }
        sqlite3_reset(s_ins);
        sqlite3_clear_bindings(s_ins);
    }

    /* 提交并重开事务：一次提交摊薄 WAL fsync 的开销 */
    sqlite3_exec(s_db, "COMMIT;", NULL, NULL, NULL);
    sqlite3_exec(s_db, "BEGIN;", NULL, NULL, NULL);

    s_cnt = 0;
}

/* ------------------------------ MQTT 回调 ------------------------------ */
/*
 * topic 形如 zigbee/node1/env，payload 是 mqtt_pub.c 发的 JSON。
 * 注意：这个函数跑在 mosquitto 的网络线程里，**绝对不能做耗时操作**。
 */
static void on_message(struct mosquitto *mosq, void *userdata,
                       const struct mosquitto_message *msg)
{
    (void)mosq;
    (void)userdata;

    if (!msg->payload || msg->payloadlen <= 0) {
        s_bad++;
        return;
    }

    s_rx++;

    /* payload 不保证以 \0 结尾，复制一份再解析 */
    char json[256];
    int  n = msg->payloadlen < (int)sizeof(json) - 1
           ? msg->payloadlen : (int)sizeof(json) - 1;
    memcpy(json, msg->payload, (size_t)n);
    json[n] = '\0';

    int node  = json_get_int(json, "node",  -1);
    int temp  = json_get_int(json, "temp",  INT32_MIN);
    int hum   = json_get_int(json, "hum",   INT32_MIN);
    int lux   = json_get_int(json, "lux",   -1);
    int gas   = json_get_int(json, "gas",   -1);
    int pir   = json_get_int(json, "pir",   -1);
    int relay = json_get_int(json, "relay", -1);

    /* 字段缺失或越界就整条丢弃 —— 宁可不写，也不要把半条脏数据写进库。
     * 这和 ESP32 侧 gw_frame 的「字段校验不过就丢帧」是同一个原则。 */
    if (node <= 0 || temp == INT32_MIN || hum == INT32_MIN ||
        lux < 0 || lux > 1023 || gas < 0 || gas > 1023 ||
        pir < 0 || pir > 1 || relay < 0 || relay > 1) {
        s_bad++;
        if (s_bad <= 3) {   /* 只在前几条打印，免得刷屏 */
            fprintf(stderr, "[PARSE] 字段非法，丢弃: %s\n", json);
        }
        return;
    }

    long long ts = (long long)time(NULL);

    /* QoS1 可能重复：同一 node 在同一秒里出现多条，记为疑似重复 */
    if (node == s_last_node && ts == s_last_ts) {
        s_dup++;
    }
    s_last_node = node;
    s_last_ts   = ts;

    if (s_cnt >= BUF_CAP) {
        /* 缓冲满了：立刻刷一次，绝不能丢数据 */
        db_flush();
    }

    rec_t *r = &s_buf[s_cnt++];
    r->ts       = ts;
    r->node     = node;
    r->temp_x10 = temp;
    r->hum_x10  = hum;
    r->lux      = lux;
    r->gas      = gas;
    r->pir      = pir;
    r->relay    = relay;
}

static void on_connect(struct mosquitto *mosq, void *userdata, int rc)
{
    (void)userdata;
    if (rc == 0) {
        printf("[MQTT] 已连接 broker，订阅 %s (QoS1)\n", DEF_TOPIC);
        mosquitto_subscribe(mosq, NULL, DEF_TOPIC, 1);
    } else {
        fprintf(stderr, "[MQTT] 连接失败, rc=%d (%s)\n",
                rc, mosquitto_strerror(rc));
    }
}

/* ------------------------------ 主流程 ------------------------------ */
int main(int argc, char **argv)
{
    const char *broker = (argc > 1) ? argv[1] : DEF_BROKER;
    int         port   = (argc > 2) ? atoi(argv[2]) : DEF_PORT;
    const char *dbpath = (argc > 3) ? argv[3] : DEF_DB;

    signal(SIGINT,  on_sigint);
    signal(SIGTERM, on_sigint);

    if (db_open(dbpath) != 0) {
        return 1;
    }
    printf("[DB] 已打开 %s（表 env_data_mqtt）\n", dbpath);

    mosquitto_lib_init();

    struct mosquitto *mosq = mosquitto_new(NULL, true, NULL);
    if (!mosq) {
        fprintf(stderr, "[MQTT] 创建客户端失败\n");
        return 1;
    }

    mosquitto_message_callback_set(mosq, on_message);
    mosquitto_connect_callback_set(mosq, on_connect);

    /* 自动重连：断开后 2 秒重试，最多 10 次 */
    mosquitto_reconnect_delay_set(mosq, 2, 10, true);

    printf("[MQTT] 连接 %s:%d ...\n", broker, port);
    int rc = mosquitto_connect(mosq, broker, port, 30);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[MQTT] 首次连接失败: %s\n", mosquitto_strerror(rc));
        fprintf(stderr, "       检查 mosquitto 是否在跑: ss -lntp | grep 1883\n");
        /* 不死心，继续进循环让自动重连去试 */
    }

    time_t next_stat = time(NULL) + STAT_PERIOD_S;

    while (!s_quit) {
        /* 100ms 一轮，既及时收消息，又不空转 */
        mosquitto_loop(mosq, 100, 1);

        time_t now = time(NULL);

        if (s_cnt >= DEF_BATCH) {
            db_flush();
        }

        if (now >= next_stat) {
            next_stat = now + STAT_PERIOD_S;
            db_flush();     /* 统计前先落盘，保证下面的数字是准的 */
            printf("---- %ds: 收到=%lu 入库=%lu 解析失败=%lu 疑似重复=%lu ----\n",
                   STAT_PERIOD_S, s_rx, s_written, s_bad, s_dup);
            fflush(stdout);
        }
    }

    printf("\n[MQTT] 收到退出信号，正在收尾...\n");
    db_flush();
    sqlite3_exec(s_db, "COMMIT;", NULL, NULL, NULL);
    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    sqlite3_finalize(s_ins);
    sqlite3_close(s_db);

    printf("[MQTT] 结束。累计 收到=%lu 入库=%lu 解析失败=%lu 疑似重复=%lu\n",
           s_rx, s_written, s_bad, s_dup);
    return 0;
}
