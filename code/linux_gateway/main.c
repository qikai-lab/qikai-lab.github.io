/*
 * main.c - Linux 物联网边缘网关
 * ---------------------------------------------------------------------------
 * 进度：M1 串口 → 解析 → 打印  /  M2 SQLite 落库  /  M3 内置 HTTP + 网页
 *
 * 能力：
 *   [x] 串口非阻塞读（termios raw + O_NONBLOCK）+ epoll 事件驱动
 *   [x] st: 24 字节协议解析（定长 + 帧头重同步 + 字段校验）
 *   [x] 帧头失步 / 粘包 / 半帧 / 串口丢字节 自动恢复
 *   [x] SQLite 落库（WAL + 批量提交 + 预处理语句）
 *   [x] USB 串口掉线自动重连
 *   [x] signalfd 优雅退出（退出前提交事务，不丢最后几条）
 *   [x] 运行统计（有效帧 / 坏帧 / 残帧 / 重同步字节 / 入库行数）
 *   [x] M3 内置 HTTP：/  /api/latest  /api/history  /api/stats
 *   [x] 网页曲线（无外部依赖，内嵌在二进制里）
 *
 * 架构（M3 的关键决定）
 * ---------------------
 *   **串口和 HTTP 跑在同一个进程、同一个线程、同一个 epoll 里。**
 *
 *       串口 ──► 解析 ──► 内存快照 gw_latest ──► /api/latest  （0 延迟）
 *                     └─► SQLite（攒 20 条提交）─► /api/history（查历史）
 *
 *   实时值走内存、历史走库，是因为 SQLite 未提交的行别的连接看不见：
 *   若实时值也查库，网页上的「当前温度」最多会滞后 19 秒。
 *   单线程还有一个好处 —— 全程无锁。
 *
 * 后续（M4 / M5）：
 *   [ ] 烟雾告警规则 + 远控继电器
 *   [ ] 低功耗 / systemd 服务
 *
 * 编译：make
 * 运行：./gw -d /dev/ttyUSB0 -b 115200 -f ./gw.db -p 8080
 * 看数据：浏览器打开 http://<这台机器的IP>:8080/
 *
 * 没有硬件怎么测（Ubuntu）：
 *   终端1: socat -d -d pty,raw,echo=0 pty,raw,echo=0
 *          记下两个 /dev/pts/N
 *   终端2: ./gw -d /dev/pts/5
 *   终端3: python3 tools/sim_node.py /dev/pts/6
 * ---------------------------------------------------------------------------
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <getopt.h>
#include <time.h>
#include <stdint.h>

#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

#include "gw_serial.h"
#include "gw_frame.h"
#include "gw_db.h"
#include "gw_latest.h"
#include "gw_filter.h"
#include "gw_alarm.h"
#include "http_srv.h"

#define RECONNECT_INTERVAL_MS   1000U
#define STATS_INTERVAL_MS       5000U
#define READ_BUF_SIZE           256
#define HTTP_TICK_MS            1000U

/* 自适应基线的时间常数（帧数 = 秒）。600 秒约 10 分钟。
 * 太小：真烟雾持续久了会被当成新常态，告警自己消失（基线的经典坑）；
 * 太大：换环境后要等很久才准。告警期间还会冻结，所以 600 是安全的。 */
#define GW_BASELINE_TAU         600U

typedef struct {
    gw_db_t *db;            /* 可能为 NULL（打不开库时降级为只打印） */
    int      quiet;         /* 1 = 不打印每帧 */

    /* ---- M4 数据加工链：原始 gas → 中值 → 基线 → 状态机 ---- */
    gw_medfilt_t  med;      /* 中值滤波 */
    gw_baseline_t base;     /* 自适应基线 */
    gw_alarm_t    alarm;    /* 告警状态机 */
    int           alarm_id; /* 当前告警在库里的行 id；0 = 没在告警 */
} gw_ctx_t;

static volatile sig_atomic_t g_running = 1;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* 打印用的本地时间 HH:MM:SS */
static void hhmmss(char *out, size_t n)
{
    time_t    t = time(NULL);
    struct tm tm_buf;

    if (localtime_r(&t, &tm_buf) == NULL) {
        snprintf(out, n, "--:--:--");
        return;
    }
    strftime(out, n, "%H:%M:%S", &tm_buf);
}

