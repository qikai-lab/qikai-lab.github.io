/*
 * test_cmd.c - gw_cmd 拼包层单元测试（第 5 步 · 网页远程控制）
 * ---------------------------------------------------------------------------
 * 为什么不测网络：
 *   网络部分需要真的 broker 和 socket，属于"到了 Ubuntu 上才验得了"的东西。
 *   但**报文拼错**是最容易犯、也最难在现场看出来的错 ——
 *   字节多一个、剩余长度算错一个数，broker 就直接断连，
 *   而现场只能看到"网页说发出去了，继电器不动"，根本没法定位。
 *
 *   所以这里把拼包函数单独拎出来做字节级断言：
 *   只要标准字节串对得上，剩下的就只是 socket 通不通。
 *
 * 编译：make tests/test_cmd && ./tests/test_cmd
 * 不需要 broker、不需要 root、Windows / Linux 都能跑。
 * ---------------------------------------------------------------------------
 */
#define GW_CMD_CODEC_ONLY       /* 让 gw_cmd.c 只留拼包，不含 socket 代码 */

#include <stdio.h>
#include <string.h>

#include "gw_cmd.h"
#include "gw_cmd.c"             /* 白盒测试：直接编进来才能访问 static 的 put_remlen */

static int g_pass = 0;
static int g_fail = 0;

static void check(int cond, const char *what)
{
    if (cond) {
        g_pass++;
        printf("  ok    %s\n", what);
    } else {
        g_fail++;
        printf("  FAIL  %s\n", what);
    }
}

static void dump(const char *tag, const unsigned char *p, size_t n)
{
    size_t i;

    printf("        %s (%u 字节): ", tag, (unsigned)n);
    for (i = 0; i < n; i++) {
        printf("%02X ", p[i]);
    }
    printf("\n");
}

/* 逐字节比对，失败时把两边的十六进制都打出来 —— 只报"不相等"是没法查的 */
static int bytes_eq(const unsigned char *got, size_t n,
                    const unsigned char *want, size_t wn)
{
    if (n != wn) {
        dump("实际", got, n);
        dump("期望", want, wn);
        return 0;
    }
    if (memcmp(got, want, n) != 0) {
        dump("实际", got, n);
        dump("期望", want, wn);
        return 0;
    }
    return 1;
}

