# esp32_bridge — ZigBee 协调器 → WiFi/MQTT 网关固件

项目第 4、5 步的全部代码：把 CC2530 协调器 UART 上那串 24 字节定长帧，
解析后通过 WiFi 发布到 MQTT broker；反向再把 MQTT 上的命令字，经同一个 UART
送回协调器，由它单播下发给终端节点控制继电器。

对应手记：<https://qikai-lab.github.io/posts/week5-esp32-bridge.html> ·
<https://qikai-lab.github.io/posts/week6-relay-remote.html>

```
                   上行（数据）
CC2530 协调器 P0_3 ──UART 115200 8N1──> ESP32-S3 GPIO18 (UART1 RX)
                                                │
                                        zigbee_frame.c  24 字节定长帧解析
                                                │
                                          mqtt_pub.c     JSON 组包 + publish
                                                │
                                          WiFi ──> mqtt://<broker>:1883

                   下行（命令）
   mqtt_pub.c 收到 zigbee/node1/cmd  ──> bridge_send_cmd(0x40 / 0x41)
                                                │
ESP32-S3 GPIO17 (UART1 TX) ──────────────> 协调器 P0_5 (UART1 RX，备用位置 1)
                                                │
                                      ZigBee 单播 + AF_ACK_REQUEST
                                                │
                                          终端节点 P1_3 继电器
```

服务端订阅程序：`../linux_gateway/mqtt_sub.c`（落到 `env_data_mqtt` 表）。

## 文件

| 文件 | 说明 |
| --- | --- |
| `main/app_cfg.h` | **唯一需要改的文件**：WiFi SSID / 密码、broker 地址、节点号、主题、OLED 引脚 |
| `main/main.c` | UART1 收发初始化、收帧任务、`bridge_send_cmd()`、5 秒统计行 |
| `main/zigbee_frame.c` | 24 字节定长帧的滑动窗口解析器（纯函数，可离线测） |
| `main/wifi_mgr.c` | WiFi 状态机：NVS 初始化、断开延后 2 秒重连、关省电 |
| `main/mqtt_pub.c` | MQTT 客户端：遗嘱消息（LWT）、订阅命令主题、JSON 组包、发布计数 |
| `main/oled_status.c` | 0.91" SSD1306 状态屏驱动（自写 I2C + 6×8 字库），不参与数据链路 |
| `partitions.csv` | 自定义分区表，factory 从 1 MB 扩到 1.5 MB |
| `sdkconfig.defaults` | 首次构建采用的默认配置，含 `no_reset` 烧录设置与 mDNS 查询开关 |
| `tests/test_frame.c` | 帧解析器的宿主机离线测试（不接板子就能跑） |
| `tests/*.py` | 三个串口探针脚本，见下文「排障」 |

## 编译与烧录

需要 ESP-IDF v5.5（EIM 安装）。

```bash
# VSCode：装 ESP-IDF 插件，打开本目录 → Build → Flash → Monitor
# 纯命令行（PowerShell，不要用 Git Bash）：
idf.py build
idf.py -p COM10 flash monitor
```

`COM10` 换成你机器上的实际端口。

### 两个必须知道的构建环境事实

**1. 不能在 Git Bash 里跑 ESP-IDF。**

- 跑 `export.bat` 会报 `This .bat file is for Windows CMD.EXE shell only`
- 直接跑 `idf.py` 会报 `MSys/Mingw is no longer supported`（v5 是硬退出，不是警告）

用 PowerShell，或者直接用 VSCode 插件。

**2. EIM 安装器不生成 `tools.json`，工具链版本目录带 `esp-` 前缀。**

`idf.py` 靠扫描 `$IDF_TOOLS_PATH/tools/<name>/<version>` 自动发现工具链，
而 EIM 实际放在 `xtensa-esp-elf/esp-14.2.0_20260121/...`，多了一层前缀，于是报

```
CMAKE_C_COMPILER xtensa-esp32s3-elf-gcc is not a full path and was not found in the PATH
```

