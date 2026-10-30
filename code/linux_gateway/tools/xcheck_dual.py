#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
xcheck_dual.py —— 双链路交叉验证

背景
----
同一块 CC2530 节点的数据，被两条**互相独立**的路径送进 Ubuntu。
注意这两条链路是**两个独立的程序**，必须同时跑：

    终端1  ./gw -d /dev/ttyUSB0 -f gw.db -p 8080     → 写 gw.db     (env_data)
    终端2  ./mqtt_sub                                 → 写 env_mqtt.db (env_data_mqtt)
           （默认参数就是 127.0.0.1 1883 env_mqtt.db，订阅 zigbee/+/env）

    A. 串口链路：节点 → 协调器 → P0_3 → CH340/USB → ./gw → env_data
    B. MQTT 链路：节点 → 协调器 → P0_3 → ESP32 → WiFi → mosquitto
                  → ./mqtt_sub → env_data_mqtt

两条链路从「协调器 P0_3」这个点之后**完全分叉**，各自解析、各自落库。
所以：如果两边同一帧的温湿度/光照/烟雾/人体全部对得上，就说明
「串口解析」「ESP32 帧解析」「JSON 组包/拆包」「MQTT 传输」「落库」
这一整条链上没有任何一处把字节搞错。这比任何单条日志都有说服力。

为什么不能简单地按 ts 相等来比对
--------------------------------
两边的 ts 都是**服务端收到时刻**（`time(NULL)`），不是节点采样时刻：
  · env_data.ts       → main.c:171
  · env_data_mqtt.ts  → mqtt_sub.c:244
串口链路几乎瞬时到达，MQTT 链路要多走「ESP32 解析 + WiFi + broker 转发」，
通常晚 100~800 毫秒，跨秒后 ts 就差 1。所以按 ts 死磕相等会大面积误判。
本工具的做法是：**用数据内容做键，用时间窗做约束**。

三层验证
--------
  [1] raw 自校验（只用串口库）
      env_data 存了原始 24 字节。这里按 gw_frame.c 的规则重新解一遍，
      和入库字段逐条比对。目的是证明「表里的值 = 帧里的字节」，
      排除「解析器偏移写错、却因为一直错而看起来正常」这种最难发现的 bug。

  [2] 双链路交叉验证
      键 = (node, 温度x10, 湿度x10, 光照, 烟雾)，窗口默认 ±3 秒。
      pir / relay 不参与匹配（它们可能在远控瞬间变化），但匹配上之后
      要校验这两项是否一致 —— 不一致说明两条链路对同一帧解析出了不同结果。

  [3] 判重逻辑体检
      mqtt_sub.c 的去重条件是「node 与上一条相同 且 ts 与上一条相同」。
      当 1Hz 的两帧恰好落进同一个整秒时，第二帧会被误当成重复丢掉。
      本工具把「串口侧同一秒有 2 条、MQTT 侧只有 1 条」的时间点列出来，
      供人工确认。**只报告，不自动判定为 bug。**

用法
----
    # 最常用：先采样 5 分钟，用增量算帧率，再比对
    # （两个程序都要在跑，这条命令会自己等，不用你掐表）
    python3 tools/xcheck_dual.py --watch 300

    # 只比对现有数据，不采样
    python3 tools/xcheck_dual.py

    # 指定库路径 / 只看最近 500 条 / 放宽时间窗
    python3 tools/xcheck_dual.py -s gw.db -m env_mqtt.db -n 500 -w 5

    # 机器可读（给以后的自动化用）
    python3 tools/xcheck_dual.py --json

退出码
------
    0  全部一致
    1  有差异需要人工确认
    2  环境/参数问题（库不存在、表不存在、数据为空、时间窗不重叠）

