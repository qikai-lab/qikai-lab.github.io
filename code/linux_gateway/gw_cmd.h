/*
 * gw_cmd.h - 向 MQTT broker 发布一条下行命令（第 5 步 · 网页远程控制）
 * ---------------------------------------------------------------------------
 * 为什么自己拼 MQTT 报文，不用 libmosquitto
 * -----------------------------------------
 *   1) 这里只需要"连上去、发一条、断开"，是 MQTT 里最小的一个子集：
 *        CONNECT → CONNACK → PUBLISH → DISCONNECT
 *      QoS0、无遗愿、无订阅、无保活，协议开销一共 40 多个字节。
 *      引一个库反而要多装 libmosquitto-dev，还要处理它的回调线程模型。
 *   2) 这个网关是单线程 epoll 主循环，**不能阻塞**。
 *      自己写就能明确控制超时（默认 1.2 秒），并且失败直接返回错误码，
 *      不会出现"库在后台悄悄重连"这种看不见的行为。
 *   3) 拼包是纯函数（gw_cmd_build_*），可以离线单元测试 ——
 *      见 tests/test_cmd.c，不需要 broker 也能验证字节对不对。
 *   4) 求职项目里手写一遍 MQTT 报文，比调库更能说明问题。
 *
 * 不做的部分（都写清楚为什么）
 * ----------------------------
 *   · QoS1/2：命令是幂等的（继电器"开"连发 5 次结果一样），
 *     ZigBee 侧协调器本身就有 5 次重发，这里再上 QoS1 只会拖长网页响应。
 *   · 保活 / 长连接：每次按钮点一下连一次，发完就断。
 *     代价是一次 TCP 三次握手（本机 <1ms），好处是**永远不会有半死连接**。
 *   · 用户名密码：mosquitto 本机监听 127.0.0.1，允许匿名。
 * ---------------------------------------------------------------------------
 */
#ifndef GW_CMD_H
#define GW_CMD_H

#include <stddef.h>

/* 默认值。改不动的话可以直接改这几行，或者用环境变量覆盖：
 *   GW_MQTT_HOST / GW_MQTT_PORT / GW_CMD_TOPIC
 * 加环境变量的原因是：以后换 broker 地址（甚至换 mDNS 名字）不用重新编译。 */
#define GW_CMD_DEFAULT_HOST        "127.0.0.1"
#define GW_CMD_DEFAULT_PORT        1883
#define GW_CMD_DEFAULT_TOPIC       "zigbee/node1/cmd"
#define GW_CMD_DEFAULT_TIMEOUT_MS  1200

/* 返回码（都是负数，0 才是成功；网页上会翻译成人话） */
#define GW_CMD_OK            0
#define GW_CMD_ERR_ADDR     (-1)   /* 域名解析失败（host 写错）        */
#define GW_CMD_ERR_CONNECT  (-2)   /* 连不上（broker 没起 / 端口不对）  */
#define GW_CMD_ERR_SEND     (-3)   /* 发不出去（对端提前关了）          */
#define GW_CMD_ERR_REFUSED  (-4)   /* broker 拒绝连接（输错协议/鉴权）  */
#define GW_CMD_ERR_ARG      (-5)   /* 参数非法（topic 空 / 太长）       */
#define GW_CMD_ERR_PLATFORM (-6)   /* Windows 编译版：没有 POSIX socket */

/**
 * @brief 拼一个 MQTT CONNECT 报文（协议 3.1.1，clean session，无账号）
 * @param buf       输出缓冲
 * @param cap       缓冲容量
 * @param client_id 客户端 ID（同一 broker 上重复会踢掉前一个，建议带随机后缀）
 * @return 写入字节数；缓冲不够返回 0
 */
size_t gw_cmd_build_connect(unsigned char *buf, size_t cap, const char *client_id);

/**
 * @brief 拼一个 MQTT PUBLISH 报文（QoS0，不需要 packet id）
 * @return 写入字节数；参数非法或缓冲不够返回 0
 */
size_t gw_cmd_build_publish(unsigned char *buf, size_t cap,
                            const char *topic, const char *payload);

/**
 * @brief 把 payload 发到 broker 的指定 topic，发完就断开
 * @param timeout_ms 连接 + 等 CONNACK 的总时间上限
 * @return GW_CMD_OK 或 GW_CMD_ERR_*
 */
int gw_cmd_publish(const char *host, int port, const char *topic,
                   const char *payload, int timeout_ms);

/**
 * @brief 用默认配置（或环境变量覆盖）发一条命令，给 HTTP 接口用
 * @note  topic/port/host 优先取环境变量 GW_CMD_TOPIC / GW_MQTT_PORT / GW_MQTT_HOST
 */
int gw_cmd_publish_default(const char *payload);

/** @brief 给网页显示用：把返回码翻译成一句中文原因 */
const char *gw_cmd_strerror(int rc);

#endif /* GW_CMD_H */
