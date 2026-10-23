# ZigBee 无线环境监测系统

一个从裸机驱动一路写到 Linux 服务端的完整 IoT 项目，目标是把「传感器采集 → 无线组网 → 云端汇聚 → 数据落库 → 网页可视 + 远程控制」整条链路真正跑通，而不是停留在演示板点灯。

**在线开发手记：** https://qikai-lab.github.io/

---

## 系统架构

![系统架构](assets/img/architecture.svg)

四层结构，每层职责单一、接口清晰：

| 层级 | 组成 | 职责 |
| --- | --- | --- |
| 感知层 | CC2530 + DHT11 / MQ-2 / 光敏 / 人体红外 | 定时采集环境数据，本地彩屏显示，电池供电 |
| 汇聚层 | CC2530 协调器 | 建立 ZigBee 网络、管理节点入网、串口输出结构化帧 |
| 网关层 | ESP32-S3 | 串口收帧 → 解析 → Wi-Fi + MQTT 上云，支持 BLE 配网 |
| 应用层 | Ubuntu + C 服务端 | 数据落库、告警判定、HTTP 接口、网页曲线、命令下发 |

控制链路反向贯通：网页下发指令 → MQTT → ESP32 → 协调器 → 节点 → 继电器动作。

---

## 当前进度

- [x] **第 0 周**　工具链搭建与裸机冒烟测试（IAR + CC-Debugger + UART + GPIO 全链路打通）
- [x] **第 1 步**　节点读取多路传感器，彩屏本地显示（DHT11 / 光敏 / MQ-2 / 人体红外 / 继电器全部读通）
- [ ] 第 2 步　协调器与终端组网，脱离 USB 独立供电运行
- [ ] 第 3 步　Linux 服务端：串口采集、SQLite 落库、HTTP 接口、网页曲线
- [ ] 第 4 步　ESP32 网关接入，Wi-Fi + MQTT 上云
- [ ] 第 5 步　告警、远程控制、低功耗与 systemd 托管

---

## 技术栈

**嵌入式端**
`C` · `CC2530 (8051)` · `IAR EW8051` · `Z-Stack` · `ZigBee / IEEE 802.15.4` · `UART` · `ADC` · `单总线时序`

**网关端**
`ESP32-S3` · `ESP-IDF` · `FreeRTOS` · `MQTT` · `Wi-Fi` · `BLE 配网`

**服务端**
`Linux` · `C` · `termios` · `epoll` · `signalfd` · `多线程` · `SQLite` · `socket 手写 HTTP` · `systemd`

---

## 目录

```
.
├── index.html              首页
├── posts/
│   ├── project-overview.html   项目全景：架构、功能、技术选型与取舍
│   ├── week0-bringup.html      第 0 周开发日志：工具链、里程碑与踩坑复盘
│   ├── week1-sensors.html      第 1 周开发日志：五路传感器打通与 ADC 三次翻车复盘
│   └── pinout-reference.html   终端节点板引脚速查表
└── assets/
    ├── css/style.css
    └── img/                架构图 / 引脚图 / 路线图 / 实物照片
```

---

## 关于这个仓库

代码与文档同步推进。每完成一个阶段，会把当阶段的源码、遇到的问题和解决过程一起整理进来——踩过的坑比顺利的部分更值得记录。
