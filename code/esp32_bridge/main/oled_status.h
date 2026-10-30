/*
 * oled_status.h - 0.91" OLED 状态屏（SSD1306 / 128×32 / I2C）
 * ===========================================================================
 * 这块屏只做一件事：**把网关自己的状态显示出来**，让演示时不用开着电脑
 * 盯串口，也让「拔了 USB 只插电池」的时候仍能一眼看出链路通不通。
 *
 * 它和 CC2530 终端板上那块 1.44" 彩屏不是一回事：
 *   终端彩屏  → 传感器的值（温度、湿度、光照、烟雾、有人、继电器）
 *   这块 OLED → 网关的值（WiFi 有没有、IP 是多少、MQTT 通不通、发了几条、丢了几条）
 *
 * 设计原则：**屏是旁路，不是链路**。
 *   - 初始化失败（没接屏、地址不对、线松）只打一行警告，程序继续跑
 *   - 刷新失败只累加计数，不重试、不阻塞、不打印（否则串口会被刷爆）
 *   - 所有 I2C 写都有超时，最坏情况拖 20ms，不会拖住收帧
 * ===========================================================================
 */
#ifndef OLED_STATUS_H
#define OLED_STATUS_H

#include <stdbool.h>
#include <stdint.h>

/* 一屏要显示的东西。字段和 main.c 里现成的数据一一对应，不额外采集任何信息 */
typedef struct {
    bool        wifi_ok;     /* WiFi 是否拿到 IP */
    const char *ip;          /* 点分十进制字符串，如 "10.236.232.204"；NULL 或空串显示 "--" */
    int         rssi;        /* 信号强度 dBm，如 -29 */

    bool        mqtt_ok;     /* MQTT 是否连上 broker */
    uint32_t    tx_ok;       /* 成功发布条数 */
    uint32_t    dropped;     /* 因未连接而丢弃的条数 */

    uint32_t    frames;      /* 解析出的有效帧总数 */
    uint32_t    bad;         /* 坏帧计数 */

    /* 最新一帧的传感器值 */
    uint8_t     node;
    int         temp_x10;    /* 18.3℃ → 183 */
    int         hum_x10;     /* 55.0%  → 550 */
    uint16_t    lux;         /* ADC 0~1023 */
    uint16_t    gas;         /* ADC 0~1023 */
    uint8_t     pir;         /* 0/1 */
    uint8_t     relay;       /* 0/1 */
} oled_info_t;

/* 初始化 I2C + 屏。
 * 返回 0 = 屏有应答，可以用；返回 -1 = 没找到屏（不致命，调用方继续跑）。
 * 内部会先做一次「地址探测」，因为很多模块是 0x3D 而不是 0x3C。 */
int oled_status_init(void);

/* 是否需要刷新（按 APP_OLED_REFRESH_MS 节流）。调用方可以每帧都问一次，
 * 只有返回 true 时才需要真的调用 oled_status_render()。 */
bool oled_status_due(void);

/* 画一屏并推到屏上。
 * 未初始化成功时直接返回，安全可重复调用。
 * 内容没变化时也会重画（成本 13ms，比维护脏标记更简单可靠）。 */
void oled_status_render(const oled_info_t *in);

/* 屏是否可用（init 成功过）。给主程序打日志用。 */
bool oled_status_ok(void);

#endif /* OLED_STATUS_H */
