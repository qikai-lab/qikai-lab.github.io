#!/bin/sh
# ============================================================================
# setup_ubuntu.sh —— 在 Ubuntu 里一键装好网关的编译环境
#
# 用法（在 Ubuntu 终端里，进到本文件所在目录后）：
#     chmod +x setup_ubuntu.sh
#     ./setup_ubuntu.sh
#
# 干四件事：
#   1. 打印系统版本，方便确认是哪一版 Ubuntu
#   2. apt update；如果源已失效（16.04/18.04 早已 EOL），自动换 old-releases
#   3. 装 build-essential / libsqlite3-dev / sqlite3 / socat
#   4. 把当前用户加入 dialout 组（否则读 /dev/ttyUSB0 会 Permission denied）
# ============================================================================
set -e

echo "============================================================"
echo " [1/4] 系统版本"
echo "============================================================"
if [ -f /etc/os-release ]; then
    . /etc/os-release
    echo "  $PRETTY_NAME   (codename: ${VERSION_CODENAME:-${UBUNTU_CODENAME:-unknown}})"
else
    echo "  读不到 /etc/os-release，下面用 lsb_release 看："
    lsb_release -a 2>/dev/null || true
fi
gcc --version 2>/dev/null | head -1 || echo "  gcc 还没装（正常，下面会装）"

echo
echo "============================================================"
echo " [2/4] 刷新软件源"
echo "============================================================"
if sudo apt-get update -qq; then
    echo "  软件源正常"
else
    echo "  软件源不通 —— 多半是系统版本已经 EOL，官方把包移到了 old-releases"
    BAK="/etc/apt/sources.list.bak.$(date +%Y%m%d%H%M)"
    sudo cp /etc/apt/sources.list "$BAK"
    echo "  原文件已备份到 $BAK"
    sudo sed -i \
        -e 's|https\?://[a-zA-Z0-9._-]*archive\.ubuntu\.com|http://old-releases.ubuntu.com|g' \
        -e 's|https\?://[a-zA-Z0-9._-]*security\.ubuntu\.com|http://old-releases.ubuntu.com|g' \
        -e 's|https\?://[a-zA-Z0-9._-]*ports\.ubuntu\.com|http://old-releases.ubuntu.com|g' \
        /etc/apt/sources.list
    echo "  已改为 old-releases.ubuntu.com，重试："
    sudo apt-get update
fi

echo
echo "============================================================"
echo " [3/4] 安装编译依赖"
echo "============================================================"
sudo apt-get install -y build-essential libsqlite3-dev sqlite3 socat

echo
echo "  版本确认："
echo -n "    gcc     : "; gcc --version | head -1
echo -n "    make    : "; make --version | head -1
echo -n "    sqlite3 : "; sqlite3 --version 2>/dev/null | cut -d' ' -f1

echo
echo "============================================================"
echo " [4/4] 串口权限（dialout 组）"
echo "============================================================"
if id -nG "$USER" | tr ' ' '\n' | grep -qx dialout; then
    echo "  $USER 已经在 dialout 组里，不用动"
else
    sudo usermod -aG dialout "$USER"
    echo "  已把 $USER 加入 dialout"
    echo "  ！必须注销重新登录（或重启）才生效，只关终端窗口不算"
fi

echo
echo "============================================================"
echo " 完成。下一步："
echo "   make test        # 跑 50 项单元测试，不需要板子"
echo "   make             # 编译网关主程序 gw"
echo "============================================================"
