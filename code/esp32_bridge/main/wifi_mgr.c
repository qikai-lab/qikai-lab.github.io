/*
 * wifi_mgr.c - WiFi STA 连接管理实现
 * ===========================================================================
 * 三个容易踩的坑，都在这里处理掉了：
 *
 * 1) NVS 必须先初始化。
 *    WiFi 驱动要把射频校准数据存进 NVS，NVS 没初始化时 esp_wifi_init()
 *    会返回 ESP_ERR_NVS_NO_FREE_PAGES 或者直接崩。所以这里自己负责 NVS。
 *
 * 2) ESP32-S3 只支持 2.4GHz。
 *    如果 SSID 是 5GHz 的，扫描根本扫不到，表现为「一直重连、reason=201
 *    (NO_AP_FOUND)」。reason 码打在日志里，一眼能看出来是这个问题，
 *    不用瞎猜。
 *
 * 3) 重连不能死循环空转。
 *    断开事件里直接调 esp_wifi_connect() 会立刻再失败、再进事件、再调…
 *    短时间内刷爆日志、占满 CPU，串口数据也会被日志淹没。
 *    这里用一次性 esp_timer 延后 2 秒再连，给路由器喘息时间。
 * ===========================================================================
 */
#include "wifi_mgr.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "app_cfg.h"

static const char *TAG = "WIFI";

/* ------------------------------ 对外状态 ------------------------------ */
static volatile bool     s_connected   = false;
static volatile uint32_t s_disconnects = 0;
static volatile uint8_t  s_last_reason = 0;
static char              s_ip[16]      = "0.0.0.0";

static esp_timer_handle_t s_reconn_timer = NULL;
static bool               s_inited       = false;

/* ------------------------------ 重连定时器 ------------------------------ */
/* 断开后 2 秒再发起连接。定时器回调跑在 esp_timer 任务里，
 * esp_wifi_connect() 只是把请求丢给驱动，不会阻塞，可以在这里调。 */
static void reconnect_cb(void *arg)
{
    (void)arg;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "重连请求失败: %s", esp_err_to_name(err));
    }
}

/* ------------------------------ 事件处理 ------------------------------ */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA 启动，开始连接 \"%s\" ...", APP_WIFI_SSID);
        esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        s_connected   = false;
        s_last_reason = d->reason;
        s_disconnects++;

        /* 前 3 次详细打印，之后每 10 次打一条 ——
         * 断线时如果每次都打，串口数据会被日志埋掉，反而看不见真正的问题 */
        if (s_disconnects <= 3 || (s_disconnects % 10) == 0) {
            ESP_LOGW(TAG, "WiFi 断开 (reason=%u, 累计 %u 次)，2 秒后重连",
                     (unsigned)d->reason, (unsigned)s_disconnects);

            /* 这两条 reason 码太常见，直接给结论，省得去查表 */
            if (d->reason == WIFI_REASON_NO_AP_FOUND) {
                ESP_LOGW(TAG, "  reason=201 表示扫不到这个 AP：SSID 写错了，"
                              "或者它是 5GHz 的（ESP32-S3 只支持 2.4GHz）");
            } else if (d->reason == WIFI_REASON_AUTH_FAIL) {
                /* 202 不等于「密码一定错」。手机热点在密码正确时也会偶发
                 * 拒绝一次，2 秒后用同样的密码重连就上去了。所以这里不能
                 * 直接判死，否则每次自愈都像是在报一个新故障。 */
                ESP_LOGW(TAG, "  reason=202 认证失败：密码错，或者热点临时拒绝"
                              "（手机热点偶发，重连同密码通常自愈，不算故障）");
            } else if (d->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) {
                ESP_LOGW(TAG, "  reason=15 四次握手超时：和 202 同类，"
                              "多半还是密码/加密方式的问题，不是信号差");
            }
        }

        if (s_reconn_timer) {
            esp_timer_stop(s_reconn_timer);          /* 已有待执行的，重置 */
            esp_timer_start_once(s_reconn_timer, 2000000);   /* 2 秒 */
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;

        ESP_LOGI(TAG, "============================================");
        ESP_LOGI(TAG, " WiFi 已连接: %s", s_ip);
        ESP_LOGI(TAG, " 信号: %d dBm", wifi_mgr_rssi());
        ESP_LOGI(TAG, "============================================");
        return;
    }
}

/* ------------------------------ 启动 ------------------------------ */
int wifi_mgr_start(void)
{
    if (s_inited) {
        return 0;
    }

    /* NVS：WiFi 驱动要往这里存校准数据 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* 分区满了或者版本变了，擦掉重来。只在第一次刷固件后会遇到 */
        ESP_LOGW(TAG, "NVS 需要擦除重建");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
        return -1;
    }

    err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "netif 初始化失败: %s", esp_err_to_name(err));
        return -1;
    }

    /* 默认事件循环可能已经被别处创建，重复创建返回 ESP_ERR_INVALID_STATE，正常 */
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "事件循环创建失败: %s", esp_err_to_name(err));
        return -1;
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 初始化失败: %s", esp_err_to_name(err));
        return -1;
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, NULL);

    /* 重连定时器：一次性，每次断开重新 arm */
    const esp_timer_create_args_t targs = {
        .callback = reconnect_cb,
        .name     = "wifi_reconn",
    };
    err = esp_timer_create(&targs, &s_reconn_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "重连定时器创建失败: %s", esp_err_to_name(err));
        return -1;
    }

    wifi_config_t wcfg = { 0 };
    strlcpy((char *)wcfg.sta.ssid,     APP_WIFI_SSID,     sizeof(wcfg.sta.ssid));
    strlcpy((char *)wcfg.sta.password, APP_WIFI_PASSWORD, sizeof(wcfg.sta.password));
    /* WPA2 起步。如果路由器是 WPA3-only，这里要放宽到 WIFI_AUTH_WPA3_PSK */
    wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wcfg.sta.sort_method        = WIFI_CONNECT_AP_BY_SIGNAL;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置 STA 模式失败: %s", esp_err_to_name(err));
        return -1;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置 WiFi 配置失败: %s", esp_err_to_name(err));
        return -1;
    }

    /* 关掉省电模式：省电模式下 WiFi 会周期性休眠，
     * MQTT 的 keepalive 容易超时，而且消息延迟变大。
     * 这个设备是插着电用的，不差这点功耗。 */
    esp_wifi_set_ps(WIFI_PS_NONE);

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 启动失败: %s", esp_err_to_name(err));
        return -1;
    }

    s_inited = true;
    return 0;
}

/* ------------------------------ 查询接口 ------------------------------ */
bool wifi_mgr_is_connected(void)
{
    return s_connected;
}

const char *wifi_mgr_ip(void)
{
    return s_ip;
}

int wifi_mgr_rssi(void)
{
    wifi_ap_record_t ap;
    if (!s_connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}

uint32_t wifi_mgr_disconnect_count(void)
{
    return s_disconnects;
}

uint8_t wifi_mgr_last_reason(void)
{
    return s_last_reason;
}
