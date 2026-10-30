/*
 * mqtt_pub.c - MQTT 发布模块实现
 * ===========================================================================
 * 用到的三个 MQTT 特性，都是实际项目里必须有的：
 *
 * 1) QoS 1（至少送达一次）
 *    环境数据丢一条不影响趋势，但长期漏数据会让曲线出现假的「空档」。
 *    QoS1 保证 broker 收到，代价是可能重复 —— 服务端按 (node, ts) 去重即可。
 *
 * 2) 遗嘱消息（LWT, Last Will and Testament）
 *    连接时就把「我掉线了」这句话交给 broker 保管。设备一旦异常断线
 *    （拔电、死机、WiFi 掉），broker 会**代它**发出 offline。
 *    这比自己轮询「设备在不在」可靠得多，是 IoT 的标准做法。
 *
 * 3) retain 标记
 *    status 主题用 retain=1，新订阅者一连上就能立刻拿到当前状态，
 *    不用等下次心跳。env 主题不用 retain（它是数据流，不是状态）。
 * ===========================================================================
 */
#include "mqtt_pub.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "mqtt_client.h"

#include "app_cfg.h"
#include "zigbee_frame.h"

static const char *TAG = "MQTT";

/* 由 main.c 提供：把一字节命令从 UART1 发给协调器。
 * 这里只负责"把 MQTT 消息翻译成命令字"，不碰串口细节。 */
void bridge_send_cmd(uint8_t cmd);

static esp_mqtt_client_handle_t s_client   = NULL;
static volatile bool            s_connected = false;

/* 主题串在启动时按节点号生成一次，不在每帧里反复 snprintf */
static char s_topic_env[64];
static char s_topic_status[64];
static char s_topic_cmd[64];

static volatile uint32_t s_ok      = 0;
static volatile uint32_t s_dropped = 0;

/* ------------------------------ 事件处理 ------------------------------ */
static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;

    switch ((esp_mqtt_event_id_t)id) {

    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "============================================");
        ESP_LOGI(TAG, " MQTT 已连接  broker=%s", APP_MQTT_URI);
        ESP_LOGI(TAG, " 发布主题: %s  (QoS%d)", s_topic_env, APP_MQTT_QOS);
        ESP_LOGI(TAG, " 命令主题: %s  (QoS1)", s_topic_cmd);
        ESP_LOGI(TAG, "============================================");

        /* 上线通告：retain=1，让后订阅的人立刻知道设备活着 */
        esp_mqtt_client_publish(s_client, s_topic_status,
                                APP_STATUS_ONLINE, 0, 1, 1);

        /* 订阅命令主题。放在 CONNECTED 里而不是启动时：断线重连后
         * 会话就没了，只有在这里重新订阅才保证一直收得到。
         * QoS1 —— 宁可重复收（命令是幂等的，开就是开），不能漏。 */
        esp_mqtt_client_subscribe(s_client, s_topic_cmd, 1);
        break;

    case MQTT_EVENT_DATA:
        /* 只订阅了一个主题，所以不比对 topic 也能确定来源。
         * ev->data 不以 '\0' 结尾，长度必须看 data_len，不能当字符串用。 */
        if (ev->data_len < 1) {
            break;
        }
        ESP_LOGI(TAG, "[收到命令] payload='%.*s' (%d 字节)",
                 ev->data_len, ev->data, ev->data_len);

        if (ev->data[0] == '1') {
            bridge_send_cmd(0x41);      /* 节点1 · 继电器 · 吸合 */
        } else if (ev->data[0] == '0') {
            bridge_send_cmd(0x40);      /* 节点1 · 继电器 · 断开 */
        } else {
            ESP_LOGW(TAG, "  只认 '1'(开) 和 '0'(关)，这条忽略");
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "MQTT 断开（客户端会自动重连；断开期间的数据会被丢弃）");
        break;

    case MQTT_EVENT_ERROR:
        if (ev->error_handle) {
            if (ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                /* 连不上 broker 时最常见的就是这一条 */
                ESP_LOGE(TAG, "TCP 连接失败: sock_errno=%d, tls=0x%x",
                         ev->error_handle->esp_transport_sock_errno,
                         ev->error_handle->esp_tls_last_esp_err);
                ESP_LOGE(TAG, "  → 检查 broker 地址/端口是否可达："
                              "先在 Ubuntu 上 `ss -lntp | grep 1883`");
            } else {
                ESP_LOGE(TAG, "MQTT 错误, type=%d",
                         ev->error_handle->error_type);
            }
        }
        break;

    case MQTT_EVENT_PUBLISHED:
        /* QoS1 收到 PUBACK 才会走到这里，说明 broker 真的收到了 */
        break;

    default:
        break;
    }
}

