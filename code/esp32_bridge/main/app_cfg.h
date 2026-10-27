/*
 * app_cfg.h - 阶段 B 的全部可调参数（改这一个文件就够了）
 * ===========================================================================
 * 阶段 B 要做的事：把 GPIO18 收到的帧，通过 WiFi + MQTT 发到 Ubuntu 上的
 * mosquitto，由服务端订阅入库。
 *
 * ⚠️ 下面标了「必须改」的三项，改完直接 Build + Flash 就行，别的地方不用动。
 * ===========================================================================
 */
#ifndef APP_CFG_H
#define APP_CFG_H

/* ---------------------------------------------------------------------------
 * 1) WiFi —— 必须改
 * ------------------------------------------------------------------------ */
/* 填你家路由器的 2.4GHz SSID。
 * ⚠️ ESP32-S3 **只支持 2.4GHz**，不认 5GHz 的 SSID（连着会一直重连失败）。
 * ⚠️ SSID 里有中文的话，这个宏要写成 UTF-8 字节串，容易出错，建议用英文 SSID。 */
#define APP_WIFI_SSID       "YOUR_WIFI_NAME"

#define APP_WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

/* ---------------------------------------------------------------------------
 * 2) MQTT Broker —— 必须改
 * ------------------------------------------------------------------------ */
/* Ubuntu 桥接后的局域网 IP（不是虚拟机的 192.168.236.x）。
 * 查看方法：在 Ubuntu 里执行 `ip addr` 或 `hostname -I`，找到 192.168.110.x 那个。
 * mosquitto 默认端口 1883，没开 TLS 就写 mqtt://，不要写 mqtts:// */
#define APP_MQTT_URI        "mqtt://192.168.110.100:1883"

/* 如果 mosquitto 开了用户名密码认证就填上；匿名访问留空字符串。
 * 阶段 B1 先用匿名跑通，B2 再加认证（见 README 的「加固」一节）。 */
#define APP_MQTT_USER       ""
#define APP_MQTT_PASS       ""

/* ---------------------------------------------------------------------------
 * 3) 主题 —— 一般不用改
 * ------------------------------------------------------------------------ */
/* 节点号。现在只接了一个终端节点（板子上是 n1），多节点时按 node 分主题 */
#define APP_NODE_ID         1

/* 数据主题：zigbee/node1/env */
#define APP_TOPIC_ENV       "zigbee/node%d/env"

/* 在线状态主题：zigbee/node1/status
 * 用 MQTT 的「遗嘱消息」（LWT）实现掉线检测：
 *   连上后 broker 记住 "online"，一旦设备异常断线（拔电、WiFi 掉），
 *   broker 会自动代发 "offline"。这是 IoT 项目的标准做法，
 *   比自己去轮询设备是否在线可靠得多。 */
#define APP_TOPIC_STATUS    "zigbee/node%d/status"

#define APP_STATUS_ONLINE   "online"
#define APP_STATUS_OFFLINE  "offline"

/* ---------------------------------------------------------------------------
 * 4) 运行时参数
 * ------------------------------------------------------------------------ */
#define APP_MQTT_QOS        1       /* QoS1 = 至少送达一次，允许重复不允许丢 */
#define APP_MQTT_KEEPALIVE  30      /* 秒。半开连接最多 1.5 倍时间被发现 */

/* 串口参数（和协调器一致，别改） */
#define APP_UART_PORT       1
#define APP_PIN_RX          18
#define APP_PIN_TX          17
#define APP_UART_BAUD       115200

/* 统计打印周期 */
#define APP_STAT_PERIOD_MS  5000

#endif /* APP_CFG_H */