/**
 * @brief 打印本机的局域网 IPv4，让人知道浏览器该开哪个地址
 * @note  跳过 127.0.0.1（那个只有虚拟机自己能访问）。
 *        虚拟机里通常是 ens33 / ens160 之类的网卡。
 */
static int print_lan_ip(int port)
{
    struct ifaddrs *ifa = NULL;
    struct ifaddrs *p;
    int             printed = 0;

    if (getifaddrs(&ifa) != 0) {
        return 0;
    }
    for (p = ifa; p != NULL; p = p->ifa_next) {
        struct sockaddr_in *sin;
        char                ip[INET_ADDRSTRLEN];

        if (p->ifa_addr == NULL || p->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        sin = (struct sockaddr_in *)p->ifa_addr;
        if (ntohl(sin->sin_addr.s_addr) == 0x7F000001U) {
            continue;       /* 127.0.0.1 */
        }
        if (inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip)) == NULL) {
            continue;
        }
        printf(" 网页   : http://%s:%d/\n", ip, port);
        printed = 1;
        break;              /* 只打第一个，免得虚拟网卡一堆刷屏 */
    }
    freeifaddrs(ifa);
    return printed;
}

/*
 * 帧回调：更新快照 → 数据加工 → 打印 → 落库
 *
 * 顺序有三条硬约束，都不能换：
 *
 *  1) **加工必须在打印之前**。打印出来的 gas 是滤波后的值和当前基线，
 *     不然屏幕上看到的是"原始 45"，而状态机用的是"滤波 35"，对不上。
 *
 *  2) **快照更新必须在落库之前**。落库是攒 20 条才提交，未提交的行
 *     别的连接看不见，但网页要的是"现在"。
 *
 *  3) **告警判定必须在基线更新之前**。基线是跟着采样慢慢漂的，
 *     如果先拿这一帧的高值把基线抬上去，再判 2.5 倍就永远够不着 ——
 *     烟越浓、基线涨得越快，形成自锁。这是自适应基线的经典陷阱。
 */
