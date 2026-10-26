/*
 * gw_serial.c - 串口封装实现
 */
#include "gw_serial.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>

/* 把整数波特率映射到 termios 常量 */
static int baud_to_speed(int baud, speed_t *out)
{
    switch (baud) {
    case 1200:   *out = B1200;   return 0;
    case 2400:   *out = B2400;   return 0;
    case 4800:   *out = B4800;   return 0;
    case 9600:   *out = B9600;   return 0;
    case 19200:  *out = B19200;  return 0;
    case 38400:  *out = B38400;  return 0;
    case 57600:  *out = B57600;  return 0;
    case 115200: *out = B115200; return 0;
    case 230400: *out = B230400; return 0;
    case 460800: *out = B460800; return 0;
    case 921600: *out = B921600; return 0;
    default:     return -1;
    }
}

/* 配置 8-N-1、无流控、非阻塞 */
static int serial_config(int fd, int baud)
{
    struct termios tio;
    speed_t spd;

    if (baud_to_speed(baud, &spd) != 0) {
        errno = EINVAL;
        return -1;
    }

    if (tcgetattr(fd, &tio) != 0) {
        return -1;
    }

    /* 原始模式：不做任何字符转换、不回显、不做行缓冲 */
    cfmakeraw(&tio);

    tio.c_cflag |= (CLOCAL | CREAD);   /* 忽略调制解调器控制线，使能接收 */
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;                /* 8 数据位 */
    tio.c_cflag &= ~PARENB;            /* 无校验 */
    tio.c_cflag &= ~CSTOPB;            /* 1 停止位 */
    tio.c_cflag &= ~CRTSCTS;           /* 无硬件流控 */

    tio.c_iflag &= ~(IXON | IXOFF | IXANY);  /* 无软件流控 */

    /* 非阻塞读：VMIN=0 VTIME=0 表示立即返回 */
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;

    if (cfsetispeed(&tio, spd) != 0 || cfsetospeed(&tio, spd) != 0) {
        return -1;
    }
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        return -1;
    }

    /* 清空收发缓冲，避免残留脏数据 */
    tcflush(fd, TCIOFLUSH);
    return 0;
}

int gw_serial_open(gw_serial_t *s, const char *dev, int baud)
{
    int fd;

    if (s == NULL || dev == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(s, 0, sizeof(*s));
    s->fd = -1;

    /*
     * 目标设备和波特率在尝试打开之前就记下来。
     *
     * 以前这两行写在 open() 成功之后，于是"第一次打开失败"会留下一个
     * dev 为空、baud 为 0 的句柄 —— 上层拿它调 gw_serial_reopen() 时
     * 等于 open("")，永远不可能成功。启动时串口还没插好，
     * 就再也接不上了，只能靠重启进程。
     */
    s->baud = baud;
    snprintf(s->dev, sizeof(s->dev), "%s", dev);

    fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }

    if (serial_config(fd, baud) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    s->fd = fd;
    return 0;
}

void gw_serial_close(gw_serial_t *s)
{
    if (s != NULL && s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}

int gw_serial_reopen(gw_serial_t *s)
{
    int fd;

    if (s == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* 先把旧 fd 关掉，否则 USB 设备重新枚举后会泄漏 fd */
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }

    fd = open(s->dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }

    if (serial_config(fd, s->baud) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    s->fd = fd;
    return 0;
}

int gw_serial_read(gw_serial_t *s, void *buf, size_t n)
{
    ssize_t r;

    if (s == NULL || s->fd < 0) {
        errno = EBADF;
        return -1;
    }

    do {
        r = read(s->fd, buf, n);
    } while (r < 0 && errno == EINTR);   /* 被信号打断就重试 */

    if (r > 0) {
        return (int)r;
    }

    if (r == 0) {
        return GW_SERIAL_EOF;   /* read() 返回 0 = 对端已关闭，设备被拔了 */
    }

    /* EAGAIN/EWOULDBLOCK：暂无数据，不是错误 */
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
    }

    /* 其余（EIO / ENODEV / EBADF）视为设备异常，交由上层重连 */
    return -1;
}

int gw_serial_write(gw_serial_t *s, const void *buf, size_t n)
{
    ssize_t w;

    if (s == NULL || s->fd < 0) {
        errno = EBADF;
        return -1;
    }

    do {
        w = write(s->fd, buf, n);
    } while (w < 0 && errno == EINTR);

    if (w < 0) {
        return -1;
    }
    return (int)w;
}