修法：把 `...\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin` 手工加进 `PATH`。
完整清单看 `Microsoft.v5.5.6.PowerShell_profile.ps1`——里面把整套 PATH 都写好了。

另外，如果报 `ERROR: espidf.constraints.v5.5.txt doesn't exist`，
是 `idf_tools.py` 去 `$IDF_TOOLS_PATH` 根目录找而 EIM 放在 `tools/` 下的原因，
复制一份上去即可。

## 烧录：这块板子必须手动进下载模式

`sdkconfig.defaults` 里已经设好：

```ini
CONFIG_ESPTOOLPY_BEFORE_NORESET=y
# 等效于命令行 --before no_reset
```

因为这块开发板的 CH343 拉不动 `EN` / `IO0`（自动下载电路不通），
只能**按住 BOOT → 点一下 RST → 松开 BOOT** 进下载模式。

而 esptool 默认的 `--before default_reset` 会在**连接之前**复位一次板子——
那一下正好把手动进好的下载模式踢出去，于是永远报：

```
A fatal error occurred: Failed to connect to ESP32-S3: No serial data received.
```

改成 `no_reset` 后 esptool 完全不碰复位线，保持手动进入的状态。

## 排障：三个串口探针脚本

`No serial data received` 有三种完全不同的成因，只看报错分不出来。
这三个脚本按「从粗到细」的顺序用，几十秒就能排除掉大部分：

```bash
# 1) 板子到底说不说话了
python tests/peek_port.py COM10

# 2) 是不是波特率不对
python tests/scan_port.py COM10

# 3) 芯片还活着吗：试 6 种 DTR/RTS 组合，再直发 ROM 的 SYNC 握手包
python tests/probe_bootloader.py COM10
```

判读：

| 结果 | 含义 |
| --- | --- |
| 五档波特率全 0 字节，SYNC 包也无回应 | **主芯片没在跑**（先查供电） |
| 有输出但全是乱码 | 波特率不对，试 74880（ROM 默认） |
| 能看到 `rst:0x1 (POWERON)` | 供电和串口都通，问题在别处 |

**注意：设备管理器里能看到 COM 口，只证明 USB 转串口芯片活着，不能证明主芯片活着。**
这块板子上 CH343 由 USB 5V 直供，ESP32-S3 主芯片走另一路 3.3V。
曾经在这上面浪费了很久：端口在、驱动正常、设备管理器干干净净，但芯片是块石头。

## 阶段 A 验收数据

只透传、不连网的版本（去掉 `mqtt_pub` / `wifi_mgr` 的调用）实测：

```
BRIDGE: ---- 5s:  字节=120  帧=5  坏帧=0 无效字节=0 ----
BRIDGE: ---- 15s: 字节=360  帧=15 坏帧=0 无效字节=0 ----
...
BRIDGE: ---- 50s: 字节=1200 帧=50 坏帧=0 无效字节=0 ----
BRIDGE: ---- 60s: 字节=1440 帧=60 坏帧=0 无效字节=0 ----
```

每 5 秒精确 `+120 字节 / +5 帧`，坏帧与无效字节全程为 0（60 秒 = 1440 字节 / 60 帧）。
`坏帧`（帧头对上但字段越界）与 `无效字节`（没对上帧头被丢掉）**分开计数**，
两者都稳定为 0 才说明线路干净、解析器没有在重同步。

## 阶段 B 验收数据

上 WiFi + MQTT 之后实跑（串口统计行，第二次烧录后的版本）：

```
BRIDGE: ---- 5s: 字节=4691 帧=205 坏帧=0 无效字节=11 | WiFi=通(10.236.232.204,-28dBm,断0次) MQTT=通 发布=205 丢弃=0  ----
BRIDGE: ---- 5s: 字节=5051 帧=210 坏帧=0 无效字节=11 | WiFi=通(10.236.232.204,-27dBm,断0次) MQTT=通 发布=210 丢弃=0  ----
```

服务端订阅器同时给出 `收到=198 入库=198 解析失败=0 疑似重复=8`。