注意
----
本脚本**只读**打开数据库（`mode=ro`），不会拿写锁，不会干扰正在运行的 ./gw，
也不会像一条 `sqlite3 gw.db` 手敲命令那样把库锁住导致 `database is locked`。
"""

import argparse
import json
import os
import sqlite3
import sys
import time
from collections import Counter, defaultdict, namedtuple

# ---------------------------------------------------------------- 常量

FRAME_LEN   = 24
FRAME_MAGIC = b"st:e"          # 73 74 3A 65

# 匹配键：这几项在同一帧里必须相同。
#   pir 必须进来 —— 环境静止时温度/湿度/光照/烟雾几乎不动（实测 lux 卡在 665、
#   gas 卡在 25 好几秒不动），只用这 5 项做键会得到几百条一模一样的 key，
#   配对算法直接失效。pir 至少能区分「有人 / 没人」，把重复度打下来。
#   relay 不进键：远控按下的瞬间它会变，进键会把正常帧判成不匹配。
KEY_FIELDS = ("node", "temp", "hum", "lux", "gas", "pir")

# 退一步的宽松键：用于区分「MQTT 真的没收到」和「收到了但内容不对」
LOOSE_FIELDS = ("node", "temp", "hum", "lux", "gas")

Row = namedtuple("Row", "rid ts node temp hum lux gas pir relay raw")

# ---------------------------------------------------------------- 输出小工具


class C(object):
    """ANSI 颜色。重定向到文件时自动关闭，避免日志里满屏转义符。"""
    on = sys.stdout.isatty()

    @classmethod
    def _w(cls, s, code):
        if not cls.on:
            return s
        return "\033[%sm%s\033[0m" % (code, s)

    @classmethod
    def ok(cls, s):
        return cls._w(s, "32")

    @classmethod
    def bad(cls, s):
        return cls._w(s, "31")

    @classmethod
    def warn(cls, s):
        return cls._w(s, "33")

    @classmethod
    def dim(cls, s):
        return cls._w(s, "90")

    @classmethod
    def bold(cls, s):
        return cls._w(s, "1")


def fmt_ts(ts):
    if not ts:
        return "-"
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(ts))


def hr(ch="=", n=74):
    print(ch * n)


def head(title):
    hr()
    print(C.bold(title))
    hr()


# ---------------------------------------------------------------- 读库


class DbError(Exception):
    pass


def _open_ro(path):
    """
    优先用只读方式打开（不拿写锁，绝不干扰正在跑的 ./gw）。

    但有个坑要处理：这两个库都开了 WAL（gw_db.c:108、mqtt_sub.c:144）。
    WAL 的只读连接要读 -wal 文件，就必须能打开 -shm；如果 -shm 恰好不存在
    （比如程序刚崩过、或者库是拷过来的），只读打开会直接失败。这种情况下
    回退到普通打开 —— WAL 的设计本来就允许读写并发，普通读连接同样不会
    阻塞 ./gw 写入，只是万一目录不可写时才可能出问题。
    """
    if not os.path.exists(path):
        raise DbError(
            "找不到数据库文件：%s\n"
            "      提示：本工具要在 Ubuntu 的 linux_gateway 目录下跑，\n"
            "            串口链路的库默认是 ./gw.db，MQTT 链路的库默认是 ./env_mqtt.db。\n"
            "            如果你的库在别处，用 -s / -m 指定完整路径。" % path)

    # 路径里含 ? 或 # 会破坏 file: URI 的语义，直接走普通打开
    if "?" in path or "#" in path:
        return sqlite3.connect(path, timeout=3.0)

    try:
        con = sqlite3.connect("file:%s?mode=ro" % path, uri=True, timeout=3.0)
        con.execute("SELECT 1")      # 真正触发一次 I/O，WAL 的问题会在这里暴露
        return con
    except sqlite3.Error as e:
        try:
            con = sqlite3.connect(path, timeout=3.0)
            con.execute("SELECT 1")
            print(C.warn("  ! 只读打开失败（%s），已改用普通方式读取。" % e))
            print(C.warn("    如果 ./gw 正在跑，这不会影响它 —— WAL 允许读写并发。"))
            return con
        except sqlite3.Error as e2:
            raise DbError("打开 %s 失败：%s" % (path, e2))


def _table_exists(con, table):
    cur = con.execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name=?", (table,))
    return cur.fetchone() is not None


def table_stat(path, table):
    """
    返回 (min_ts, max_ts, count)。

    为什么要先查这个：两条链路启动时刻不同（比如 mqtt_sub 是 21:52 才开的，
    而 gw 21:44 就在收了）。如果两边各取「最近 N 条」，取到的时间段根本不是
    同一段，比的就不是同一批帧 —— 会产生大量假的「一边有一边没有」。
    正确做法是先求时间交集，只比交集内的数据。
    """
    con = _open_ro(path)
    try:
        if not _table_exists(con, table):
            raise DbError(
                "%s 里没有表 %s。\n"
                "      提示：这张表是程序第一次跑起来时自动创建的。\n"
                "            如果文件刚建还没数据，先让对应程序跑一会儿再测。"
                % (path, table))
        r = con.execute("SELECT MIN(ts), MAX(ts), COUNT(*) FROM %s" % table).fetchone()
        if r is None or r[2] == 0:
            return (0, 0, 0)
        return (r[0], r[1], r[2])
    finally:
        con.close()


def load_rows(path, table, lo, hi, with_raw):
    """
    读 [lo, hi] 时间段内的记录，返回按 ts 升序、同一秒内按入库顺序排好的 Row 列表。
    """
    con = _open_ro(path)
    try:
        if not _table_exists(con, table):
            raise DbError(
                "%s 里没有表 %s。\n"
                "      提示：这张表是程序第一次跑起来时自动创建的。\n"
                "            如果文件刚建还没数据，先让对应程序跑一会儿再测。"
                % (path, table))

        cols = "id,ts,node,temp_x10,hum_x10,lux,gas,pir,relay"
        if with_raw:
            cols += ",raw"
        sql = ("SELECT %s FROM %s WHERE ts >= ? AND ts <= ? ORDER BY ts, id"
               % (cols, table))
        try:
            cur = con.execute(sql, (lo, hi))
        except sqlite3.Error as e:
            raise DbError("查询 %s 失败：%s" % (table, e))

        rows = []
        for r in cur.fetchall():
            rows.append(Row(
                rid=r[0], ts=r[1], node=r[2],
                temp=r[3], hum=r[4], lux=r[5], gas=r[6],
                pir=r[7], relay=r[8],
                raw=(r[9] if with_raw else None)))
        return rows
    finally:
        con.close()


# ---------------------------------------------------------------- [1] raw 自校验


def unpack_frame(raw):
    """
    按 gw_frame.c:89 gw_env_unpack() 的规则，从 24 字节原始帧里解出字段。

    偏移（全部小端）：
        0..3   帧头 'st:e'
        4      temp 小数位
        5      temp 整数位
        6      hum  小数位
        7      hum  整数位
        8      node
        9..11  x,y,z（int8）
        12..13 pir   (u16)
        14..15 lux   (u16)
        16..17 rsv2  (u16，未用)
        18..19 gas   (u16)
        20..21 relay (u16)
        22..23 adc   (u16)

    温度 x10 = 整数位*10 + 小数位（gw_frame.c:33）
    """
    if raw is None or len(raw) != FRAME_LEN:
        return None
    if raw[0:4] != FRAME_MAGIC:
        return None

    def u16(off):
        return raw[off] | (raw[off + 1] << 8)

    return {
        "temp": raw[5] * 10 + raw[4],
        "hum":  raw[7] * 10 + raw[6],
        "node": raw[8],
        "pir":  u16(12),
        "lux":  u16(14),
        "gas":  u16(18),
        "relay": u16(20),
    }


def check_raw(serial_rows):
    """
    把串口库里每条记录的 raw 重新解一遍，和入库字段比对。
    返回 (检查条数, 缺 raw 条数, 问题列表[(行, 字段, 入库值, 从他解出的值)])
    """
    checked = 0
    missing = 0
    problems = []

    for r in serial_rows:
        if not r.raw:
            missing += 1
            continue
        got = unpack_frame(r.raw)
        if got is None:
            problems.append((r, "raw", "帧头/长度非法",
                             r.raw[:8].hex() if r.raw else "-"))
            continue
        checked += 1
        for f in ("node", "temp", "hum", "lux", "gas", "pir", "relay"):
            if getattr(r, f) != got[f]:
                problems.append((r, f, getattr(r, f), got[f]))

    return checked, missing, problems


# ---------------------------------------------------------------- [2] 双链路匹配


def content_key(r):
    return tuple(getattr(r, f) for f in KEY_FIELDS)


def loose_key(r):
    return tuple(getattr(r, f) for f in LOOSE_FIELDS)


def _greedy_pair(subject, pool, keyf, window, used):
    """
    通用配对：对 subject 里每条，去 pool 中找「key 相同 + 未占用 + 窗口内 + ts 最近」的。

    返回 (pairs, left)，pairs 是 [(subject_row, pool_row, 延迟)]，left 是配不上的 subject_row。
    """
    idx = defaultdict(list)
    for i, r in enumerate(pool):
        idx[keyf(r)].append(i)

    pairs = []
    left = []
    for s in subject:
        cand = idx.get(keyf(s))
        best = None
        if cand:
            for midx in cand:
                if used[midx]:
                    continue
                d = abs(pool[midx].ts - s.ts)
                if d > window:
                    continue
                if best is None or d < best[0]:
                    best = (d, midx)
        if best is None:
            left.append(s)
        else:
            used[best[1]] = True
            m = pool[best[1]]
            pairs.append((s, m, m.ts - s.ts))
    return pairs, left


def match_dual(serial_rows, mqtt_rows, window):
    """
    两轮配对，把「配不上」拆成两种完全不同的情况：

      第一轮（严格）：key = node/temp/hum/lux/gas/**pir** 全等
                      → 配上的就是真正一致的帧

      第二轮（宽松）：第一轮剩下的，再用 node/temp/hum/lux/gas 去配
                      → 能配上说明「两边都收到了这一帧，但 pir 对不上」
                        = 某条链路解析出了不同的 pir，这是**真 bug**，
                        不能笼统算成「丢帧」

      剩下的才是真的「一边收到、另一边没收到」。

    这个拆分很重要：上一版把两种情况混在一起报成「MQTT 链路丢失 230 条」，
    实际大部分只是取数时间段不一致造成的假象。

    返回 (pairs, conflicts, only_serial, only_mqtt, stubbed)
        conflicts  [(串口行, mqtt行, 不一致的字段名)]
        stubbed    [串口行]  宽松键也配不上、但 MQTT 侧同一时刻有别的帧
                             → 「内容真的对不上」，最值得怀疑
    """
    used = [False] * len(mqtt_rows)

    # 第一轮：严格键
    pairs, left = _greedy_pair(serial_rows, mqtt_rows,
                               content_key, window, used)

    # 第二轮：宽松键（只在第一轮剩下的里面找）
    loose_pairs, still_left = _greedy_pair(left, mqtt_rows,
                                           loose_key, window, used)

    conflicts = []
    for s, m, d in loose_pairs:
        bad = [f for f in ("node", "temp", "hum", "lux", "gas", "pir", "relay")
               if getattr(s, f) != getattr(m, f)]
        conflicts.append((s, m, ",".join(bad)))

    only_serial = still_left
    only_mqtt = [mqtt_rows[i] for i in range(len(mqtt_rows)) if not used[i]]
    return pairs, conflicts, only_serial, only_mqtt


def find_same_second_loss(serial_rows, mqtt_rows):
    """
    找「串口侧同一整秒内有 >=2 条」的时间点，并看 MQTT 侧那一秒有几条。

    用来体检 mqtt_sub.c 的判重逻辑：
        if (node == s_last_node && ts == s_last_ts) { 丢弃 }
    它只和**上一条**比，所以 1Hz 的两帧一旦挤进同一整秒，
    第二帧就被当重复扔了。返回疑似点，供人工确认。
    """
    s_cnt = Counter((r.node, r.ts) for r in serial_rows)
    m_cnt = Counter((r.node, r.ts) for r in mqtt_rows)

    suspects = []
    for (node, ts), n in sorted(s_cnt.items(), key=lambda x: x[0][1]):
        if n < 2:
            continue
        # MQTT 侧可能因为跨秒延迟，落在 ts 或 ts+1
        got = m_cnt.get((node, ts), 0) + m_cnt.get((node, ts + 1), 0)
        if got < n:
            suspects.append({
                "node": node,
                "ts": ts,
                "serial_n": n,
                "mqtt_n": got,
            })
    return suspects


# ---------------------------------------------------------------- 报告


def watch_phase(args, s_stat, m_stat):
    """
    采样窗口：先记基线，等 N 秒再取一次，用**增量**算两边的帧率。

    为什么不能只看全库：库里可能掺着几小时前的、只跑了一半的数据。
    比如串口库从下午 18:50 就有数据，中间停过，那么「全库条数 / 全库时长」
    这个平均帧率就完全失真，会得出「串口在丢帧」的错误结论。
    增量只反映「刚刚这一段时间」，两边都在跑，比较才公平。

    gw 默认攒 20 条才 commit（GW_DB_DEFAULT_BATCH），未提交的事务
    只读连接看不到，所以窗口结束后要再等一会儿，否则会低估串口侧。
    """
    w = args.watch
    print()
    print(C.bold("  [0] 采样窗口 %d 秒（用增量算帧率，不受历史数据干扰）" % w))
    print("      基线：串口 %d 条 / MQTT %d 条" % (s_stat[2], m_stat[2]))

    sys.stdout.write("      采样中（每 30 秒一个点）")
    sys.stdout.flush()
    t_end = time.time() + w
    while True:
        left = t_end - time.time()
        if left <= 0:
            break
        time.sleep(min(30.0, left))
        sys.stdout.write(".")
        sys.stdout.flush()
    print("  完成")

    if args.settle > 0:
        sys.stdout.write("      等 %d 秒让最后一个事务提交（gw 默认攒 20 条才 commit）"
                         % args.settle)
        sys.stdout.flush()
        time.sleep(args.settle)
        print("  完成")

    try:
        s2 = table_stat(args.db_serial, "env_data")
        m2 = table_stat(args.db_mqtt, "env_data_mqtt")
    except DbError as e:
        print(C.bad("错误：") + str(e), file=sys.stderr)
        return s_stat, m_stat, 2

    ds = s2[2] - s_stat[2]
    dm = m2[2] - m_stat[2]
    print()
    print("      窗口内新增：串口 %d 条 = %.2f 帧/秒    MQTT %d 条 = %.2f 帧/秒"
          % (ds, float(ds) / w, dm, float(dm) / w))

    verdict = 0
    if ds <= 0 and dm <= 0:
        print(C.bad("      ✗ 这段时间两条链路都没有新数据，先解决「有没有数据」再谈比对。"))
        print("        串口侧：./gw 在跑吗？协调器在发吗？终端节点还入着网吗？")
        print("        MQTT 侧：./mqtt_sub 在跑吗？ESP32 串口里有「MQTT 已连接」吗？")
        verdict = 2
    elif dm <= 0:
        print(C.bad("      ✗ 串口在收（%d 条），MQTT 一条都没有 → MQTT 链路断了。" % ds))
        print("        看 ./mqtt_sub 的终端：还有没有每 5 秒打一次统计？")
        print("          还在打但「收到=」不涨 → ESP32 没发上来，去看 ESP32 串口。")
        print("          连统计都不打了         → mqtt_sub 已经退出，重开。")
        verdict = 1
    elif ds <= 0:
        print(C.bad("      ✗ MQTT 在收（%d 条），串口一条都没有 → 串口链路断了。" % dm))
        print("        看 ./gw 的终端：有效帧= 有没有在涨？")
        print("          在涨但库不涨 → 入库失败，看「库错=」。")
        print("          也不涨       → 设备侧没数据，查 /dev/ttyUSB0、协调器、飞线。")
        verdict = 1
    else:
        r = float(ds) / dm
        if 0.8 <= r <= 1.25:
            print(C.ok("      ✓ 两侧帧率接近（串口是 MQTT 的 %.0f%%），两条链路都正常。"
                       % (r * 100)))
        elif r < 0.8:
            print(C.warn("      ! 串口只有 MQTT 的 %.0f%% —— 这段时间串口侧在丢帧。"
                         % (r * 100)))
            print("        这不是比对算法的问题，是串口读取/入库跟不上的问题。")
            verdict = 1
        else:
            print(C.warn("      ! MQTT 只有串口的 %.0f%% —— 这段时间 WiFi/MQTT 侧在丢帧。"
                         % (100.0 / r)))
            print("        看 ESP32 串口的「丢弃=」、WiFi 信号强度、broker 负载。")
            verdict = 1
    sys.stdout.flush()
    return s2, m2, verdict


def report(args, s_stat, m_stat, srows, mrows, lo, hi):
    problems_total = 0
    span = hi - lo + 1
    gap_inner = []      # 交集内某个整分钟两边不齐
    gap_right = []      # 交集之后某条链路整段没数据（说明它中途死了）

    # 传进来的是「两库并集范围」的数据：[7] 要看到交集之外的部分，
    # 否则某条链路整体停掉时，交集窗口刚好把它截掉，空白就永远看不见。
    s_all, m_all = srows, mrows
    srows = [r for r in s_all if lo <= r.ts <= hi]
    mrows = [r for r in m_all if lo <= r.ts <= hi]

    # ---- 概览
    head("双链路交叉验证   env_data(串口)  vs  env_data_mqtt(MQTT)")

    print("  串口库  %s" % args.db_serial)
    print("          全库 %d 条，%s ~ %s"
          % (s_stat[2], fmt_ts(s_stat[0]), fmt_ts(s_stat[1])))
    print("  MQTT库  %s" % args.db_mqtt)
    print("          全库 %d 条，%s ~ %s"
          % (m_stat[2], fmt_ts(m_stat[0]), fmt_ts(m_stat[1])))
    print()
    print(C.bold("  只比交集时间段：%s ~ %s  （%d 秒）" % (fmt_ts(lo), fmt_ts(hi), span)))
    print("    串口 %d 条 = %.2f 帧/秒     MQTT %d 条 = %.2f 帧/秒"
          % (len(srows), float(len(srows)) / span,
             len(mrows), float(len(mrows)) / span))

    # 帧率差异是判断「哪条链路漏了」的第一手证据，先摆在最前面
    if len(srows) and len(mrows):
        ratio = float(len(srows)) / len(mrows)
        if ratio < 0.8:
            print()
            print(C.warn("  ! 串口侧帧数只有 MQTT 侧的 %.0f%% —— 串口链路收得比 MQTT 少。" % (ratio * 100)))
            print(C.warn("    同一个 P0_3 信号两路分叉读，理论上应该一样多，少的那边在丢。"))
            print(C.warn("    可能是：./gw 中途断过、串口读取跟不上、或者根本没在跑。"))
            print(C.warn("    先看 [7] 的每分钟对照，能直接分出是哪一种。"))
        elif ratio > 1.25:
            print()
            print(C.warn("  ! MQTT 侧帧数只有串口侧的 %.0f%% —— WiFi/MQTT 链路在漏帧。"
                         % (100.0 / ratio)))
            print(C.warn("    看 ESP32 串口里的「丢弃=」和 WiFi 信号强度。"))

    if len(srows) < 10 or len(mrows) < 10:
        print()
        print(C.warn("  ! 样本太少（串口 %d 条 / MQTT %d 条），结论不可靠。"
                     % (len(srows), len(mrows))))
        print("    建议让两条链路同时跑 2 分钟以上（约 120 帧）再测。")

    # ---- [1] raw 自校验
    print()
    print(C.bold("[1] 原始字节自校验（证明「表里的值 = 帧里的字节」）"))
    checked, missing, rproblems = check_raw(srows)
    if checked == 0 and missing > 0:
        print("  %-22s %s" % ("无法校验", C.warn("串口库里没有 raw 列内容")))
    else:
        if rproblems:
            problems_total += len(rproblems)
            print("  %-22s %s" % ("解析不一致", C.bad("%d 处" % len(rproblems))))
            for r, f, stored, got in rproblems[:10]:
                print("    %s id=%d  %s  入库=%s  从raw解出=%s" % (
                    fmt_ts(r.ts), r.rid, f, stored, got))
        else:
            print("  %-22s %s" % ("解析一致", C.ok("%d/%d 条全部吻合" % (checked, checked))))
        if missing:
            print("  %-22s %d 条（早期记录没存 raw，跳过）" % ("缺 raw", missing))

    # ---- [2] 双链路匹配
    print()
    print(C.bold("[2] 双链路内容比对（严格键=%s，窗口 ±%d 秒）"
                 % ("/".join(KEY_FIELDS), args.window)))
    pairs, conflicts, only_s, only_m = match_dual(srows, mrows, args.window)

    base = len(srows)
    pct = (100.0 * len(pairs) / base) if base else 0.0

    if not pairs and not conflicts:
        print("  %-22s %s" % ("能对上的记录", C.bad("0 条")))
        print()
        print("  一条都对不上，通常是这几种原因：")
        print("    · ESP32 组 JSON 时字段串位了（对比两边最新一条的原始值：")
        if srows:
            r = srows[-1]
            print("        串口   node=%d 温度=%d 湿度=%d 光照=%d 烟雾=%d pir=%d"
                  % (r.node, r.temp, r.hum, r.lux, r.gas, r.pir))
        if mrows:
            r = mrows[-1]
            print("        MQTT   node=%d 温度=%d 湿度=%d 光照=%d 烟雾=%d pir=%d"
                  % (r.node, r.temp, r.hum, r.lux, r.gas, r.pir))
        print("      如果两行的数值互相错位（比如温度等于湿度），就是 JSON 字段映射错了。")
    else:
        print("  %-22s %s" % ("完全一致", C.ok("%d 条  %.1f%%" % (len(pairs), pct))))
        if conflicts:
            problems_total += len(conflicts)
            print("  %-22s %s" % ("内容冲突", C.bad("%d 条" % len(conflicts))
                                  + C.dim("   两边都收到，但字段值不同 → 解析 bug")))
        else:
            print("  %-22s %s" % ("内容冲突", C.ok("0 条")))
        col = C.ok if not only_s else C.warn
        print("  %-22s %s" % ("串口有 / MQTT 无", col("%d 条" % len(only_s))
                              + C.dim("   MQTT 链路真的没收到")))
        col = C.ok if not only_m else C.warn
        print("  %-22s %s" % ("MQTT 有 / 串口无", col("%d 条" % len(only_m))
                              + C.dim("   串口链路漏帧，或 QoS1 重复发布")))

    # 延迟分布
    if pairs:
        print()
        print(C.bold("[3] 传输延迟（MQTT 收到时刻 − 串口收到时刻）"))
        delays = Counter(d for (_, _, d) in pairs)
        total = len(pairs)
        for d in sorted(delays):
            n = delays[d]
            bar = "#" * max(1, int(40.0 * n / total))
            print("    %+2d 秒  %4d 条 (%5.1f%%)  %s"
                  % (d, n, 100.0 * n / total, C.dim(bar)))
        if max(delays) >= 2:
            print("    " + C.warn("! 有超过 2 秒的延迟，检查 WiFi 信号强度和 broker 负载。"))

    # ---- [4] 内容冲突明细
    print()
    print(C.bold("[4] 内容冲突（两边都收到了这一帧，但解析出的值不同）"))
    if not conflicts:
        print("  %-22s %s" % ("字段冲突", C.ok("无 —— 同一帧在两条链路上解出的值完全一致")))
    else:
        for s, m, fields in conflicts[:12]:
            print("  不一致字段：%s" % C.bad(fields))
            print("    串口 %s id=%-5d node=%d T=%d.%d H=%d.%d lux=%d gas=%d pir=%d relay=%d"
                  % (fmt_ts(s.ts), s.rid, s.node, s.temp // 10, s.temp % 10,
                     s.hum // 10, s.hum % 10, s.lux, s.gas, s.pir, s.relay))
            print("    MQTT %s id=%-5d node=%d T=%d.%d H=%d.%d lux=%d gas=%d pir=%d relay=%d"
                  % (fmt_ts(m.ts), m.rid, m.node, m.temp // 10, m.temp % 10,
                     m.hum // 10, m.hum % 10, m.lux, m.gas, m.pir, m.relay))
        if len(conflicts) > 12:
            print("  …… 还有 %d 条，用 --json 看全部" % (len(conflicts) - 12))

    # ---- [5] 判重逻辑体检
    print()
    print(C.bold("[5] 判重逻辑体检（mqtt_sub.c 的「与上一条比」）"))
    suspects = find_same_second_loss(srows, mrows)
    if not suspects:
        print("  %-22s %s" % ("同一秒多帧误丢", C.ok("未发现")))
    else:
        problems_total += len(suspects)
        print("  %-22s %s" % ("疑似同一秒误丢", C.warn("%d 处" % len(suspects))))
        for sp in suspects[:10]:
            print("    %s  node=%d  串口 %d 条 / MQTT 侧 %d 条"
                  % (fmt_ts(sp["ts"]), sp["node"], sp["serial_n"], sp["mqtt_n"]))
        print()
        print("  " + C.dim("说明：mqtt_sub.c 用「node 与上一条相同 且 ts 相同」判断重复，"))
        print("  " + C.dim("      只和上一条比。1Hz 的两帧挤进同一整秒时，第二帧会被误丢。"))
        print("  " + C.dim("      这只说明现象，是否要改判重逻辑由你决定。"))

    # ---- 明细
    if only_s or only_m:
        print()
        print(C.bold("[6] 差异明细（最多各列 10 条）"))
        for r in only_s[:10]:
            print("  只在串口  %s  node=%d  T=%d.%d H=%d.%d lux=%d gas=%d pir=%d relay=%d"
                  % (fmt_ts(r.ts), r.node, r.temp // 10, r.temp % 10,
                     r.hum // 10, r.hum % 10, r.lux, r.gas, r.pir, r.relay))
        for r in only_m[:10]:
            print("  只在MQTT  %s  node=%d  T=%d.%d H=%d.%d lux=%d gas=%d pir=%d relay=%d"
                  % (fmt_ts(r.ts), r.node, r.temp // 10, r.temp % 10,
                     r.hum // 10, r.hum % 10, r.lux, r.gas, r.pir, r.relay))

    # ---- 每分钟分布：区分「整段没数据」和「匀速少一半」
    #
    # 只看总数是分不清这两种情况的：
    #   A. gw 中途停了 10 分钟  → 总数少 600，但每分钟都是满的
    #   B. 全程匀速只收到一半  → 总数也少 600，但每分钟都少一半
    # 处理方式完全不同（A 查进程/重连，B 查读取性能），所以必须按分钟看。
    if s_all and m_all:
        print()
        print(C.bold("[7] 每分钟帧数对照（分辨「整段没数据」还是「匀速少一半」）"))
        sh = Counter(r.ts // 60 for r in s_all)
        mh = Counter(r.ts // 60 for r in m_all)
        allm = sorted(set(sh) | set(mh))
        print("  %-8s %6s %6s %8s" % ("时间", "串口", "MQTT", "差值"))
        for m in allm:
            s_c, m_c = sh.get(m, 0), mh.get(m, 0)
            d = s_c - m_c
            line = "  %-8s %6d %6d %8s" % (
                time.strftime("%H:%M", time.localtime(m * 60)), s_c, m_c,
                ("%+d" % d) if d else "0")
            mark = ""
            if m_c >= 10 and s_c == 0:
                # 串口整分钟空白，但 MQTT 有：串口进程没在收
                mark = C.bad("  ← 串口整分钟空白（进程停了/串口断了）")
            elif s_c >= 10 and m_c == 0:
                mark = C.bad("  ← MQTT 整分钟空白（订阅断了/ESP32 没发）")
            elif m_c >= 10 and s_c * 2 < m_c:
                mark = C.warn("  ← 串口只收到一半（读取跟不上）")
            elif m * 60 > hi or m * 60 + 59 < lo:
                mark = C.dim("  （这条分钟在交集之外，不参与比对）")
            print(line + mark)

        # 空白判定要覆盖「并集」全部分钟，不能只看交集：
        # 某条链路整体停掉时，交集刚好把它截掉，只看交集的写法会漏掉最关键的线索。
        # 但要区分位置——停在交集右侧 = 这条链路中途死了；左侧 = 只是起步时间不同。
        def _blank_minutes(cnt_this, cnt_other):
            return [m for m in allm
                    if cnt_other.get(m, 0) >= 10 and cnt_this.get(m, 0) == 0]

        def _say(name, blanks, why):
            if not blanks:
                return
            inner = [m for m in blanks if m * 60 + 59 >= lo and m * 60 <= hi]
            right = [m for m in blanks if m * 60 > hi]
            left = [m for m in blanks if m * 60 + 59 < lo]
            print(C.warn("  %s 有 %d 个整分钟完全没数据" % (name, len(blanks))))
            if inner:
                print(C.bad("    · 交集内 %d 分钟空白（例 %s）→ 这是真断线，%s"
                            % (len(inner),
                               time.strftime("%H:%M", time.localtime(inner[0] * 60)),
                               why)))
            if right:
                print(C.bad("    · 交集之后 %d 分钟空白（从 %s 起）→ %s 在这之后就没有数据了"
                            % (len(right),
                               time.strftime("%H:%M", time.localtime(right[0] * 60)),
                               name)))
            if left:
                print(C.dim("    · 交集之前 %d 分钟空白（到 %s 止）→ 只是这条链路启动得晚"
                            % (len(left),
                               time.strftime("%H:%M", time.localtime(left[-1] * 60)))))

        print()
        s_blanks = _blank_minutes(sh, mh)
        m_blanks = _blank_minutes(mh, sh)
        _say("串口", s_blanks, "查 ./gw 是不是退出了、串口是不是掉了")
        _say("MQTT", m_blanks, "查 ./mqtt_sub 是不是退出了、订阅是不是断了")

        # 有整段空白时，就不再谈「匀速少」—— 那会把真正的断线说成性能问题
        all_blanks = s_blanks + m_blanks
        gap_inner = [m for m in all_blanks if m * 60 + 59 >= lo and m * 60 <= hi]
        gap_right = [m for m in all_blanks if m * 60 > hi]
        in_m = [m for m in allm if m * 60 + 59 >= lo and m * 60 <= hi]
        s_blank = [m for m in in_m if mh.get(m, 0) >= 10 and sh.get(m, 0) == 0]
        m_blank = [m for m in in_m if sh.get(m, 0) >= 10 and mh.get(m, 0) == 0]
        if not all_blanks:
            partial = [m for m in in_m
                       if mh.get(m, 0) >= 10 and sh.get(m, 0) < mh.get(m, 0) * 0.8]
            if partial:
                print(C.warn("  交集内每分钟都有收到，但串口普遍少于 MQTT 的分钟数：%d" % len(partial)))
                print("    → 这是**匀速少**，不是断线：串口读取跟不上，查读取性能/入库耗时。")
            else:
                print(C.ok("  交集内每分钟都对齐，没有整段空白。"))

    # ---- 结论
    print()
    hr()
    if problems_total:
        print("  问题合计：%d 处（解析 %d / 冲突 %d / 同秒误丢 %d）"
              % (problems_total, len(rproblems), len(conflicts), len(suspects)))
        print()
    if not pairs and not conflicts:
        print(C.bad("结论：未能比对 —— 见上面 [2] 的原因分析。"))
    elif conflicts:
        print(C.bad("结论：不通过 —— 有 %d 条同一帧在两链路解出的值不同，" % len(conflicts)))
        print(C.bad("      这是解析层的 bug，必须查（见 [4]）。"))
    elif only_s or only_m:
        print(C.ok("结论：解析一致 —— 能对上的 %d 条，字段分毫不差。" % len(pairs)))
        print(C.warn("      但两边帧数不等（串口独有 %d 条 / MQTT 独有 %d 条），"
                     % (len(only_s), len(only_m))))
        print(C.warn("      说明有一条链路在丢帧，要单独查那一条（见 [7] 的每分钟对照）。"))
    elif gap_right or gap_inner:
        print(C.ok("结论：交集内解析一致 —— 能对上的 %d 条，字段分毫不差。" % len(pairs)))
        print(C.warn("      但有 %d 个整分钟某条链路完全没数据（见 [7] 标红那几行），"
                     % (len(gap_right) + len(gap_inner))))
        print(C.warn("      那是链路中途断掉，不是解析问题，先查断在哪一端。"))
    else:
        print(C.ok("结论：完全通过 —— 两条链路收的帧数相同、内容逐条一致。"))
        print(C.ok("      协调器 P0_3 之后分叉的整条链路（串口解析 / ESP32 解析 → JSON"))
        print(C.ok("      → MQTT 传输 → 落库）没有把任何一个字节搞错。"))
    hr()

    return 0 if (not conflicts and not only_s and not only_m
                 and not gap_inner and not gap_right) else 1


def report_json(args, s_stat, m_stat, srows, mrows, lo, hi):
    span = hi - lo + 1
    # 传进来的是并集数据，比对只用交集（理由见 report()）
    srows = [r for r in srows if lo <= r.ts <= hi]
    mrows = [r for r in mrows if lo <= r.ts <= hi]
    checked, missing, rproblems = check_raw(srows)
    pairs, conflicts, only_s, only_m = match_dual(srows, mrows, args.window)
    suspects = find_same_second_loss(srows, mrows)

    out = {
        "serial_db": args.db_serial,
        "mqtt_db": args.db_mqtt,
        "window": args.window,
        "compare_range": {"lo": lo, "hi": hi, "seconds": span},
        "serial": {
            "count_total": s_stat[2],
            "count_in_range": len(srows),
            "fps": round(float(len(srows)) / span, 3) if span else 0,
            "first_ts": s_stat[0],
            "last_ts": s_stat[1],
        },
        "mqtt": {
            "count_total": m_stat[2],
            "count_in_range": len(mrows),
            "fps": round(float(len(mrows)) / span, 3) if span else 0,
            "first_ts": m_stat[0],
            "last_ts": m_stat[1],
        },
        "raw_check": {
            "checked": checked,
            "missing_raw": missing,
            "problems": len(rproblems),
        },
        "match": {
            "paired": len(pairs),
            "match_pct": round(100.0 * len(pairs) / len(srows), 2) if srows else 0.0,
            "conflicts": len(conflicts),
            "conflict_fields": [f for (_, _, f) in conflicts],
            "only_serial": len(only_s),
            "only_mqtt": len(only_m),
            "delay_hist": dict((str(k), v) for k, v in
                               Counter(d for (_, _, d) in pairs).items()),
        },
        "same_second_suspects": suspects,
    }
    print(json.dumps(out, ensure_ascii=False, indent=2, sort_keys=True))

    bad = len(rproblems) + len(conflicts) + len(suspects)
    ok = (len(conflicts) == 0 and not only_s and not only_m)
    return 0 if ok and bad == 0 else 1


# ---------------------------------------------------------------- main


def main():
    ap = argparse.ArgumentParser(
        description="双链路交叉验证：env_data(串口) vs env_data_mqtt(MQTT)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("用法\n----")[-1])
    ap.add_argument("-s", "--serial-db", dest="db_serial", default="./gw.db",
                    help="串口链路的库（默认 ./gw.db）")
    ap.add_argument("-m", "--mqtt-db", dest="db_mqtt", default="./env_mqtt.db",
                    help="MQTT 链路的库（默认 ./env_mqtt.db）")
    ap.add_argument("-n", "--limit", type=int, default=20000,
                    help="交集内每个库最多参与比对多少条（默认 20000，等于不限）")
    ap.add_argument("-w", "--window", type=int, default=3,
                    help="时间窗 ±N 秒（默认 3；手机热点抖动大时可以放到 5）")
    ap.add_argument("--watch", type=int, default=0, metavar="秒",
                    help="先采样 N 秒，用增量算两边帧率再比对（推荐 300）")
    ap.add_argument("--settle", type=int, default=-1, metavar="秒",
                    help="采样结束后再等几秒让事务提交（默认 25，配合 --watch）")
    ap.add_argument("--json", action="store_true", help="输出 JSON 而不是中文报告")
    args = ap.parse_args()

    if args.limit < 1:
        print("-n 必须 >= 1", file=sys.stderr)
        return 2
    if args.watch < 0:
        print("--watch 必须 >= 0", file=sys.stderr)
        return 2
    if args.watch > 0 and args.settle < 0:
        args.settle = 25

    # 第一步：先查两个库各自的时间范围，求出交集
    try:
        s_stat = table_stat(args.db_serial, "env_data")
        m_stat = table_stat(args.db_mqtt, "env_data_mqtt")
    except DbError as e:
        print(C.bad("错误：") + str(e), file=sys.stderr)
        return 2

    # 可选的采样窗口：先量「刚刚这段时间」两边各收了多少，再谈比对。
    # 放在空表检查之前，因为「MQTT 没数据」正是最需要它来定位的情况。
    watched = False
    if args.watch > 0:
        s_stat, m_stat, wv = watch_phase(args, s_stat, m_stat)
        watched = True
        # 采样已经说明了谁断了，就别再重复报一遍通用错误，
        # 否则两段话交错在一起，看着像出了两个毛病
        if s_stat[2] == 0 or m_stat[2] == 0:
            print(C.dim("      库里没有可比对的数据，先解决上面那个问题，再回来跑这条命令。"))
            return 2

    if not watched and s_stat[2] == 0:
        print(C.bad("错误：") + "串口库 env_data 里没有数据。", file=sys.stderr)
        print("      先确认 ./gw -d /dev/ttyUSB0 在跑，并且协调器在往串口发帧。",
              file=sys.stderr)
        return 2
    if m_stat[2] == 0:
        print(C.bad("错误：") + "MQTT 库 env_data_mqtt 里没有数据。", file=sys.stderr)
        print("      env_data_mqtt 是 ./mqtt_sub 写的，和 ./gw 是两个程序。", file=sys.stderr)
        print("      先另开一个终端跑：./mqtt_sub", file=sys.stderr)
        print("      （默认连 127.0.0.1:1883，订阅 zigbee/+/env）", file=sys.stderr)
        print("      同时确认 ESP32 那边串口日志里有「MQTT 已连接」。", file=sys.stderr)
        return 2

    lo = max(s_stat[0], m_stat[0])
    hi = min(s_stat[1], m_stat[1])

    if lo > hi:
        print(C.bad("  ✗ 两个库的时间范围没有交集，没法比对。"))
        print("    串口库   %s ~ %s   （%d 条）"
              % (fmt_ts(s_stat[0]), fmt_ts(s_stat[1]), s_stat[2]))
        print("    MQTT库   %s ~ %s   （%d 条）"
              % (fmt_ts(m_stat[0]), fmt_ts(m_stat[1]), m_stat[2]))
        print()
        print("  原因通常是：这两条链路不是同时跑的（比如 mqtt_sub 是早上开的，")
        print("  串口是刚开的）。交叉验证必须让两条链路**同时**收同一批帧。")
        print("  做法：两个终端分别跑")
        print("          ./gw -d /dev/ttyUSB0 -f gw.db -p 8080")
        print("          ./mqtt_sub")
        print("        等 2 分钟（约 120 帧）再测。")
        return 2

    # 第二步：取「并集」范围的数据。
    # 为什么不用交集：某条链路整段停掉时，交集窗口刚好把它截掉，
    # 那段空白就永远看不见 —— 而这恰恰是最需要看见的东西。
    # 比对时再在 report() 里按交集切片。
    ulo = min(s_stat[0], m_stat[0])
    uhi = max(s_stat[1], m_stat[1])
    try:
        srows = load_rows(args.db_serial, "env_data", ulo, uhi, with_raw=True)
        mrows = load_rows(args.db_mqtt, "env_data_mqtt", ulo, uhi, with_raw=False)
    except DbError as e:
        print(C.bad("错误：") + str(e), file=sys.stderr)
        return 2

    # -n 现在的作用是「交集内最多算多少条」，防极端情况下跑太久
    if args.limit and len(srows) > args.limit:
        srows = srows[-args.limit:]
    if args.limit and len(mrows) > args.limit:
        mrows = mrows[-args.limit:]
    if srows:
        lo = max(lo, srows[0].ts)
    if mrows:
        lo = max(lo, mrows[0].ts)

    if args.json:
        return report_json(args, s_stat, m_stat, srows, mrows, lo, hi)
    return report(args, s_stat, m_stat, srows, mrows, lo, hi)


if __name__ == "__main__":
    sys.exit(main())