static void on_env(const gw_env_t *e, void *user)
{
    gw_ctx_t      *ctx = (gw_ctx_t *)user;
    gw_latest_t   *L   = gw_latest();
    int64_t        ts  = (int64_t)time(NULL);
    gw_alarm_ev_t  ev;
    uint16_t       med;
    char           tbuf[16];

    /* 里面有 frames++，别在别处再加 */
    gw_latest_on_frame(e, ts);

    /* ---- M4 加工链 ---- */
    /* ① 中值滤波：吃掉单帧尖峰。传入原始 ADC 值。 */
    med = gw_medfilt_push(&ctx->med, e->gas);

    /* ② 首帧先把基线建起来，否则第一秒的比值是 0%（看着像故障）。 */
    if (!gw_baseline_valid(&ctx->base)) {
        gw_baseline_update(&ctx->base, med, 0);
    }

    /* ③ 状态机：用的是"烟雾到来之前"的基线 */
    ev = gw_alarm_feed(&ctx->alarm, med, &ctx->base, ts);

    /* ④ 基线按状态机的**新状态**决定要不要冻结（告警中不动） */
    gw_baseline_update(&ctx->base, med,
                       ctx->alarm.state == ALARM_ST_ALARM);

    /* 把加工结果写进快照，HTTP 直接读，不重复算 */
    L->gas_med          = med;
    L->gas_base         = gw_baseline_value(&ctx->base);
    L->gas_pct          = ctx->alarm.pct;
    L->alarm_state      = ctx->alarm.state;
    L->alarm_peak       = ctx->alarm.peak_gas;
    L->alarm_total      = ctx->alarm.total;
    L->alarm_active_sec = ctx->alarm.active_sec;

    hhmmss(tbuf, sizeof(tbuf));

    if (!ctx->quiet) {
        printf("[%s] #%-5u n%-3u %2d.%dC %3d.%d%%  lux=%4u(%3d%%)"
               "  gas=%4u med=%4u base=%4u %3u%%  PIR=%s  RLY=%s%s\n",
               tbuf,
               (unsigned)L->frames,
               (unsigned)e->node,
               (int)e->temp_int, (int)e->temp_dec,
               (int)e->hum_int,  (int)e->hum_dec,
               (unsigned)e->lux, gw_env_lux_pct(e),
               (unsigned)e->gas,
               (unsigned)med,
               (unsigned)L->gas_base,
               (unsigned)ctx->alarm.pct,
               e->pir   ? "有人" : "无人",
               e->relay ? "ON"  : "OFF",
               (ctx->alarm.state == ALARM_ST_ALARM) ? "  <<< 烟雾告警"
             : (ctx->alarm.state == ALARM_ST_WARN)  ? "  <<< 疑似"
             : "");
        fflush(stdout);
    }

    /* ---- 告警的起止：打印醒目横幅 + 写库 ---- */
    if (ev == ALARM_EV_BEGIN) {
        printf("\n"
               "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
               "!! [%s] 烟雾告警  第 %u 次\n"
               "!!   gas=%u  基线=%u  当前是基线的 %u%%（触发线 %d%%）\n"
               "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",
               tbuf, (unsigned)ctx->alarm.total,
               (unsigned)ctx->alarm.gas, (unsigned)ctx->alarm.base,
               (unsigned)ctx->alarm.pct, ALARM_TRIG_PCT);
        fflush(stdout);

        ctx->alarm_id = (ctx->db != NULL)
            ? gw_db_alarm_begin(ctx->db, (int)e->node, ts,
                                (int)ctx->alarm.base, (int)ctx->alarm.gas)
            : 0;
        if (ctx->db != NULL && ctx->alarm_id <= 0) {
            L->db_fail++;
        }
    } else if (ev == ALARM_EV_END) {
        printf("---- [%s] 烟雾告警解除，持续 %u 秒，峰值 gas=%u ----\n",
               tbuf,
               (unsigned)ctx->alarm.last_sec,
               (unsigned)ctx->alarm.peak_gas);
        fflush(stdout);

        if (ctx->db != NULL && ctx->alarm_id > 0) {
            if (gw_db_alarm_end(ctx->db, ctx->alarm_id, ts,
                                (int)ctx->alarm.peak_gas) != 0) {
                L->db_fail++;
            }
        }
        ctx->alarm_id = 0;
    }

    if (ctx->db != NULL) {
        if (gw_db_insert(ctx->db, e, ts) == 0) {
            L->db_ok++;
        } else {
            L->db_fail++;
            /* 只在第一次失败时刷屏，避免每秒一条把终端冲爆 */
            if (L->db_fail == 1U) {
                printf("[db] 入库失败：%s（后续失败不再逐条打印）\n",
                       gw_db_last_error(ctx->db));
                fflush(stdout);
            }
        }
    }
}

static void usage(const char *prog)
{
    printf("用法: %s [-d 设备] [-b 波特率] [-f 数据库] [-n 批量] [-p 端口] [-q] [-h]\n", prog);
    printf("  -d  串口设备，默认 /dev/ttyUSB0\n");
    printf("  -b  波特率，默认 115200\n");
    printf("  -f  SQLite 文件，默认 ./gw.db\n");
    printf("  -n  攒多少条提交一次事务，默认 20\n");
    printf("  -p  HTTP 端口，默认 %d；填 0 表示关闭网页服务\n", HTTP_DEFAULT_PORT);
    printf("  -q  安静模式：不打印每一帧\n");
    printf("  -h  显示本帮助\n");
}