**账目是能互相对上的**：`210 帧 × 24 字节 = 5040`，加上 11 个无效字节 = **5051**，
和日志里 `字节=5051` 完全一致。那 11 个无效字节是上电瞬间收到的半个帧，属一次性损耗。

四个数字容易被误判成故障，实际都自洽：

| 现象 | 真实原因 |
|---|---|
| `210` 帧 vs `198` 收到，差 12 | `mqtt_sub` 比 ESP32 晚启动 12 秒，前 12 帧它还没开始监听。判断有没有丢包只看「收到 = 入库」 |
| `疑似重复=8` | 协调器自己连吐两帧（同一毫秒、同值），不是 QoS 1 重发 |
| `无效字节=11` | 上电瞬间协调器正在吐帧，取到了半个帧。`坏帧=0` 才是线路质量指标 |
| `坏帧` 偶发 2~3 | 协议没有 CRC，24 字节里任何一个被打歪就得整帧丢弃，是当初省 CRC 的设计代价 |

> `丢弃=0` 是「等 WiFi 再启 MQTT」这个改动最直接的成果。
> 改之前同一套逻辑跑出来是 `丢弃=13`：MQTT 从上电到连上花了 10718 ms，
> 这期间协调器已吐 13 帧（`10718 ÷ 800ms/帧 ≈ 13`），未连接时只计数不排队，于是全丢。

## 启动顺序：先等 WiFi 拿到 IP，再启 MQTT

`app_main()` 里不是「起完 WiFi 直接起 MQTT」，中间有一个等待：

```c
if (wifi_mgr_start() != 0) { ESP_LOGE(...); }   /* 失败也不退出，采集继续 */
start_mqtt_after_wifi();                        /* 最多等 APP_WIFI_WAIT_MS */
```

**不等会怎样**：MQTT 客户端一 init 就立刻连 broker，此时 IP 栈还没就绪，
必然刷一条 `E esp-tls: Host is unreachable`，5 秒后它自己又连上了。
它对功能无害，但**红字会把真正该看的错误埋掉**——排障时最怕报错里一半是噪音。

超时（`APP_WIFI_WAIT_MS`，默认 15 秒）也照样启动 MQTT，让它自己一直重试，
并且明确打一条 warn 说明「接下来若报 TCP/DNS 错误，是 WiFi 没通的连带结果」，
免得拿着网络层的错误去查 broker。

## 下行：一条命令从网页走到继电器

这是第 5 步「网页远控」里 ESP32 负责的那一段。它**不理解命令含义**，
只做字节搬运：MQTT 收到 `"1"` → 发 `0x41`；收到 `"0"` → 发 `0x40`。

```c
/* main.c —— 只写一个字节，然后等 TX 发完 */
void bridge_send_cmd(uint8_t cmd)
{
    uart_write_bytes(APP_UART_PORT, (const char *)&cmd, 1);
    (void)uart_wait_tx_done(APP_UART_PORT, pdMS_TO_TICKS(50));
}
```

命令字沿用 CC2530 工程 `GenericApp.h` 里早就定好的位段，不是新发明的：

```
bit7..6 = 节点号   bit5..4 = 设备类型   bit3..0 = 命令
0x41 = 节点1 · 继电器 · 吸合
0x40 = 节点1 · 继电器 · 断开
```

三个容易踩的点：

| 点 | 为什么 |
| --- | --- |
| 订阅写在 `MQTT_EVENT_CONNECTED` 里，不是启动时 | 重连后 broker 上的会话没了，只有在这里补订阅才不会「断一次网就再也收不到命令」 |
| 订阅用 QoS 1 | 命令是幂等的（开就是开），宁可重复收，不能漏 |
| `ev->data` 不以 `'\0'` 结尾 | 必须用 `data_len` 判断长度，当字符串用会越界读到脏数据 |

**UART1 是双向的**：RX 走 GPIO18（收协调器 P0_3），TX 走 GPIO17（发协调器 **P0_5**）。

这根线有个反直觉的点，值得单独记一笔：

