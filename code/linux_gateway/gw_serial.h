/*
 * gw_serial.h - 串口封装（POSIX termios）
 *
 * 设计要点：
 *   1) 非阻塞模式（O_NONBLOCK）—— 配合 epoll 使用，不阻塞主线程
 *   2) 传入 EOF 时返回 0 而非 -1，与"暂无数据"区分开
 *   3) 支持断线重连 —— USB 转串口设备长时间运行会掉线，/dev/ttyUSB0 会消失，
 *      这是实操中最常见的问题，必须在框架层面处理
 */
#ifndef GW_SERIAL_H
#define GW_SERIAL_H

#include <stddef.h>

#define GW_SERIAL_DEV_MAX   128

typedef struct {
    int  fd;
    char dev[GW_SERIAL_DEV_MAX];
    int  baud;
} gw_serial_t;

/* 返回值：0 成功，-1 失败（errno 有值） */
int  gw_serial_open(gw_serial_t *s, const char *dev, int baud);
void gw_serial_close(gw_serial_t *s);

/* 返回值：0 成功，-1 失败 */
int gw_serial_reopen(gw_serial_t *s);

/**
 * @brief 非阻塞读
 * @return >0 读到的字节数
 *          0 暂无数据（EAGAIN），属正常情况，调用方继续等下一次事件
 *   GW_SERIAL_EOF 对端关闭（read() 返回 0）—— **设备已断开，必须重连**
 *         -1 设备异常（EIO/ENODEV 等），调用方应触发重连
 *
 * 为什么 0 和 EOF 必须分开：
 *   以前两者都返回 0，上层会把"设备已拔"当成"暂时没数据"。有些 USB 转串口
 *   驱动拔掉时只报 EPOLLIN、不报 EPOLLHUP，这时 epoll_wait 会持续返回可读、
 *   read 持续返回 0 —— 整个事件循环变成 100% CPU 的忙等，而且永远不重连。
 */
#define GW_SERIAL_EOF  (-2)
int gw_serial_read(gw_serial_t *s, void *buf, size_t n);

/* @return 实际写入字节数，-1 表示失败 */
int gw_serial_write(gw_serial_t *s, const void *buf, size_t n);

#endif /* GW_SERIAL_H */