int main(int argc, char *argv[])
{
    const char    *dev   = "/dev/ttyUSB0";
    const char    *dbf   = "./gw.db";
    int            baud  = 115200;
    int            batch = GW_DB_DEFAULT_BATCH;
    int            quiet = 0;
    int            port  = HTTP_DEFAULT_PORT;
    int            opt;

    gw_serial_t    ser;
    gw_parser_t    parser;
    gw_latest_t   *L;
    gw_ctx_t       ctx;
    http_srv_t    *hs = NULL;
    uint8_t        rbuf[READ_BUF_SIZE];

    int            epfd;
    int            sfd;
    sigset_t       mask;
    struct epoll_event ev;
    struct epoll_event events[8];       /* 串口 + 信号 + 监听 + 若干客户端 */

    uint64_t       next_reconnect = 0;
    uint64_t       next_stats     = 0;
    uint64_t       next_http_tick = 0;

    while ((opt = getopt(argc, argv, "d:b:f:n:p:qh")) != -1) {
        switch (opt) {
        case 'd': dev   = optarg; break;
        case 'b': baud  = atoi(optarg); break;
        case 'f': dbf   = optarg; break;
        case 'n': batch = atoi(optarg); break;
        case 'p': port  = atoi(optarg); break;
        case 'q': quiet = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    /*
     * 用 signalfd 而不是 signal()/sigaction()
     * 原因：信号处理函数里只能调用异步信号安全函数，限制极多；
     *       转成 fd 后用 epoll 统一处理，逻辑干净，这是标准做法。
     */
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        perror("sigprocmask");
        return 1;
    }
    sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd < 0) {
        perror("signalfd");
        return 1;
    }

    /* 全局快照：网页读的就是它，和 SQLite 完全解耦 */
    gw_latest_init();
    L = gw_latest();

    /* M4 加工链的三个状态。ctx 是栈变量，不初始化会带着垃圾跑。 */
    gw_medfilt_init(&ctx.med);
    gw_baseline_init(&ctx.base, GW_BASELINE_TAU);
    gw_alarm_init(&ctx.alarm);
    ctx.alarm_id = 0;

    /* 打开数据库：失败不算致命错误，降级为「只打印不落库」继续跑 */
    ctx.db    = gw_db_open(dbf);
    ctx.quiet = quiet;
    if (ctx.db != NULL) {
        gw_db_set_batch(ctx.db, batch);
    }

    gw_parser_init(&parser, on_env, &ctx);

    /*
     * 串口打不开不再退出。
     *
     * 以前这里是 return 1，配合 systemd 的 Restart=always 会变成
     * "每 3 秒拉一次、每次立刻死" 的重启风暴；更糟的是 HTTP 监听建在
     * 串口之后，串口不在时网页也跟着挂掉 —— 而那恰恰是最需要看数据的时候。
     *
     * 现在改成：记下目标设备和波特率，继续往下走，由主循环里那段
     * "每秒重连" 的代码反复尝试。网页照常能开，页面上显示串口未连接。
     * 串口什么时候插上，什么时候自己接上，不需要重启进程。
     */
    if (gw_serial_open(&ser, dev, baud) != 0) {
        fprintf(stderr, "[警告] 打开串口 %s 失败: %s\n", dev, strerror(errno));
        fprintf(stderr, "[警告] 不退出：进入主循环每秒重试，网页仍可访问\n");
        fprintf(stderr, "[提示] 没有硬件时可用 socat 造虚拟串口，见文件头注释\n");
        next_reconnect = now_ms() + RECONNECT_INTERVAL_MS;
    }

    printf("========================================\n");
    printf(" Linux IoT Gateway  (M4 采集+告警+网页)\n");
    printf("========================================\n");
    printf(" 串口   : %s @ %d 8-N-1%s\n", ser.dev, baud,
           ser.fd < 0 ? "  [当前未连接，每秒重试中]" : "");
    printf(" 数据库 : %s%s\n", dbf, ctx.db ? "" : "  [打开失败，仅打印]");
    printf(" 批次   : %d 条提交一次\n", batch);
    printf(" 协议   : st: 24 字节固定帧\n");
    printf(" 告警   : gas ≥ 基线×%d%% 连续 %d 帧确认；"
           "≤ %d%% 连续 %d 帧解除；基线 τ=%us\n",
           ALARM_TRIG_PCT, ALARM_CONFIRM_N,
           ALARM_CLR_PCT, ALARM_CLEAR_N, GW_BASELINE_TAU);

    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        perror("epoll_create1");
        gw_serial_close(&ser);
        gw_db_close(ctx.db);
        close(sfd);
        return 1;
    }

    /* 串口可能还没打开（fd<0），这时不要往 epoll 里塞一个非法 fd */
    if (ser.fd >= 0) {
        memset(&ev, 0, sizeof(ev));
        ev.events   = EPOLLIN;
        ev.data.fd  = ser.fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, ser.fd, &ev);
    }

    ev.events  = EPOLLIN;
    ev.data.fd = sfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &ev);

    /* ---- HTTP：端口 0 表示不要网页 ---- */
    if (port > 0) {
        hs = http_srv_open(port, ctx.db);
        if (hs == NULL) {
            printf(" 网页   : [启动失败] 端口 %d 可能被占用，采集继续\n", port);
        } else {
            http_srv_set_epoll(hs, epfd);
            memset(&ev, 0, sizeof(ev));
            ev.events  = EPOLLIN;
            ev.data.fd = http_srv_listen_fd(hs);
            if (epoll_ctl(epfd, EPOLL_CTL_ADD, http_srv_listen_fd(hs), &ev) != 0) {
                printf(" 网页   : [监听注册失败]，采集继续\n");
                http_srv_close(hs);
                hs = NULL;
            }
        }
    } else {
        printf(" 网页   : [已关闭 -p 0]\n");
    }

    if (hs != NULL) {
        if (!print_lan_ip(http_srv_port(hs))) {
            printf(" 网页   : http://localhost:%d/\n", http_srv_port(hs));
        }
        printf(" 接口   : /api/latest  /api/history?limit=60  /api/stats\n");
    }
    printf(" 退出   : Ctrl+C\n");
    printf("----------------------------------------\n");
    fflush(stdout);

    next_stats     = now_ms() + STATS_INTERVAL_MS;
    next_http_tick = now_ms() + HTTP_TICK_MS;

    while (g_running) {

        int n = epoll_wait(epfd, events, (int)(sizeof(events) / sizeof(events[0])), 200);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {

            /* 退出信号 */
            if (events[i].data.fd == sfd) {
                struct signalfd_siginfo si;
                if (read(sfd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
                    printf("\n[退出] 收到信号 %u，正在优雅关闭...\n", si.ssi_signo);
                    g_running = 0;
                }
                continue;
            }

            /* HTTP 新连接：http_srv 自己会把这些 fd 注册进 epoll */
            if (hs != NULL && events[i].data.fd == http_srv_listen_fd(hs)) {
                http_srv_accept(hs);
                continue;
            }

            /* HTTP 客户端可读 */
            if (hs != NULL) {
                int idx = http_srv_client_index(hs, events[i].data.fd);
                if (idx >= 0) {
                    http_srv_handle(hs, idx);
                    continue;
                }
            }

            /* 串口可读 */
            if (events[i].data.fd == ser.fd) {

                /*
                 * EPOLLHUP / EPOLLERR：USB 转串口被拔掉时内核会报这个。
                 * 不处理的话，read() 可能一直返回 0（EOF），而原逻辑把 0 当成
                 * "暂无数据"，于是永远不触发重连，串口就静默死掉了。
                 * 这两个事件不需要显式注册，只要 fd 在 epoll 里就会被报告。
                 */
                if ((events[i].events & (EPOLLHUP | EPOLLERR)) != 0U) {
                    printf("[错误] 串口挂断（设备可能被拔出），准备重连\n");
                    epoll_ctl(epfd, EPOLL_CTL_DEL, ser.fd, NULL);
                    gw_serial_close(&ser);
                    /* 丢弃断线前的残留半帧，否则它会和重连后的第一个字节
                     * 拼成一个假帧（字段校验能拦住，但会多记一次坏帧） */
                    gw_parser_reset(&parser);
                    next_reconnect = now_ms() + RECONNECT_INTERVAL_MS;
                    continue;
                }

                int got = gw_serial_read(&ser, rbuf, sizeof(rbuf));

                if (got > 0) {
                    gw_parser_feed(&parser, rbuf, (size_t)got);
                } else if (got == GW_SERIAL_EOF) {
                    /* 只有 EPOLLIN、没有 EPOLLHUP 的驱动走这条路。
                     * 不处理会变成 100% CPU 忙等（每次 epoll 都报可读、每次读回 0）。 */
                    printf("[错误] 串口读到 EOF（设备已断开），准备重连\n");
                    epoll_ctl(epfd, EPOLL_CTL_DEL, ser.fd, NULL);
                    gw_serial_close(&ser);
                    gw_parser_reset(&parser);
                    next_reconnect = now_ms() + RECONNECT_INTERVAL_MS;
                } else if (got < 0) {
                    printf("[错误] 串口异常 (%s)，准备重连\n", strerror(errno));
                    epoll_ctl(epfd, EPOLL_CTL_DEL, ser.fd, NULL);
                    gw_serial_close(&ser);
                    gw_parser_reset(&parser);
                    next_reconnect = now_ms() + RECONNECT_INTERVAL_MS;
                }
            }
        }

        /* 串口在线状态：让网页能区分「串口没连上」和「连上了但没数据」。
           这两件事在数据上完全一样（都是 0 帧），但处理方式完全不同。 */
        L->serial_ok = (ser.fd >= 0) ? 1U : 0U;

        /* 重连逻辑：每秒尝试一次 */
        if (ser.fd < 0 && now_ms() >= next_reconnect) {
            if (gw_serial_reopen(&ser) == 0) {
                L->reconnects++;
                ev.events  = EPOLLIN;
                ev.data.fd = ser.fd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, ser.fd, &ev);
                printf("[恢复] 串口重连成功，第 %u 次\n", L->reconnects);
            } else {
                next_reconnect = now_ms() + RECONNECT_INTERVAL_MS;
            }
        }

        /* HTTP：关掉超时没动静的连接（浏览器预连接会占槽位） */
        if (hs != NULL && now_ms() >= next_http_tick) {
            http_srv_tick(hs, (int64_t)now_ms());
            next_http_tick = now_ms() + HTTP_TICK_MS;
        }

        /* 定时打印统计（顺便把解析器的统计同步到快照，/api/stats 才有数） */
        if (now_ms() >= next_stats) {
            L->bad     = parser.stat_bad;
            L->shortf  = parser.stat_short;
            L->resync  = parser.stat_resync;
            L->db_rows = (ctx.db != NULL) ? (uint32_t)gw_db_count_rows(ctx.db) : 0U;

            printf("---- 有效帧=%u  坏帧=%u  残帧=%u  重同步=%uB  入库=%u  库错=%u  重连=%u ----\n",
                   L->frames,
                   parser.stat_bad,
                   parser.stat_short,
                   parser.stat_resync,
                   L->db_ok,
                   ctx.db ? gw_db_err_count(ctx.db) : 0U,
                   L->reconnects);

            /* 告警状态单独一行，正常的时候只在末尾标一句"正常"，
               出事时把倍数和峰值打出来，方便对着时间点查日志 */
            if (ctx.alarm.state == ALARM_ST_NORMAL) {
                printf("---- 烟雾：正常  基线=%u  倍数=%u%%  累计告警=%u 次 ----\n",
                       (unsigned)L->gas_base, (unsigned)ctx.alarm.pct,
                       (unsigned)ctx.alarm.total);
            } else {
                printf("---- 烟雾：%s  基线=%u  倍数=%u%%  峰值=%u  "
                       "已持续=%u秒  累计告警=%u 次 ----\n",
                       (ctx.alarm.state == ALARM_ST_ALARM) ? "告警中" : "疑似",
                       (unsigned)L->gas_base, (unsigned)ctx.alarm.pct,
                       (unsigned)ctx.alarm.peak_gas,
                       (unsigned)ctx.alarm.active_sec,
                       (unsigned)ctx.alarm.total);
            }
            fflush(stdout);
            next_stats = now_ms() + STATS_INTERVAL_MS;
        }
    }

    printf("[退出] 提交未落盘数据并关闭资源\n");

    /* 退出时如果还在告警，先把这条记录收尾，
     * 否则库里留一条 end_ts 为 NULL 的记录，下次启动会被当成异常退出清理。 */
    if (ctx.db != NULL && ctx.alarm_id > 0) {
        (void)gw_db_alarm_end(ctx.db, ctx.alarm_id, (int64_t)time(NULL),
                              (int)ctx.alarm.peak_gas);
        ctx.alarm_id = 0;
    }

    /* 顺序很重要：先提交事务，再关串口，最后关库 */
    if (ctx.db != NULL) {
        if (gw_db_commit(ctx.db) != 0) {
            printf("[退出] 警告：事务提交失败 %s\n", gw_db_last_error(ctx.db));
        }
        printf("[退出] 库内累计 %lld 条\n",
               (long long)gw_db_total_rows(ctx.db));
    }
    printf("[退出] 本次运行：有效帧=%u  坏帧=%u  残帧=%u  入库=%u  入库失败=%u\n",
           L->frames, parser.stat_bad, parser.stat_short,
           L->db_ok, L->db_fail);

    http_srv_close(hs);
    gw_serial_close(&ser);
    gw_db_close(ctx.db);
    close(sfd);
    close(epfd);
    return 0;
}