| | UART0 | UART1（备用位置 1） |
| --- | --- | --- |
| RX | P0_2（低脚） | **P0_5（高脚）** |
| TX | P0_3（低脚） | P0_4（低脚） |

UART0 是「低脚收」，照这个直觉往 UART1 上套，就会把 ESP32 的 TX 接到 P0_4 ——
而 P0_4 是协调器自己的 TX 脚，等于两个输出顶在一个节点上，一个字节都收不到。
**ESP32 的 TX 必须接 P0_5。**

也不是 P0_2：P0_2 是 UART0 的 RX，和板载 CH340 的 TXD 顶在同一个网络里。

## OLED 状态屏（可选，拔掉不影响任何功能）

0.91" SSD1306，128×32，I2C，接 `GPIO4 (SDA)` / `GPIO5 (SCL)`。

它和终端节点板上那块 1.44" 彩屏分工不同：

- **终端彩屏** → 传感器的值：温度、湿度、光照、烟雾、有人、继电器
- **这块 OLED** → 网关自己的值：WiFi 有没有、IP 多少、MQTT 通不通、发了几条、丢了几条

四行 16 字符，长这样：

```
10.236.232.204
-29dB M:1 t205 d0
18.3C 55% L123 G45 P0
#205 bad0 RLY-OFF
```

设计原则是「**屏是旁路，不是链路**」：初始化失败只打一行警告、刷新失败只累加计数、
所有 I2C 写带超时（最坏 20 ms）。所以没接屏、线松了、模块是 0x3D 而不是 0x3C，
都不会影响收帧和上云——这一点是刻意的，不是为了省事。

## 供电：烧录才需要 USB，运行不需要

这块板子上有两个东西在用电，但走的是两路：

| 场景 | USB 线 | 说明 |
| --- | --- | --- |
| 烧录 / 看串口日志 | **必须插** | `idf.py -p COM10 flash monitor`，同时供电、同时出日志 |
| 只跑业务 | **可以不插** | 从 `5V` / `3V3` 排针喂电即可；WiFi + MQTT 照常工作 |

数据**不是**从 USB 进来的。ESP32 只从 GPIO18 那一根线读协调器的 UART 信号，
USB 那一路是 CH343 转出来的**另一条**串口（接电脑用的），两条互不相干。
所以「插个电就能传」是成立的，前提是：

1. 协调器自己有电（它靠 USB 或排针供电，拔了就没信号）
2. `P0_3 → GPIO18` 这根线在
3. **GND 必须共地** —— 只接信号不接地，是最常见的「一个字节都没有」

反过来，**如果你把 USB 插在电脑上，而电脑上又开着串口助手盯着 COM10**，
那你会看到日志但不会影响数据——COM10 是 CH343 那条，不是 GPIO18 那条。

## 已知限制

1. **加 OLED 后固件 916 KB，默认 1 MB 分区只剩 107 KB**。
   所以 `partitions.csv` 把 factory 从 1 MB 扩到 1.5 MB（起始偏移仍是 `0x10000`，不动 NVS）。
   芯片实际 16 MB，扩完还剩 14.4 MB 闲置。
2. **QoS 1 会重复**：帧里没有序号字段，服务端只能统计不能严格去重。
   要做去重得改 CC2530 侧的协议。
3. **断线期间数据直接丢**（只计数），时间戳是服务端到达时刻。
   补发会让服务端时间戳全部挤在一起，制造假台阶，比缺失更有害。
4. **mDNS 依赖三条同时成立**：ESP-IDF 开了 `CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES`、
   Ubuntu 上 avahi-daemon 在跑且主机名是 `ubuntu`、虚拟机网络是**桥接**（NAT 会拦组播）。
   哪条不满足就退回写死 IP，改 `APP_MQTT_URI` 一行重烧，其它代码不用动。
5. **没有 CRC，约 3% 的帧会被整帧丢掉**。这是协议设计代价，不是 bug。
6. **不做的**：TLS、掉线续传、低功耗休眠。前两项会让「时间戳是否可信」变得难以自证，
   第三项本地没有条件做 24 小时以上续航实测。
