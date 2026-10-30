# 源码

这里只放**完全由我本人从头编写**的那几层：Linux 网关服务端（第 3~6 步）
与 ESP32 网关固件（第 4、5 步）。

- `linux_gateway/` — 串口 → 解析 → SQLite → 网页 → 告警 → 历史曲线，纯 C，无第三方框架
- `esp32_bridge/` — 协调器 UART ⇄ WiFi/MQTT，ESP-IDF v5.5

## 为什么只有这一层

整个项目分四层，其中：

| 层 | 代码来源 | 是否放在这里 |
| --- | --- | --- |
| CC2530 终端节点 | 外设驱动（Timer1 / ADC / DHT11 单总线 / ST7735 彩屏 / 按键状态机）为自己编写，但工程骨架与射频部分基于课程提供的 Z-Stack 示例工程 | ❌ 不整包公开 |
| CC2530 协调器 | 同上，应用层改为透传 24 字节定长帧 | ❌ 不整包公开 |
| **Linux 网关** | **100% 自写，无第三方框架** | ✅ **在这里** |
| **ESP32 上云固件** | **100% 自写，基于 ESP-IDF** | ✅ **在这里** |

CC2530 那一层的工程骨架和 Z-Stack 协议栈示例来自课程材料，整包公开涉及他人版权，
所以不放进仓库。节点板外设驱动的关键实现（引脚分配、时序处理、踩过的坑）
写在了手记的对应章节里：`posts/week1-sensors.html`、`posts/pinout-reference.html`。

## 这一层做了什么

一个**单进程单线程**的 C 服务，把串口上的字节流变成网页上会动的曲线：

```
串口 115200 8-N-1
   │  termios + 非阻塞读
   ▼
帧解析（24 字节定长，无 CRC，靠字段范围校验）
   │
   ├──→ 中值滤波 → 自适应基线 → 告警状态机 ──→ 告警事件表
   │
   ├──→ 内存快照（实时数据，网页 /api/latest 直接读这里）
   │
   └──→ SQLite（WAL + 20 条一批批量提交，历史查询走这里）
                │
                ▼
        手写 socket HTTP 服务（无 libevent / 无框架）
        网页 HTML 用 xxd 编进二进制，单文件部署
```

关键点全部有注释，几个值得先看的：

- `main.c` — epoll 统一事件循环、`signalfd` 把信号转成 fd、串口每秒重连
- `gw_frame.c` — 24 字节定长帧的状态机解析，含半帧、垃圾前缀、假帧头处理
- `gw_db.c` — WAL 模式 + 批量事务 + 预处理语句，以及「实时数据为什么不查库」；
  `gw_db_trend()` 做 SQL 时间桶聚合（1h→30s 桶、24h→900s 桶，点数固定）
- `gw_filter.c` / `gw_alarm.c` — 中值滤波、自适应基线、迟滞状态机
- `http_srv.c` — 从零实现的 HTTP/1.1 响应构造，含 `Content-Length` 一致性、
  `/api/trend` 的范围解析与分桶
- `gw_cmd.c` — 手写 MQTT 报文（不依赖 libmosquitto），把网页按钮变成一条 `publish`

## 编译与运行

```bash
# 依赖：gcc + libsqlite3-dev
sudo apt install build-essential libsqlite3-dev

make                # 出 gw（+ mqtt_sub）可执行文件
make test           # 230 项离线断言，不需要任何硬件
sudo ./gw -d /dev/ttyUSB0 -p 8080
```

**没有硬件也能跑**：

```bash
python3 tools/sim_node.py --socat     # 用 socat 造一对虚拟串口
./gw -d /dev/ttyS10 -p 8080
python3 tools/sim_node.py --smoke     # 另一个窗口：模拟烟雾浓度爬升，看告警触发
```

## 部署

```bash
sudo bash install_service.sh /dev/ttyUSB0 8080
```

装成 systemd 服务：开机自启、崩溃 3 秒后自动拉起、日志进 journald。
`install_service.sh` 会把当前 sudo 调用者作为服务运行用户，避免 `gw.db` 变成 root 所有。

## 测试

| 文件 | 覆盖 | 项数 |
| --- | --- | --- |
| `tests/test_parse.c` | 帧解析 + 落库，含逐字节喂入、断帧、假帧头、真实串口抓包 | 50 |
| `tests/test_recent.c` | 历史查询分页、节点过滤、告警表 begin/end、上电清理未收尾告警 | 31 |
| `tests/test_trend.c` | 时间桶聚合：分桶边界、四舍五入、断档不补 0、峰值保留、节点过滤 | 32 |
| `tests/test_filter.c` | 中值滤波、自适应基线收敛、告警迟滞与死区 | 36 |
| `tests/test_http.c` | 各接口响应体与 `Content-Length` 一致性、404、空库 | 38 |
| `tests/test_chain.c` | 串口字节 → 解析 → 滤波 → 告警 → 落库 的端到端串联 | 23 |
| `tests/test_cmd.c` | MQTT CONNECT / PUBLISH 报文逐字节断言（不需要 broker） | 20 |
| **合计** | 不需要串口、不需要板子、不需要 broker | **230** |

`test_parse` 里那 208 字节真实串口抓包是最有价值的用例：
它同时包含 5 个完整帧和 8 个残缺帧，能验证解析器在真实丢字节的情况下不会乱。

---

对应手记：<https://qikai-lab.github.io/posts/week3-linux-gateway.html>
