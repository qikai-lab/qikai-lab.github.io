/*
 * 临时测试：直接把 http_srv.c 的实现拉进来（为了访问 static 的 build_response），
 * 在 PC 上验证 HTTP 响应拼装 —— 不需要 socket、不需要板子。
 *
 * 重点验证 Content-Length 和实际 body 长度是否一致。
 * 这两个数一旦对不上，浏览器就会一直转圈（等不到剩下的字节）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gw_latest.h"
#include "gw_db.h"
#include "gw_frame.h"

/* 把实现 include 进来，static 函数就可见了。
 * 生产代码当然不这么写，这里纯粹为了白盒测试。 */
#include "http_srv.c"

static int fails = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  [OK]   %s\n", msg); } \
    else { printf("  [FAIL] %s\n", msg); fails++; } \
} while (0)

/* 从响应里抠出 Content-Length，并检查它和 body 实际长度是否一致 */
static void check_len(const char *resp, size_t total, const char *tag)
{
    const char *h = strstr(resp, "Content-Length: ");
    const char *body = strstr(resp, "\r\n\r\n");
    long declared;
    size_t actual;

    if (h == NULL || body == NULL) {
        printf("  [FAIL] %s 找不到 Content-Length 或 body 分隔符\n", tag);
        fails++;
        return;
    }
    declared = atol(h + 16);
    actual   = total - (size_t)(body + 4 - resp);

    if ((size_t)declared == actual) {
        printf("  [OK]   %s Content-Length=%ld 与实际 body %lu 一致\n",
               tag, declared, (unsigned long)actual);
    } else {
        printf("  [FAIL] %s Content-Length=%ld 但实际 body %lu\n",
               tag, declared, (unsigned long)actual);
        fails++;
    }
}

static void dump_body(const char *resp, int max)
{
    const char *b = strstr(resp, "\r\n\r\n");
    if (b == NULL) {
        return;
    }
    b += 4;
    printf("     body: %.*s%s\n", max, b, strlen(b) > (size_t)max ? "..." : "");
}

