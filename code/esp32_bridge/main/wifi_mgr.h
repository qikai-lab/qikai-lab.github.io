/*
 * wifi_mgr.h - WiFi STA 连接管理
 * ===========================================================================
 * 为什么不直接在 main.c 里调 esp_wifi_start() 就完事：
 *   现场部署时 WiFi 会断（路由器重启、信号差、邻居改信道）。
 *   IoT 设备必须能自己缓过来，而且要把「断了几次」暴露出来 ——
 *   不然出故障时只能看到「没数据」，看不出是设备死了还是网断了。
 *
 * 这个模块只做三件事：
 *   1) 连上指定的 AP，断了自动重连（指数退避，别把路由器打挂）
 *   2) 对外提供「现在通不通 / IP 是多少 / 信号多强」
 *   3) 记录累计断线次数和最近一次断开原因，供日志和诊断用
 * ===========================================================================
 */
#ifndef WIFI_MGR_H
#define WIFI_MGR_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief 初始化 WiFi 并开始连接（非阻塞，连上后 ip 事件里置位）
 * @return 0 成功启动；负数 失败
 */
int wifi_mgr_start(void);

/** 当前是否已拿到 IP（有 IP 才算真的能用，STA_START 不算） */
bool wifi_mgr_is_connected(void);

/** 当前 IP 字符串，未连接时返回 "0.0.0.0" */
const char *wifi_mgr_ip(void);

/** 信号强度 dBm（-30 很强，-80 很弱），未连接返回 0 */
int wifi_mgr_rssi(void);

/** 累计断线次数（诊断用） */
uint32_t wifi_mgr_disconnect_count(void);

/** 最近一次断开的原因码（wifi_err_reason_t），0 = 还没断过 */
uint8_t wifi_mgr_last_reason(void);

#endif /* WIFI_MGR_H */
