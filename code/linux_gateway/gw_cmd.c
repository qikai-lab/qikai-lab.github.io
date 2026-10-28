/*
 * gw_cmd.c - 向 MQTT broker 发布一条下行命令（第 5 步 · 网页远程控制）
 *
 * 报文格式（MQTT 3.1.1，OASIS 标准）：
 *
 *   CONNECT                                  PUBLISH (QoS0)
 *   ┌──────┬────────┬─────────────┐          ┌──────┬────────┬───────────────┬─────────┐
 *   │ 0x10 │ 剩余长 │ 变长头+载荷 │          │ 0x30 │ 剩余长 │ 2字节主题长 + │ payload │
 *   └──────┴────────┴─────────────┘          └──────┴────────┴──────主题──────┴─────────┘
 *     变长头：00 04 "MQTT"  04(3.1.1)  02(clean)  00 3C(保活60s)
 *     载荷　：2字节clientId长度 + clientId
 *
 * 实际字节（clientId="gw", topic="zigbee/node1/cmd", payload="1"）：
 *   CONNECT  = 10 0E 00 04 4D 51 54 54 04 02 00 3C 00 02 67 77
 *              （剩余长度 14 = 0x0E = 变长头 10 + ID长度字段 2 + "gw" 2）
 *   PUBLISH  = 30 13 00 10 7A 69 67 62 65 65 2F 6E 6F 64 65 31 2F 63 6D 64 31
 *              （剩余长度 19 = 0x13 = 2 + 主题 16 + "1" 1）
 *   这两个字节串在 tests/test_cmd.c 里有断言，改代码改错了会当场报出来。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE             /* 为了 getaddrinfo 与 O_NONBLOCK 的完整声明 */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gw_cmd.h"

/* 只有 POSIX 且不是"只要拼包"的模式才需要 socket 头。
 * Windows(MinGW) 没有 <sys/socket.h>，走下面的桩分支。 */
#if !defined(GW_CMD_CODEC_ONLY) && !defined(_WIN32)
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#endif

/* ===========================================================================
 * 一、拼包（纯函数，不碰网络 —— 所以可以在任何机器上离线单元测试）
 * ========================================================================= */

/**
 * @brief 写 MQTT 的"剩余长度"变长整数
 *
 * 规则：每字节低 7 位是数据，最高位是"还有后续"。所以 0~127 用 1 字节，
 * 128~16383 用 2 字节…… 我们这个场景（1 字节 payload）永远是 1 字节，
 * 但主题名长一点就可能到 2 字节，所以还是按标准写完整。
 */
static size_t put_remlen(unsigned char *p, size_t n)
{
    size_t i = 0;

    do {
        unsigned char b = (unsigned char)(n % 128U);

        n /= 128U;
        if (n > 0) {
            b |= 0x80U;         /* 告诉对方"后面还有" */
        }
        p[i++] = b;
    } while (n > 0 && i < 4U);  /* 标准规定最多 4 字节 */

    return i;
}

size_t gw_cmd_build_connect(unsigned char *buf, size_t cap, const char *client_id)
{
    size_t idlen;
    size_t rem;
    size_t i = 0;

    if (buf == NULL || client_id == NULL) {
        return 0;
    }

    idlen = strlen(client_id);
    if (idlen > 23U) {
        idlen = 23U;            /* 3.1.1 允许 1~23 字节的短 ID，再长也可能被拒 */
    }

    /* 变长头 10 字节 + clientId 长度字段 2 字节 + clientId 本身 */
    rem = 10U + 2U + idlen;
    if (cap < 1U + 4U + rem) {
        return 0;
    }

    buf[i++] = 0x10U;           /* CONNECT */
    i += put_remlen(buf + i, rem);

    buf[i++] = 0x00;            /* 协议名长度 = 4 */
    buf[i++] = 0x04;
    buf[i++] = 'M';
    buf[i++] = 'Q';
    buf[i++] = 'T';
    buf[i++] = 'T';

    buf[i++] = 0x04;            /* 协议级别 4 = 3.1.1 */
    buf[i++] = 0x02;            /* 连接标志：clean session（bit1），无账号无遗愿 */
    buf[i++] = 0x00;            /* 保活 60 秒（高低字节） */
    buf[i++] = 0x3C;

    buf[i++] = (unsigned char)((idlen >> 8) & 0xFFU);
    buf[i++] = (unsigned char)(idlen & 0xFFU);
    memcpy(buf + i, client_id, idlen);
    i += idlen;

    return i;
}