int main(void)
{
    http_srv_t  srv;
    char        out[HTTP_RESP_MAX];
    size_t      n;
    gw_env_t    e;

    remove("tests/test_http.db");
    remove("tests/test_http.db-wal");
    remove("tests/test_http.db-shm");

    memset(&srv, 0, sizeof(srv));
    srv.db = gw_db_open("tests/test_http.db");

    gw_latest_init();

    printf("[1] 还没收到任何帧时 /api/latest\n");
    n = build_response(&srv, "/api/latest", out, sizeof(out));
    check_len(out, n, "/api/latest(empty)");
    dump_body(out, 120);
    CHECK(strstr(out, "\"valid\":0") != NULL, "返回 valid:0 而不是乱码");

    printf("\n[2] 塞一帧进去（19.5C / 57.0%% / lux584 / gas36）\n");
    {
        memset(&e, 0, sizeof(e));
        e.node = 1;
        e.temp_int = 19; e.temp_dec = 5;
        e.hum_int = 57;  e.hum_dec = 0;
        e.lux = 584;     e.gas = 36;
        e.pir = 0;       e.relay = 0;
        gw_latest_on_frame(&e, (int64_t)time(NULL));
    }
    n = build_response(&srv, "/api/latest", out, sizeof(out));
    check_len(out, n, "/api/latest");
    dump_body(out, 200);
    CHECK(strstr(out, "\"temp\":19.5") != NULL, "温度格式化成 19.5");
    CHECK(strstr(out, "\"hum\":57.0")  != NULL, "湿度格式化成 57.0");
    CHECK(strstr(out, "\"lux\":584")   != NULL, "光照原值 584");
    CHECK(strstr(out, "\"gas\":36")    != NULL, "烟雾原值 36");
    CHECK(strstr(out, "text/html")     == NULL, "Content-Type 是 JSON");

    printf("\n[3] 负温度（-0.5C）\n");
    {
        memset(&e, 0, sizeof(e));
        e.node = 1;
        e.temp_int = 0; e.temp_dec = 5;   /* 这一帧按固件协议不带符号 */
        gw_latest_on_frame(&e, (int64_t)time(NULL));
        gw_latest()->temp_x10 = -5;       /* 直接改成负数，验 js_x10 */
    }
    n = build_response(&srv, "/api/latest", out, sizeof(out));
    check_len(out, n, "/api/latest(neg)");
    dump_body(out, 200);
    CHECK(strstr(out, "\"temp\":-0.5") != NULL, "负温度输出 -0.5（不是 -0.-5）");

    printf("\n[4] 首页 /\n");
    n = build_response(&srv, "/", out, sizeof(out));
    check_len(out, n, "/");
    CHECK(strstr(out, "Content-Type: text/html") != NULL, "Content-Type 是 HTML");
    CHECK(strstr(out, "<!DOCTYPE html>") != NULL, "body 是网页开头");

    printf("\n[5] /index.html 和 / 一样\n");
    n = build_response(&srv, "/index.html", out, sizeof(out));
    check_len(out, n, "/index.html");

    printf("\n[6] 404\n");
    n = build_response(&srv, "/nope", out, sizeof(out));
    check_len(out, n, "/nope");
    CHECK(strstr(out, "HTTP/1.1 404") != NULL, "状态码 404");
    dump_body(out, 80);

    printf("\n[7] /api/history 空库\n");
    n = build_response(&srv, "/api/history", out, sizeof(out));
    check_len(out, n, "/api/history(empty)");
    dump_body(out, 120);
    CHECK(strstr(out, "\"count\":0") != NULL, "空库返回 count:0");

    printf("\n[8] 灌 30 条再查 history?limit=10\n");
    {
        int i;
        for (i = 0; i < 30; i++) {
            memset(&e, 0, sizeof(e));
            e.node = 1;
            e.temp_int = (uint8_t)(20 + i % 5);
            e.temp_dec = (uint8_t)(i % 10);
            e.hum_int = 50; e.hum_dec = 0;
            e.lux = (uint16_t)(500 + i);
            e.gas = (uint16_t)(30 + i);
            gw_db_insert(srv.db, &e, 1000 + i);
        }
        gw_db_commit(srv.db);
    }
    n = build_response(&srv, "/api/history?limit=10", out, sizeof(out));
    check_len(out, n, "/api/history?limit=10");
    dump_body(out, 180);
    CHECK(strstr(out, "\"count\":10") != NULL, "limit=10 生效");

    printf("\n[9] limit 超上限会被卡住（不会撑爆缓冲）\n");
    n = build_response(&srv, "/api/history?limit=999", out, sizeof(out));
    check_len(out, n, "/api/history?limit=999");
    CHECK(n < HTTP_RESP_MAX, "响应没有溢出缓冲");

    printf("\n[10] /api/stats\n");
    n = build_response(&srv, "/api/stats", out, sizeof(out));
    check_len(out, n, "/api/stats");
    dump_body(out, 200);
    CHECK(strstr(out, "\"frames\"") != NULL, "带 frames 字段");

    printf("\n[11] 带查询串的路径匹配（/api/latest?x=1 也该走 latest）\n");
    n = build_response(&srv, "/api/latest?x=1", out, sizeof(out));
    check_len(out, n, "/api/latest?x=1");
    CHECK(strstr(out, "\"valid\":1") != NULL, "查询串不影响路由");

    printf("\n[12] /api/alarms（M4）\n");
    {
        int ida;

        n = build_response(&srv, "/api/alarms", out, sizeof(out));
        check_len(out, n, "/api/alarms(empty)");
        dump_body(out, 100);
        CHECK(strstr(out, "\"count\":0") != NULL, "还没有告警时 count:0");

        ida = gw_db_alarm_begin(srv.db, 1, 3000, 40, 105);
        gw_db_alarm_end(srv.db, ida, 3040, 320);
        ida = gw_db_alarm_begin(srv.db, 1, 3100, 42, 120);
        gw_db_alarm_end(srv.db, ida, 3115, 200);
        gw_db_commit(srv.db);

        n = build_response(&srv, "/api/alarms", out, sizeof(out));
        check_len(out, n, "/api/alarms");
        dump_body(out, 300);
        CHECK(strstr(out, "\"count\":2") != NULL, "2 条告警");
        CHECK(strstr(out, "\"dur\":40") != NULL, "第一条时长 40 秒");
        CHECK(strstr(out, "\"dur\":15") != NULL, "第二条时长 15 秒");
        CHECK(strstr(out, "\"open\":0") != NULL, "都已结束");
        CHECK(strstr(out, "\"peak\":320") != NULL, "峰值 320");

        n = build_response(&srv, "/api/alarms?limit=1", out, sizeof(out));
        check_len(out, n, "/api/alarms?limit=1");
        CHECK(strstr(out, "\"count\":1") != NULL, "limit=1 只回 1 条");

        printf("\n[13] /api/stats 里 alarms 计数\n");
        n = build_response(&srv, "/api/stats", out, sizeof(out));
        check_len(out, n, "/api/stats(alarms)");
        dump_body(out, 200);
        CHECK(strstr(out, "\"alarms\":2") != NULL, "alarms=2");
    }

    gw_db_close(srv.db);
    remove("tests/test_http.db");
    remove("tests/test_http.db-wal");
    remove("tests/test_http.db-shm");

    printf("\n===== %s（失败 %d 项）=====\n", fails ? "有用例失败" : "全部通过", fails);
    return fails ? 1 : 0;
}
