/*
 * http_srv.h - 内置 HTTP 服务（M3）
 * ---------------------------------------------------------------------------
 * 为什么自己写、不用 libmicrohttpd / mongoose
 * --------------------------------------------
 *   1) 这里只需要 4 个 GET 接口，不解析表单、不处理 POST、不需要 TLS。
 *      引一个框架反而要多带几万行代码，面试时也说不清自己做了什么。
 *   2) 必须和串口采集跑在**同一个 epoll 主循环**里 —— 单线程、
 *      无锁、无队列。用框架就得起线程，还要处理跨线程数据同步。
 *   3) 这是一个求职项目，手写一遍 HTTP 请求解析是加分项。
 *
 * 架构（这是 M3 的核心决定）
 * --------------------------
 *
 *       串口 ──► 解析 ──► 内存快照 gw_latest ──► /api/latest  （0 延迟）
 *                     └──► SQLite（批量提交）──► /api/history（查历史）
 *
 *   「实时值走内存，历史值走库」的原因：
 *     SQLite 攒 20 条才提交一次事务，未提交的行**别的连接看不见**。
 *     如果实时值也查库，网页上的「当前温度」最多会滞后 19 秒。
 *
 * 实现取舍（都是刻意的）
 * ----------------------
 *   · GET only，请求头读到 \r\n\r\n 就停，不解析 Header 内容（不需要）
 *   · Connection: close —— 每个请求一个连接，响应完就关。
 *     少一层 keep-alive 状态机，几十个并发毫无压力。
 *   · 响应一次 write 写完；真遇到 EAGAIN 就直接断开（响应最大 8KB，
 *     小于默认 socket 发送缓冲，正常不会发生）。
 *   · 客户端槽位有超时（浏览器会预连接然后不说话，不清掉会把槽位占满）。
 * ---------------------------------------------------------------------------
 */
#ifndef GW_HTTP_SRV_H
#define GW_HTTP_SRV_H

#include <stdint.h>

#include "gw_db.h"

#define HTTP_MAX_CLIENTS   6        /* 同时挂着的连接上限（浏览器一般 2~4 个） */
#define HTTP_REQ_MAX       1024     /* GET 请求头不可能比这长 */
#define HTTP_RESP_MAX      16384    /* 网页 HTML 约 10KB，历史 120 点约 9KB，都放得下 */

#define HTTP_DEFAULT_PORT  8080

typedef struct http_srv http_srv_t;

/**
 * @brief 创建监听 socket 并 bind/listen
 * @param port  端口，0 表示用默认 8080
 * @param db    历史查询用的数据库句柄，**可以为 NULL**（此时 /api/history 返回空）
 * @return 句柄；失败返回 NULL（端口被占用是最常见原因）
 */
http_srv_t *http_srv_open(int port, gw_db_t *db);

/** 关闭所有连接和监听 socket */
void http_srv_close(http_srv_t *s);

/** 监听 fd，主循环把它注册进 epoll（EPOLLIN） */
int http_srv_listen_fd(const http_srv_t *s);

/** 实际监听端口（传 0 时由内核分配，调试用） */
int http_srv_port(const http_srv_t *s);

/**
 * @brief 把 epoll 句柄交给本模块，之后接受到的新连接由它自己注册
 * @note  传进来之后，accept 里会自动 epoll_ctl(ADD)，close 时自动 DEL，
 *        main 侧完全不用管新 fd 的生命周期。
 */
void http_srv_set_epoll(http_srv_t *s, int epfd);

/**
 * @brief 接受所有排队中的连接，并用 epoll 注册
 * @return 本次接受的连接数（0 = 没有新连接；<0 = accept 出错，通常可忽略）
 */
int http_srv_accept(http_srv_t *s);

/**
 * @brief fd 是不是本模块管理的客户端
 * @return 槽位下标（>=0），不是则返回 -1
 */
int http_srv_client_index(const http_srv_t *s, int fd);

/**
 * @brief 处理一个客户端的可读事件：读请求 → 生成响应 → 发完就关
 * @param idx http_srv_client_index() 的返回值
 */
void http_srv_handle(http_srv_t *s, int idx);

/** @brief 定期调用：关掉超时没动静的连接（浏览器预连接会占槽位） */
void http_srv_tick(http_srv_t *s, int64_t now_ms);

#endif /* GW_HTTP_SRV_H */