size_t gw_cmd_build_publish(unsigned char *buf, size_t cap,
                            const char *topic, const char *payload)
{
    size_t tlen;
    size_t plen;
    size_t rem;
    size_t i = 0;

    if (buf == NULL || topic == NULL || payload == NULL) {
        return 0;
    }

    tlen = strlen(topic);
    plen = strlen(payload);

    /* 主题长度字段是 2 字节，理论上限 65535；这里给个更保守的界，
     * 顺便防止有人把一整段 HTML 当 payload 塞进来 */
    if (tlen == 0U || tlen > 512U || plen > 1024U) {
        return 0;
    }

    rem = 2U + tlen + plen;
    if (cap < 1U + 4U + rem) {
        return 0;
    }

    buf[i++] = 0x30U;           /* PUBLISH，QoS0，无重发标志，无 packet id */
    i += put_remlen(buf + i, rem);

    buf[i++] = (unsigned char)((tlen >> 8) & 0xFFU);
    buf[i++] = (unsigned char)(tlen & 0xFFU);
    memcpy(buf + i, topic, tlen);
    i += tlen;

    memcpy(buf + i, payload, plen);
    i += plen;

    return i;
}

/* ===========================================================================
 * 二、错误码翻译（所有平台都有，网页上把这句话原样显示给用户）
 * ========================================================================= */
const char *gw_cmd_strerror(int rc)
{
    switch (rc) {
    case GW_CMD_OK:            return "已发出";
    case GW_CMD_ERR_ADDR:      return "broker 地址解析失败";
    case GW_CMD_ERR_CONNECT:   return "连不上 broker（mosquitto 起了吗）";
    case GW_CMD_ERR_SEND:      return "发送失败（连接被提前关闭）";
    case GW_CMD_ERR_REFUSED:   return "broker 拒绝连接";
    case GW_CMD_ERR_ARG:       return "参数非法";
    case GW_CMD_ERR_PLATFORM:  return "这是 Windows 编译版（网关要跑在 Ubuntu）";
    default:                   return "未知错误";
    }
}

/* ===========================================================================
 * 三、网络（只在真机上跑，测试时用 GW_CMD_CODEC_ONLY 跳过）
 * ========================================================================= */
#ifndef GW_CMD_CODEC_ONLY

#if defined(_WIN32)

/* ---------------------------------------------------------------------------
 * Windows 桩
 *
 * 网关真正部署在家里的 Ubuntu 上，Windows 只是写代码和跑单元测试的机器。
 * 但 test_http / test_chain 里 #include 了 http_srv.c，而 http_srv.c 现在
 * 调用了 gw_cmd_publish_default()，所以 gw_cmd.c 必须能在 MinGW 下编译通过，
 * 否则整条 Windows 测试链就断了（缺符号）。
 *
 * 真实实现用的是 POSIX socket（<sys/socket.h> / getaddrinfo），MinGW 没有。
 * 所以这里给一对桩：编得过、链接得上、调了直接返回"平台不支持"。
 * 这样在 Windows 上误点按钮会得到一句明确原因，而不是莫名其妙地"成功"。
 * ------------------------------------------------------------------------- */
int gw_cmd_publish(const char *host, int port, const char *topic,
                   const char *payload, int timeout_ms)
{
    (void)host;
    (void)port;
    (void)topic;
    (void)payload;
    (void)timeout_ms;
    return GW_CMD_ERR_PLATFORM;
}

int gw_cmd_publish_default(const char *payload)
{
    (void)payload;
    return GW_CMD_ERR_PLATFORM;
}

#else  /* ---------- 以下为 POSIX 真实实现（Ubuntu / 任何 Linux）---------- */

static int set_nonblock(int fd, int on)
{
    int fl = fcntl(fd, F_GETFL, 0);

    if (fl < 0) {
        return -1;
    }
    fl = on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, fl);
}

/** 等 fd 可写（非阻塞 connect 用它判断"连上了没有"） */
static int wait_fd(int fd, int for_write, int timeout_ms)
{
    fd_set          set;
    struct timeval  tv;
    int             rc;
    int             soerr = 0;
    socklen_t       slen  = (socklen_t)sizeof(soerr);

    FD_ZERO(&set);
    FD_SET(fd, &set);
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    rc = select(fd + 1,
                for_write ? NULL : &set,
                for_write ? &set : NULL,
                NULL, &tv);
    if (rc <= 0) {
        return -1;              /* 超时或出错 */
    }

    /* select 说可写不等于连上了，还要问一次 SO_ERROR：
     * 连被拒绝时 select 也会立刻返回可写。 */
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) != 0 || soerr != 0) {
        return -1;
    }
    return 0;
}

