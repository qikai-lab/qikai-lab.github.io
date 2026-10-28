#!/usr/bin/env bash
# ============================================================================
# set_static_ip.sh —— 把 Ubuntu 的 IPv4 固定下来，或者还原成 DHCP
#
# 为什么需要它：
#   ESP32 固件里 broker 地址写的是 IP（app_cfg.h 的 APP_MQTT_URI），
#   Ubuntu 一换 IP，ESP32 就连不上，只能改代码重烧固件。
#   把 Ubuntu 的地址钉死，就不用反复烧板子了。
#
# 用法（都在 Ubuntu 里执行）：
#   sudo bash set_static_ip.sh --show     # 只看现状，什么都不改（安全）
#   sudo bash set_static_ip.sh            # 固定成下面「配置区」里的地址
#   sudo bash set_static_ip.sh --dhcp     # 还原成自动获取（配坏了用这条退回）
#
# ⚠️ 重要前提：静态 IP **只对同一个网段有效**。
#    换成另一个手机热点后，网段会变（比如 10.233.60.x → 192.168.43.x），
#    这时静态 IP 反而比 DHCP 更糟 —— 连路由器都不在同一网段，彻底不通。
#    脚本会先比对「当前网关」和「配置区网关」是否同网段，不同就拒绝执行。
#    真要换热点，先跑 --dhcp 还原，等 DHCP 拿到新地址后重烧一次固件。
# ============================================================================
set -u

# ------------------------------- 配置区 --------------------------------------
# 和 ESP32 固件里 app_cfg.h 的 APP_MQTT_URI 必须一致，改这里就要改那里
# 🔴 2026-10-28 手机热点 ling 重启后网段由 10.233.60.x 变成 10.236.232.x
#    注意：热点每次重启**都可能换网段**，换完要重跑 --show 重新看网关，再改这三行
WANT_IP="10.236.232.172"
WANT_PREFIX=24
WANT_GW="10.236.232.59"
WANT_DNS="$WANT_GW 223.5.5.5"

MARK_BEGIN="# >>> zigbee-static (by set_static_ip.sh) >>>"
MARK_END="# <<< zigbee-static <<<"
# -----------------------------------------------------------------------------

FORCE=0
ACTION="set"
case "${1:-}" in
    --show) ACTION="show" ;;
    --dhcp) ACTION="dhcp" ;;
    --force) ACTION="set"; FORCE=1 ;;
    "")     ACTION="set" ;;
    -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
    *) echo "未知参数：$1（可用 --show / --dhcp / --force）"; exit 2 ;;
esac

# ------------------------------- 小工具 --------------------------------------
say()  { printf '%s\n' "$*"; }
die()  { printf '❌ %s\n' "$*" >&2; exit 1; }

# 取前 3 段，用来判断是否同一网段（/24 够用了）
subnet3() { printf '%s' "$1" | awk -F. '{print $1"."$2"."$3}'; }

prefix_to_mask() {
    local p="$1" i bits mask=""
    for i in 1 2 3 4; do
        if [ "$p" -ge 8 ]; then bits=255; p=$((p - 8))
        else bits=$(( 256 - (2 ** (8 - p)) )); p=0; fi
        mask="${mask}${mask:+.}${bits}"
    done
    printf '%s' "$mask"
}

# 找默认出口网卡
find_iface() {
    local i
    i=$(ip -4 route show default 2>/dev/null | awk '{print $5; exit}')
    if [ -z "$i" ]; then
        i=$(ip -o -4 addr show 2>/dev/null | awk '$2!="lo"{print $2; exit}' | cut -d@ -f1)
    fi
    printf '%s' "$i"
}

