#!/bin/sh
#
# install_service.sh - 把 gw 装成 systemd 服务
#
# 用法：
#   sudo ./install_service.sh                  # 默认 /dev/ttyUSB0 + 8080
#   sudo ./install_service.sh /dev/ttyUSB1     # 换串口
#   sudo ./install_service.sh /dev/ttyUSB0 9000
#
# 做的事：
#   1. 生成 /etc/systemd/system/zigbee-gw.service
#   2. daemon-reload + enable（开机自启）+ 立即启动
#   3. 打印状态
#
# 不做的事：不改 gw.db、不动你的代码。想卸载：
#   sudo systemctl disable --now zigbee-gw && sudo rm /etc/systemd/system/zigbee-gw.service
#
set -eu

SERVICE=zigbee-gw
DIR=$(cd "$(dirname "$0")" && pwd)
DEV=${1:-/dev/ttyUSB0}
PORT=${2:-8080}

# ---- 0. 自愈：给自己补上可执行位 ----
# 从 Windows 打包过来的 .sh 没有 +x，那时 `sudo ./install_service.sh` 报的是
# "command not found"（sudo 对权限不足的提示文案就是这句，非常误导）。
# 只要用 `bash install_service.sh` 跑过一次，这里就把位补上，之后 ./ 也能用。
chmod +x "$0" 2>/dev/null || true

# ---- 1. 必须是 root ----
if [ "$(id -u)" -ne 0 ]; then
    echo "需要 root。请这样运行："
    echo "    sudo $0 $*"
    exit 1
fi

# ---- 2. 服务以哪个用户跑：sudo 时用调用者，而不是 root ----
# 用 root 跑会让 gw.db 变成 root 所有，之后你自己 sudo 才能改，很别扭。
RUN_USER=${SUDO_USER:-$(id -un)}
if [ "$RUN_USER" = "root" ]; then
    echo "警告：检测到以 root 身份直接运行，服务将用 root 跑。"
    echo "      建议用普通用户 sudo 执行本脚本。"
fi

# ---- 3. 二进制必须在 ----
if [ ! -x "$DIR/gw" ]; then
    echo "找不到可执行文件：$DIR/gw"
    echo "先在 $DIR 下执行 make"
    exit 1
fi

# ---- 4. 端口别被占着 ----
# 只报告「谁占着」，不猜原因。之前的版本一口咬定"是你手动跑的 ./gw 还开着"，
# 结果占端口的是别的东西时把人误导了 —— 检查只负责给事实，判断交给用户。
if command -v ss >/dev/null 2>&1; then
    HOLDER=$(ss -ltnp 2>/dev/null | grep -E ":$PORT\b" || true)
    if [ -n "$HOLDER" ]; then
        echo "端口 $PORT 已被占用："
        echo ""
        echo "$HOLDER"
        echo ""
        echo "占用的如果是 gw，就是之前手动跑的那个没退干净："
        echo "    sudo pkill -x gw"
        echo ""
        echo "占用的如果是别的程序，换个端口装："
        echo "    sudo $0 $DEV 8081"
        exit 1
    fi
fi

# ---- 5. 串口存在性只是提醒，不阻断 ----
# 开机时 USB 串口可能还没枚举完，这时候硬性要求它存在反而装不上。
# gw 自己有每秒重连逻辑 + Restart=always 兜底，晚一点出现也能自动接上。
if [ ! -e "$DEV" ]; then
    echo "提示：$DEV 现在不存在。若串口是开机后才插上的，服务会自己重连，不用管。"
    echo "      若设备名不是这个，用 ./gw -h 看参数，或 sudo $0 /dev/ttyUSB1 重装。"
fi

# ---- 6. 生成 unit ----
#
# 几个关键决定：
#   WorkingDirectory  决定 ./gw.db 落在哪。和手动跑时同一目录 -> 数据无缝接上。
#   Restart=always    串口掉线、程序崩、OOM 被杀，3 秒后都自己起来。
#   SIGTERM           默认信号。main.c 里 signalfd 同时监听了 SIGINT/SIGTERM，
#                     收到就走"提交未落盘数据 + 关闭资源"那条路，不会丢最后一批。
#   TimeoutStopSec=10 给它 10 秒提交事务。默认 90 秒太长，卡住时等得难受。
cat > "/etc/systemd/system/$SERVICE.service" <<UNIT
[Unit]
Description=ZigBee Environment Gateway (serial -> SQLite -> HTTP)
Documentation=file://$DIR/README.md
After=network.target

[Service]
Type=simple
User=$RUN_USER
WorkingDirectory=$DIR
ExecStart=$DIR/gw -d $DEV -p $PORT
Restart=always
RestartSec=3
TimeoutStopSec=10
StandardOutput=journal
StandardError=journal
SyslogIdentifier=$SERVICE

[Install]
WantedBy=multi-user.target
UNIT

chmod 644 "/etc/systemd/system/$SERVICE.service"

# ---- 7. 启用并启动 ----
systemctl daemon-reload
systemctl enable "$SERVICE" >/dev/null 2>&1
systemctl restart "$SERVICE"

sleep 1

echo ""
echo "=========================================="
echo " 已安装并启动：$SERVICE"
echo "=========================================="
echo " 用户     : $RUN_USER"
echo " 目录     : $DIR"
echo " 串口     : $DEV"
echo " 网页     : http://localhost:$PORT/"
echo " 数据库   : $DIR/gw.db"
echo "------------------------------------------"
echo " 看日志   : journalctl -u $SERVICE -f"
echo " 看状态   : systemctl status $SERVICE"
echo " 重启     : sudo systemctl restart $SERVICE"
echo " 停止     : sudo systemctl stop $SERVICE"
echo " 卸载     : sudo systemctl disable --now $SERVICE"
echo "            sudo rm /etc/systemd/system/$SERVICE.service"
echo "------------------------------------------"
systemctl is-active "$SERVICE" >/dev/null 2>&1 \
    && echo " 当前状态 : 运行中 ✓" \
    || echo " 当前状态 : 未运行 ✗  用 journalctl -u $SERVICE -n 30 看原因"
echo ""
