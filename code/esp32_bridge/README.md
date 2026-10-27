# esp32_bridge — ZigBee 协调器 → WiFi/MQTT 网关固件

项目第 4 步的全部代码：把 CC2530 协调器 UART 上那串 24 字节定长帧，
解析后通过 WiFi 发布到 MQTT broker。

对应手记：<https://qikai-lab.github.io/posts/week5-esp32-bridge.html>

```
CC2530 协调器 P0_3 ──UART 115200 8-N-1──> ESP32-S3 GPIO18 (UART1)
                                                │
                                        zigbee_frame.c  24 字节定长帧解析
                                                │
                                          mqtt_pub.c     JSON 组包 + publish
                                                │
                                          WiFi ──> mqtt://<broker>:1883
```

服务端订阅程序：`../linux_gateway/mqtt_sub.c`（落到 `env_data_mqtt` 表）。

## 文件

| 文件 | 说明 |
| --- | --- |
| `main/app_cfg.h` | **唯一需要改的文件**：WiFi SSID / 密码、broker 地址、节点号、主题 |
| `main/main.c` | UART1 初始化、收帧任务、5 秒统计行 |
| `main/zigbee_frame.c` | 24 字节定长帧的滑动窗口解析器（纯函数，可离线测） |
| `main/wifi_mgr.c` | WiFi 状态机：NVS 初始化、断开延后 2 秒重连、关省电 |
| `main/mqtt_pub.c` | MQTT 客户端：遗嘱消息（LWT）、JSON 组包、发布计数 |
| `sdkconfig.defaults` | 首次构建采用的默认配置，含 `no_reset` 烧录设置 |
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
```

每 5 秒精确 `+120 字节 / +5 帧`，坏帧与无效字节全程为 0。
`坏帧`（帧头对上但字段越界）与 `无效字节`（没对上帧头被丢掉）**分开计数**，
两者都稳定为 0 才说明线路干净、解析器没有在重同步。

## 已知限制

1. **app 分区只剩 13%**：`esp32_bridge.bin` 约 914 KB，factory 分区 1 MB。
   要加 TLS 得先调大分区表。
2. **QoS 1 会重复**：帧里没有序号字段，服务端只能统计不能严格去重。
   要做去重得改 CC2530 侧的协议。
3. **断线期间数据直接丢**（只计数），时间戳是服务端到达时刻。
   补发会让服务端时间戳全部挤在一起，制造假台阶，比缺失更有害。
4. **反方向未接通**：`GPIO17 → P0_2` 是第 5 步（远控继电器）才需要的，
   而且会撞上 CH340 TXD 与 ESP32 TX 两个输出顶在同一网络的问题。