# 判断谁在管网卡：nm / netplan / ifupdown
detect_mode() {
    if command -v nmcli >/dev/null 2>&1 \
       && systemctl is-active --quiet NetworkManager 2>/dev/null; then
        printf 'nm'; return
    fi
    if command -v netplan >/dev/null 2>&1 && ls /etc/netplan/*.yaml >/dev/null 2>&1; then
        printf 'netplan'; return
    fi
    printf 'ifupdown'
}

# nmcli 里管这块网卡的连接名
nm_con_name() {
    local d="$1" n
    n=$(nmcli -t -f NAME,DEVICE con show --active 2>/dev/null \
        | awk -F: -v d="$d" '$2==d{print $1; exit}')
    [ -z "$n" ] && n=$(nmcli -t -f NAME,DEVICE con show 2>/dev/null \
        | awk -F: -v d="$d" '$2==d{print $1; exit}')
    printf '%s' "$n"
}

# 从 /etc/network/interfaces 里摘掉本脚本加的块（别的行原样保留）
strip_block() {
    awk -v b="$MARK_BEGIN" -v e="$MARK_END" '
        $0==b {skip=1; next}
        $0==e {skip=0; next}
        !skip {print}
    ' "$1"
}

# ------------------------------- 前置检查 ------------------------------------
IFACE=$(find_iface)
MODE=$(detect_mode)

if [ "$ACTION" = "show" ]; then
    say "============================================================"
    say " 网卡        : ${IFACE:-（没找到，可能没插网线/没连上）}"
    case "$MODE" in
        nm)       say " 管理方式    : NetworkManager（用 nmcli 配置）" ;;
        netplan)  say " 管理方式    : netplan（改 /etc/netplan/*.yaml）" ;;
        ifupdown) say " 管理方式    : /etc/network/interfaces（ifupdown）" ;;
    esac
    say " 当前 IPv4   : $(ip -4 -o addr show dev "${IFACE:-lo}" 2>/dev/null | awk '{print $4}' | paste -sd' ' -)"
    say " 默认网关    : $(ip -4 route show default | awk '{print $3; exit}')"
    say " DNS         : $(awk '/^nameserver/{printf "%s ", $2}' /etc/resolv.conf 2>/dev/null)"
    if [ "$MODE" = "nm" ] && [ -n "${IFACE:-}" ]; then
        c=$(nm_con_name "$IFACE")
        [ -n "$c" ] && say " 连接名      : $c ($(nmcli -t -f ipv4.method con show "$c" 2>/dev/null | cut -d: -f2))"
    fi
    say "------------------------------------------------------------"
    say " 脚本目标    : $WANT_IP/$WANT_PREFIX  gw $WANT_GW"
    say "============================================================"
    exit 0
fi

[ "$(id -u)" -eq 0 ] || die "要改网络配置，请用 sudo 跑：sudo bash $0 ${1:-}"
[ -n "${IFACE:-}" ] || die "找不到默认网卡。先把 'ip addr' 的输出发我看一眼"

# 只有「设静态」才需要网段校验；还原 DHCP 任何网段都能做
if [ "$ACTION" = "set" ] && [ "$FORCE" -eq 0 ]; then
    CUR_GW=$(ip -4 route show default | awk '{print $3; exit}')
    if [ -n "$CUR_GW" ] && [ "$(subnet3 "$CUR_GW")" != "$(subnet3 "$WANT_GW")" ]; then
        say "❌ 网段对不上，已中止（没有做任何修改）"
        say "   当前网关    : $CUR_GW"
        say "   配置区网关  : $WANT_GW"
        say ""
        say "   说明你换过热点：Ubuntu 现在在 $(subnet3 "$CUR_GW").x 网段，"
        say "   而脚本里写死的是 $(subnet3 "$WANT_GW").x。这种情况下设成静态 IP"
        say "   会彻底连不上（连路由器都不在同一网段），比 DHCP 更糟。"
        say ""
        say "   两个选择："
        say "     a) 换回 ling 这个热点，再来跑本脚本"
        say "     b) 就待在新网段：改脚本顶部三行 + 改固件 app_cfg.h 重烧"
        exit 1
    fi
fi

say "网卡=$IFACE  管理方式=$MODE  动作=$ACTION"

# ------------------------------- 执行 ----------------------------------------
case "$MODE" in

# ============================ NetworkManager ================================
nm)
    CON=$(nm_con_name "$IFACE")
    [ -n "$CON" ] || die "NetworkManager 里找不到管 $IFACE 的连接，把 'nmcli con show' 的输出发我"

    if [ "$ACTION" = "dhcp" ]; then
        say "还原成 DHCP：$CON"
        nmcli con mod "$CON" ipv4.method auto ipv4.addresses "" ipv4.gateway "" ipv4.dns "" || die "nmcli 修改失败"
    else
        say "固定成 $WANT_IP/$WANT_PREFIX（网关 $WANT_GW）"
        nmcli con mod "$CON" \
            ipv4.method manual \
            ipv4.addresses "$WANT_IP/$WANT_PREFIX" \
            ipv4.gateway "$WANT_GW" \
            ipv4.dns "$WANT_DNS" || die "nmcli 修改失败"
    fi

    nmcli con down "$CON" >/dev/null 2>&1
    nmcli con up   "$CON" || die "连接启用失败（用 --dhcp 退回）"
    ;;

# ================================ netplan ===================================
netplan)
    F=/etc/netplan/99-zigbee-static.yaml
    if [ "$ACTION" = "dhcp" ]; then
        [ -f "$F" ] && rm -f "$F"
        netplan apply || die "netplan apply 失败"
    else
        [ -f "$F" ] && cp -a "$F" "$F.bak.$(date +%s)"
        cat > "$F" <<EOF
network:
  version: 2
  ethernets:
    $IFACE:
      dhcp4: no
      addresses: ["$WANT_IP/$WANT_PREFIX"]
      gateway4: $WANT_GW
      nameservers:
        addresses: [$(printf '%s' "$WANT_DNS" | sed 's/ /, /g')]
EOF
        chmod 600 "$F"
        netplan apply || die "netplan apply 失败（删掉 $F 再 netplan apply 可退回）"
    fi
    ;;

# =============================== ifupdown ===================================
ifupdown)
    F=/etc/network/interfaces
    cp -a "$F" "$F.bak.$(date +%s)" || die "备份 $F 失败，不动了"

    TMP=$(mktemp)
    strip_block "$F" > "$TMP"
    if [ "$ACTION" = "set" ]; then
        {
            printf '\n%s\n' "$MARK_BEGIN"
            printf 'auto %s\n'   "$IFACE"
            printf 'iface %s inet static\n' "$IFACE"
            printf '    address %s\n' "$WANT_IP"
            printf '    netmask %s\n' "$(prefix_to_mask "$WANT_PREFIX")"
            printf '    gateway %s\n' "$WANT_GW"
            printf '    dns-nameservers %s\n' "$WANT_DNS"
            printf '%s\n' "$MARK_END"
        } >> "$TMP"
    fi
    chmod --reference="$F" "$TMP" 2>/dev/null || chmod 644 "$TMP"
    cat "$TMP" > "$F"
    rm -f "$TMP"

    ifdown "$IFACE" --force >/dev/null 2>&1 || true
    ifup "$IFACE" || die "ifup 失败（把 $F.bak.* 覆盖回去再 ifup）"
    ;;

*) die "未知管理方式 $MODE" ;;
esac

# ------------------------------- 结果校验 ------------------------------------
say "------------------------------------------------------------"
say "现在的地址：$(ip -4 -o addr show dev "$IFACE" 2>/dev/null | awk '{print $4}' | paste -sd' ' -)"
say "默认网关  ：$(ip -4 route show default | awk '{print $3; exit}')"

if [ "$ACTION" = "set" ]; then
    if ping -c 3 -W 2 "$WANT_GW" >/dev/null 2>&1; then
        say "✅ 网关 $WANT_GW 可 ping 通"
    else
        say "❌ 网关 ping 不通 —— 自动还原成 DHCP，别慌"
        # 直接内联还原，不再递归调用自己
        case "$MODE" in
            nm)       nmcli con mod "$CON" ipv4.method auto ipv4.addresses "" ipv4.gateway "" ipv4.dns ""; \
                      nmcli con down "$CON" >/dev/null 2>&1; nmcli con up "$CON" ;;
            netplan)  rm -f /etc/netplan/99-zigbee-static.yaml; netplan apply ;;
            ifupdown) f=$(ls -t "$F".bak.* 2>/dev/null | head -1); [ -n "$f" ] && cat "$f" > "$F"; \
                      ifdown "$IFACE" --force >/dev/null 2>&1; ifup "$IFACE" ;;
        esac
        say "   已还原。检查一下手机热点是不是还开着、网线/桥接有没有掉。"
        exit 1
    fi

    if ping -c 2 -W 2 223.5.5.5 >/dev/null 2>&1; then
        say "✅ 外网可达（223.5.5.5）"
    else
        say "⚠️ 外网 ping 不通（不影响 MQTT：broker 走 IP，不用 DNS）"
    fi

    say ""
    say "下一步：在 Ubuntu 上确认 mosquitto 还听着 1883"
    say "  ss -lntp | grep 1883"
    say "然后 ESP32 那边复位一次，应该看到 MQTT 已连接  broker=mqtt://$WANT_IP:1883"
else
    say "✅ 已还原成 DHCP"
fi
