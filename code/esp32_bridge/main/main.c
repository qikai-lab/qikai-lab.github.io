/*
 * main.c - ESP32-S3 网关主程序（第 4 步 · 阶段 B：WiFi + MQTT 上云）
 * ===========================================================================
 * 数据流：
 *
 *   CC2530 协调器 P0_3
 *        │ 115200 8N1, 每秒 24 字节
 *        ▼
 *   GPIO18 → UART1 → zigbee_frame.c 解析（滑动窗口重同步）
 *        │
 *        ├──→ 本地日志（不管网络通不通都有，方便断网时仍能现场看数）
 *        │
 *        └──→ mqtt_pub.c → WiFi → mosquitto → 服务端入库
 *
 * 关键设计：**网络故障不影响本地采集**。
 *   WiFi 没连上、broker 挂了，串口照样读、日志照样打，只是发布被丢弃并计数。
 *   这样现场排障时不会出现「网络一坏就什么都看不见」的死局。
 *
 * 分两段验证（对应 README 的 B1 / B2）：
 *   B1：只看 `WiFi 已连接 192.168.110.x` —— 证明能上局域网
 *   B2：再看 `MQTT 已连接` 和 `发布` 计数上涨 —— 证明数据真的到了 broker
 * ===========================================================================
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_cfg.h"
#include "mqtt_pub.h"
#include "wifi_mgr.h"
#include "zigbee_frame.h"

static const char *TAG = "BRIDGE";

static zb_parser_t s_parser;
static uint32_t    s_bytes;         /* 串口收到的原始字节数 */

/* 把一帧打出来，并顺手发布到 MQTT */
static void on_frame(const uint8_t *f, void *user)
{
    static uint32_t shown_raw = 0;
    (void)user;

    /* 前 3 帧打原始十六进制，用来证明字节确实来自串口、而且解析没错位 */
    if (shown_raw < 3) {
        shown_raw++;
        ESP_LOGI(TAG, "[原始] 第 %u 帧的 24 个字节：", (unsigned)shown_raw);
        ESP_LOGI(TAG, "  %02X %02X %02X %02X %02X %02X %02X %02X "
                      "%02X %02X %02X %02X %02X %02X %02X %02X "
                      "%02X %02X %02X %02X %02X %02X %02X %02X",
                 f[0],  f[1],  f[2],  f[3],  f[4],  f[5],  f[6],  f[7],
                 f[8],  f[9],  f[10], f[11], f[12], f[13], f[14], f[15],
                 f[16], f[17], f[18], f[19], f[20], f[21], f[22], f[23]);
    }

    ESP_LOGI(TAG,
             "[帧 #%u] n%-3u %d.%dC  %d.%d%%  lux=%4u(%3d%%)  gas=%4u  "
             "PIR=%s  RLY=%s",
             (unsigned)s_parser.frames, (unsigned)zb_node(f),
             zb_temp_x10(f) / 10, zb_temp_x10(f) % 10,
             zb_humi_x10(f) / 10, zb_humi_x10(f) % 10,
             (unsigned)zb_lux(f), zb_lux_pct(f),
             (unsigned)zb_gas(f),
             zb_pir(f)   ? "有人" : "无人",
             zb_relay(f) ? "ON"   : "OFF");

    /* 发布。失败（未连接/队列满）由 mqtt_pub 内部计数，这里不打断采集 */
    mqtt_pub_env(f);
}