/** 返回 fd（>=0）或 GW_CMD_ERR_* */
static int tcp_open(const char *host, int port, int timeout_ms)
{
    struct addrinfo  hints;
    struct addrinfo *res = NULL;
    struct addrinfo *p;
    char             portstr[16];
    int              fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;      /* v4/v6 都行，127.0.0.1 会走 v4 */
    hints.ai_socktype = SOCK_STREAM;

    snprintf(portstr, sizeof(portstr), "%d", port);

    if (getaddrinfo(host, portstr, &hints, &res) != 0 || res == NULL) {
        return GW_CMD_ERR_ADDR;
    }

    for (p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (set_nonblock(fd, 1) != 0) {
            close(fd);
            fd = -1;
            continue;
        }

        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            break;              /* 本机连本机，常常一次就成 */
        }
        if (errno == EINPROGRESS && wait_fd(fd, 1, timeout_ms) == 0) {
            break;              /* 等到了 */
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);

    if (fd < 0) {
        return GW_CMD_ERR_CONNECT;
    }

    /* 换回阻塞模式发数据：报文只有几十字节，一次 write 就能写完，
     * 阻塞反而比"非阻塞 + 自己攒发送缓冲"简单得多。 */
    if (set_nonblock(fd, 0) != 0) {
        close(fd);
        return GW_CMD_ERR_CONNECT;
    }

    return fd;
}

static int write_all(int fd, const unsigned char *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, 0);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

int gw_cmd_publish(const char *host, int port, const char *topic,
                   const char *payload, int timeout_ms)
{
    unsigned char pkt[1600];
    unsigned char ack[4];
    size_t        n;
    int           fd;
    int           rc = GW_CMD_OK;
    ssize_t       got;

    if (host == NULL || topic == NULL || payload == NULL || port <= 0) {
        return GW_CMD_ERR_ARG;
    }
    if (timeout_ms <= 0) {
        timeout_ms = GW_CMD_DEFAULT_TIMEOUT_MS;
    }

    fd = tcp_open(host, port, timeout_ms);
    if (fd < 0) {
        return fd;              /* 已经是 GW_CMD_ERR_* */
    }

    /* ---- ① CONNECT ---- */
    n = gw_cmd_build_connect(pkt, sizeof(pkt), "zigbee-gw");
    if (n == 0U) {
        close(fd);
        return GW_CMD_ERR_ARG;
    }
    if (write_all(fd, pkt, n) != 0) {
        close(fd);
        return GW_CMD_ERR_SEND;
    }

    /* ---- ② 等 CONNACK：固定 4 字节 20 02 00 00 ----
     * 必须等，否则 mosquitto 可能还没建好会话就把后面的 PUBLISH 丢掉。
     * 这一步也是"broker 到底在不在"的唯一可靠判据：端口通但对方不是
     * MQTT 服务时，这里会超时，而不是稀里糊涂地"发成功了"。 */
    if (wait_fd(fd, 0, timeout_ms) != 0) {
        close(fd);
        return GW_CMD_ERR_CONNECT;
    }

    got = recv(fd, ack, sizeof(ack), 0);
    if (got != (ssize_t)sizeof(ack) || ack[0] != 0x20 || ack[1] != 0x02) {
        close(fd);
        return GW_CMD_ERR_REFUSED;
    }
    if (ack[3] != 0x00) {
        close(fd);
        return GW_CMD_ERR_REFUSED;   /* 0x01 协议版本不支持，0x05 未授权 */
    }

    /* ---- ③ PUBLISH ---- */
    n = gw_cmd_build_publish(pkt, sizeof(pkt), topic, payload);
    if (n == 0U) {
        close(fd);
        return GW_CMD_ERR_ARG;
    }
    if (write_all(fd, pkt, n) != 0) {
        rc = GW_CMD_ERR_SEND;
    }

    /* ---- ④ DISCONNECT（尽力而为，失败不影响结论）---- */
    {
        static const unsigned char bye[2] = { 0xE0, 0x00 };

        (void)write_all(fd, bye, sizeof(bye));
    }

    close(fd);
    return rc;
}

int gw_cmd_publish_default(const char *payload)
{
    const char *host  = getenv("GW_MQTT_HOST");
    const char *topic = getenv("GW_CMD_TOPIC");
    const char *pstr  = getenv("GW_MQTT_PORT");
    int         port  = GW_CMD_DEFAULT_PORT;

    if (host == NULL || host[0] == '\0') {
        host = GW_CMD_DEFAULT_HOST;
    }
    if (topic == NULL || topic[0] == '\0') {
        topic = GW_CMD_DEFAULT_TOPIC;
    }
    if (pstr != NULL && pstr[0] != '\0') {
        int v = atoi(pstr);

        if (v > 0 && v <= 65535) {
            port = v;
        }
    }

    return gw_cmd_publish(host, port, topic, payload, GW_CMD_DEFAULT_TIMEOUT_MS);
}

#endif /* !_WIN32 —— POSIX 真实实现到此结束 */

#endif /* GW_CMD_CODEC_ONLY */