/* ------------------------------ 启动 ------------------------------ */
int mqtt_pub_start(void)
{
    if (s_client) {
        return 0;
    }

    snprintf(s_topic_env,    sizeof(s_topic_env),    APP_TOPIC_ENV,    APP_NODE_ID);
    snprintf(s_topic_status, sizeof(s_topic_status), APP_TOPIC_STATUS, APP_NODE_ID);
    snprintf(s_topic_cmd,    sizeof(s_topic_cmd),    APP_TOPIC_CMD,    APP_NODE_ID);

    esp_mqtt_client_config_t cfg = { 0 };
    cfg.broker.address.uri = APP_MQTT_URI;

    /* 空字符串传进去会被当成「有用户名但为空」，匿名访问必须传 NULL */
    if (APP_MQTT_USER[0] != '\0') {
        cfg.credentials.username = APP_MQTT_USER;
        cfg.credentials.authentication.password = APP_MQTT_PASS;
    }

    /* 遗嘱消息：连上的同时交给 broker 保管 */
    cfg.session.last_will.topic  = s_topic_status;
    cfg.session.last_will.msg    = APP_STATUS_OFFLINE;
    cfg.session.last_will.qos    = 1;
    cfg.session.last_will.retain = 1;

    cfg.session.keepalive = APP_MQTT_KEEPALIVE;
    cfg.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;

    /* 自动重连间隔。WiFi 没通时会一直失败，5 秒一次不算吵 */
    cfg.network.reconnect_timeout_ms = 5000;

    /* 收发缓冲。JSON 只有 100 来字节，1KB 足够；
     * out_size 放大是给 QoS1 的 outbox 留余量（最多能压 2KB 未确认消息） */
    cfg.buffer.size     = 1024;
    cfg.buffer.out_size = 2048;

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "MQTT 客户端初始化失败");
        return -1;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);

    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MQTT 启动失败: %s", esp_err_to_name(err));
        return -1;
    }

    ESP_LOGI(TAG, "MQTT 客户端已启动，目标 %s", APP_MQTT_URI);
    return 0;
}

/* ------------------------------ 发布 ------------------------------ */
int mqtt_pub_env(const uint8_t *raw)
{
    if (!s_connected || !s_client) {
        s_dropped++;
        return -1;
    }

    /* 温湿度用 x10 定点整数，和 SQLite 里的存储口径完全一致，
     * 避免服务端还要再做一次浮点换算（也避免 19.4 存成 19.399999） */
    char json[160];
    int n = snprintf(json, sizeof(json),
                     "{\"node\":%u,\"temp\":%d,\"hum\":%d,"
                     "\"lux\":%u,\"gas\":%u,\"pir\":%u,\"relay\":%u}",
                     (unsigned)zb_node(raw),
                     zb_temp_x10(raw),
                     zb_humi_x10(raw),
                     (unsigned)zb_lux(raw),
                     (unsigned)zb_gas(raw),
                     (unsigned)zb_pir(raw),
                     (unsigned)zb_relay(raw));

    if (n <= 0 || n >= (int)sizeof(json)) {
        s_dropped++;
        return -1;
    }

    int msg_id = esp_mqtt_client_publish(s_client, s_topic_env, json, n,
                                         APP_MQTT_QOS, 0 /* retain=0 */);
    if (msg_id < 0) {
        /* outbox 满了（网络卡住但还没判定断线），这条丢掉 */
        s_dropped++;
        return -1;
    }

    s_ok++;
    return msg_id;
}

/* ------------------------------ 查询 ------------------------------ */
bool mqtt_pub_is_connected(void)
{
    return s_connected;
}

uint32_t mqtt_pub_ok_count(void)
{
    return s_ok;
}

uint32_t mqtt_pub_dropped_count(void)
{
    return s_dropped;
}