int main(void)
{
    unsigned char buf[1600];
    size_t        n;

    printf("=== gw_cmd 拼包测试 ===\n\n");

    /* ------------------------------------------------------------------
     * 1. CONNECT：这是最严格的断言，一个字节都不能差
     * ------------------------------------------------------------------ */
    printf("[1] CONNECT 报文\n");
    {
        static const unsigned char want[] = {
            0x10, 0x0E,                     /* CONNECT，剩余长度 14 */
            0x00, 0x04, 'M', 'Q', 'T', 'T', /* 协议名 */
            0x04,                           /* 协议级别 3.1.1 */
            0x02,                           /* clean session */
            0x00, 0x3C,                     /* 保活 60 秒 */
            0x00, 0x02, 'g', 'w'            /* clientId 长度 + "gw" */
        };

        n = gw_cmd_build_connect(buf, sizeof(buf), "gw");
        check(n == sizeof(want), "长度 = 16");
        check(bytes_eq(buf, n, want, sizeof(want)), "字节完全一致");
        check(n == 16 && buf[0] == 0x10, "首字节是 CONNECT(0x10)");
        /* 剩余长度 = 报文总长 - 1(首字节) - 1(剩余长度字段本身) */
        check(n == 16 && buf[1] == (unsigned char)(n - 2), "剩余长度字段自洽");
    }

    /* ------------------------------------------------------------------
     * 2. PUBLISH：QoS0，注意首字节 0x30（低 4 位 0 = QoS0，不用 packet id）
     * ------------------------------------------------------------------ */
    printf("\n[2] PUBLISH 报文（topic=zigbee/node1/cmd, payload=1）\n");
    {
        static const unsigned char want[] = {
            0x30, 0x13,                                 /* PUBLISH QoS0，剩余长度 19 */
            0x00, 0x10,                                 /* 主题长度 16 */
            'z','i','g','b','e','e','/','n','o','d','e','1','/','c','m','d',
            '1'                                         /* payload */
        };

        n = gw_cmd_build_publish(buf, sizeof(buf), "zigbee/node1/cmd", "1");
        check(n == sizeof(want), "长度 = 21");
        check(bytes_eq(buf, n, want, sizeof(want)), "字节完全一致");
        check(n == 21 && (buf[0] & 0x0F) == 0x00, "低 4 位 = QoS0");
        check(n == 21 && buf[1] == (unsigned char)(n - 2), "剩余长度字段自洽");
    }

    /* ------------------------------------------------------------------
     * 3. payload 是 '0' 时只差最后一个字节 —— 保证没有把 '1' 写死
     * ------------------------------------------------------------------ */
    printf("\n[3] PUBLISH payload=0\n");
    {
        n = gw_cmd_build_publish(buf, sizeof(buf), "zigbee/node1/cmd", "0");
        check(n == 21, "长度仍是 21");
        check(n == 21 && buf[20] == '0', "末字节是 '0'");
    }

    /* ------------------------------------------------------------------
     * 4. 剩余长度跨过 127，必须变成两字节变长整数
     *    rem = 2 + 130 + 1 = 133 = 0x85 0x01
     *    算法：低 7 位 133 & 0x7F = 5，置高位 → 0x85；再右移 7 位 = 1 → 0x01
     * ------------------------------------------------------------------ */
    printf("\n[4] 长主题（剩余长度 > 127，两字节变长整数）\n");
    {
        char topic[131];

        memset(topic, 'a', 130);
        topic[130] = '\0';

        n = gw_cmd_build_publish(buf, sizeof(buf), topic, "1");
        check(n == 1 + 2 + 133, "总长 = 1 + 2 + 133 = 136");
        check(n >= 4 && buf[1] == 0x85 && buf[2] == 0x01, "剩余长度编码为 85 01");
    }

    /* ------------------------------------------------------------------
     * 5. 各种非法输入都要挡住，且**不能写越界**
     * ------------------------------------------------------------------ */
    printf("\n[5] 参数防御\n");
    check(gw_cmd_build_publish(buf, 8, "zigbee/node1/cmd", "1") == 0,
          "缓冲太小 → 返回 0（不越界写）");
    check(gw_cmd_build_publish(buf, sizeof(buf), "", "1") == 0,
          "空 topic → 返回 0");
    check(gw_cmd_build_publish(buf, sizeof(buf), NULL, "1") == 0,
          "topic=NULL → 返回 0");
    check(gw_cmd_build_publish(buf, sizeof(buf), "t", NULL) == 0,
          "payload=NULL → 返回 0");
    check(gw_cmd_build_connect(buf, 4, "gw") == 0,
          "CONNECT 缓冲不足 → 返回 0");

    /* ------------------------------------------------------------------
     * 6. clientId 超过 23 字节要被截断（3.1.1 的短 ID 限制）
     * ------------------------------------------------------------------ */
    printf("\n[6] clientId 截断\n");
    {
        n = gw_cmd_build_connect(buf, sizeof(buf), "0123456789012345678901234567890");
        /* 变长头 10 + 长度字段 2 + 截断后的 23 */
        check(n == 1 + 1 + 10 + 2 + 23, "截断到 23 字节，总长 = 37");
    }

    /* ------------------------------------------------------------------
     * 7. 实际生产用的 clientId 也过一遍（gw_cmd_publish 里写的是 "zigbee-gw"）
     * ------------------------------------------------------------------ */
    printf("\n[7] 生产 clientId \"zigbee-gw\"\n");
    {
        n = gw_cmd_build_connect(buf, sizeof(buf), "zigbee-gw");
        check(n == 1 + 1 + 10 + 2 + 9, "总长 = 23");
        check(n == 23 && buf[1] == 21, "剩余长度 = 21 (0x15)");
    }

    printf("\n----------------------------------------\n");
    printf("通过 %d 项，失败 %d 项\n", g_pass, g_fail);

    return (g_fail == 0) ? 0 : 1;
}