/* ------------------------------ 串口初始化 ------------------------------ */
static void bridge_uart_init(void)
{
    const uart_config_t cfg = {
        .baud_rate  = APP_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* RX 缓冲 4096：打印日志时可能卡几十毫秒，别让串口溢出丢字节 */
    ESP_ERROR_CHECK(uart_driver_install(APP_UART_PORT, 4096, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(APP_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(APP_UART_PORT,
                                 APP_PIN_TX,        /* TX，第 5 步远控才用 */
                                 APP_PIN_RX,        /* RX ← 协调器 P0_3 */
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
}

/* ------------------------------ 接收任务 ------------------------------ */
static void rx_task(void *arg)
{
    uint8_t  chunk[512];
    int64_t  next_stat_us;
    uint32_t last_bytes   = 0;
    uint32_t silent_round = 0;      /* 连续几个统计周期没有新字节 */

    (void)arg;
    next_stat_us = esp_timer_get_time() + APP_STAT_PERIOD_MS * 1000LL;

    for (;;) {
        /* 100ms 超时：既不让统计卡住，也不空转占 CPU */
        int n = uart_read_bytes(APP_UART_PORT, chunk, sizeof(chunk),
                                pdMS_TO_TICKS(100));
        if (n > 0) {
            s_bytes += (uint32_t)n;
            zb_parser_feed(&s_parser, chunk, (size_t)n, on_frame, NULL);
        }

        int64_t now = esp_timer_get_time();
        if (now < next_stat_us) {
            continue;
        }
        next_stat_us = now + APP_STAT_PERIOD_MS * 1000LL;

        /* 一行看全链路：串口 → 解析 → WiFi → MQTT */
        ESP_LOGI(TAG,
                 "---- 5s: 字节=%u 帧=%u 坏帧=%u 无效字节=%u | "
                 "WiFi=%s(%s,%ddBm,断%u次) MQTT=%s 发布=%u 丢弃=%u ----",
                 (unsigned)s_bytes, (unsigned)s_parser.frames,
                 (unsigned)s_parser.bad, (unsigned)s_parser.garbage,
                 wifi_mgr_is_connected() ? "通" : "断",
                 wifi_mgr_ip(), wifi_mgr_rssi(),
                 (unsigned)wifi_mgr_disconnect_count(),
                 mqtt_pub_is_connected() ? "通" : "断",
                 (unsigned)mqtt_pub_ok_count(),
                 (unsigned)mqtt_pub_dropped_count());

        /* 「一个字节都没有」和「有字节但不成帧」是完全不同的故障，分开提示 */
        if (s_bytes == last_bytes) {
            silent_round++;
            if (silent_round >= 2) {
                if (s_bytes == 0) {
                    ESP_LOGW(TAG, "串口一个字节都没有 → 先查线，不用看代码：");
                    ESP_LOGW(TAG, "  1) GPIO%d 是不是真的接在协调器 P0_3 上"
                                  "（不是 P0_2、也不是 P1/P3 那两组）", APP_PIN_RX);
                    ESP_LOGW(TAG, "  2) GND 有没有和协调器共地（这根最容易忘）");
                    ESP_LOGW(TAG, "  3) 协调器还插着 USB 吗？拔了它就没电");
                    ESP_LOGW(TAG, "  4) 终端节点开着吗？协调器收到无线帧才往外吐");
                } else {
                    ESP_LOGW(TAG, "有字节但解不出帧 → 看上面 [原始] 的十六进制：");
                    ESP_LOGW(TAG, "  全是 FF / 00 → 线没接实，或接在别的针上");
                    ESP_LOGW(TAG, "  字节在变但开头不是 73 74 3A 65 → 波特率不一致");
                }
            }
        } else {
            silent_round = 0;
        }
        last_bytes = s_bytes;
    }
}

/* ------------------------------ 入口 ------------------------------ */
void app_main(void)
{
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, " ESP32-S3 网关 · 第 4 步 阶段 B（WiFi + MQTT）");
    ESP_LOGI(TAG, " 接收 : GPIO%d  ← 协调器 P0_3   (UART%d)", APP_PIN_RX, APP_UART_PORT);
    ESP_LOGI(TAG, " 参数 : %d 8-N-1 无流控", APP_UART_BAUD);
    ESP_LOGI(TAG, " 协议 : st: 固定 24 字节，帧头 73 74 3A 65");
    ESP_LOGI(TAG, " WiFi : \"%s\"", APP_WIFI_SSID);
    ESP_LOGI(TAG, " MQTT : %s", APP_MQTT_URI);
    ESP_LOGI(TAG, " 主题 : " APP_TOPIC_ENV "  (QoS%d)", APP_NODE_ID, APP_MQTT_QOS);
    ESP_LOGI(TAG, "============================================");

    zb_parser_reset(&s_parser);

    /* 先起串口：网络初始化可能要几秒，这段时间的数据不能丢 */
    bridge_uart_init();

    /* 接收任务优先级 5，先跑起来 */
    xTaskCreate(rx_task, "rx_task", 4096, NULL, 5, NULL);

    /* 网络的失败不影响采集，所以这里只告警不退出 */
    if (wifi_mgr_start() != 0) {
        ESP_LOGE(TAG, "WiFi 启动失败 —— 串口采集继续，但数据无法上云");
    }
    if (mqtt_pub_start() != 0) {
        ESP_LOGE(TAG, "MQTT 启动失败 —— 串口采集继续，但数据无法上云");
    }
}
