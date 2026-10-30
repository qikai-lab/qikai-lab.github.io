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
/* 🟢 2026-10-28 起改用 mDNS 域名，**不再写 IP**。
 *
 * 原理：Ubuntu 上的 avahi-daemon 会一直对外广播「我叫 ubuntu」，ESP32 连上 WiFi
 * 后用 mDNS（组播到 224.0.0.251:5353）问一句「ubuntu.local 是谁」，avahi 直接回 IP。
 * 所以不管手机热点把网段换成 10.233.60.x 还是 10.236.232.x，这句话都成立，
 * **换热点不用再改这里、不用再重烧**。
 *
 * 依赖（三条，缺一条就解析不出来）：
 *   ① ESP-IDF 侧 CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES=y —— 本工程已开
 *   ② Ubuntu 侧 avahi-daemon 在跑，且 hostname 是 ubuntu
 *      （查：systemctl status avahi-daemon | head -3 ；ping -c2 ubuntu.local）
 *   ③ 虚拟机网络是**桥接**，不是 NAT —— NAT 会拦组播，mDNS 必失败
 *
 * 🔴 万一是 .local 解析不出来（VMware 桥接偶尔拦组播），把这一行改回
 *    "mqtt://<Ubuntu 当前 IP>:1883" 重新烧一次即可，其它代码一行都不用动。
 *
 * 历史记录（已作废，别再填）：
 *    - 10.236.232.172  10-28 那次热点会话的地址（首个跑通的 IP，已由域名取代）
 *    - 10.233.60.172   10-27 那次热点会话的地址，热点重启后整段作废
 *    - 192.168.110.115 403（5GHz）网段的旧地址，ESP32 连不上 5GHz
 *    - 192.168.110.1   路由器自带的另一个 mosquitto 1.5.9，数据会被它吞掉 */
#define APP_MQTT_URI        "mqtt://ubuntu.local:1883"

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

/* 命令主题：zigbee/node1/cmd
 *
 * payload 约定（只发一个字符，别加 JSON，省得两边解析口径不一致）：
 *   "1" → 继电器吸合
 *   "0" → 继电器断开
 *
 * ESP32 收到后翻译成 CC2530 的命令字再走 UART1 下发：
 *   字节结构 = 节点号(高2位) | 设备类型(中间2位) | 命令(低4位)
 *   0x41 = 节点1 · 继电器 · 吸合
 *   0x40 = 节点1 · 继电器 · 断开
 * 这个编码是 CC2530 工程里 GenericApp.h 早就定义好的
 * （STO_MASK 0xc0 / DEV_MASK 0x30 / CMD_MASK 0x0f），不是新发明的。 */
#define APP_TOPIC_CMD       "zigbee/node%d/cmd"

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

/* ---------------------------------------------------------------------------
 * 5) 0.91" OLED 状态屏（SSD1306，128×32，I2C）
 * ------------------------------------------------------------------------ */
/* 屏自己不用网、不参与数据链路，只把「网关现在什么状态」显示出来。
 * 拔掉屏（或线没接好）**不影响任何功能**，代码里 oled_status_init() 失败
 * 只打一行警告就继续跑，不会让程序退出。
 *
 * 接线（那块蓝板丝印从上到下写的就是这个顺序，别按别人的图乱接）：
 *   GND  → ESP32 GND
 *   VCC  → ESP32 3V3      ⚠️ 接反会烧屏，接之前对两遍
 *   SCL  → APP_PIN_OLED_SCL
 *   SDA  → APP_PIN_OLED_SDA
 *
 * 为什么选 GPIO4/GPIO5：本工程只占了 GPIO17/18（UART1），这两个是空的；
 * 而且都不是 strapping 脚（0/3/45/46）、不是原生 USB（19/20）、不是 UART0 控制台（43/44）。
 * 万一块子上这两个脚被别的东西占了，改下面两行即可，代码别的地方不用动。 */
#define APP_PIN_OLED_SDA    4
#define APP_PIN_OLED_SCL    5
#define APP_OLED_I2C_HZ     400000      /* SSD1306 支持 400kHz，写满 512 字节约 13ms */
#define APP_OLED_ADDR       0x3C        /* 四针模块几乎都是 0x3C；0x3D 少见 */

/* 刷新节流：数据每秒来一帧，但 I2C 写太勤没意义，500ms 一刷足够顺眼 */
#define APP_OLED_REFRESH_MS 500

/* 统计打印周期 */
#define APP_STAT_PERIOD_MS  5000

/* 启动时等 WiFi 拿到 IP 的最长时间（毫秒）。
 * 为什么要等：MQTT 客户端一起来就会立刻去连 broker，此时 IP 栈还没就绪，
 * DNS/connect 必然失败，日志里会刷一条红色的 "Host is unreachable"。
 * 它不是故障（5 秒后自己就重连好了），但红字会把真正的问题埋掉，
 * 所以先等 WiFi。超时也照样启动 MQTT（让它自己一直重试），不阻塞采集。 */
#define APP_WIFI_WAIT_MS    15000

#endif /* APP_CFG_H */
