#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
serial_probe.py —— 绕开 gw，直接量串口原始字节流

为什么需要它
------------
双链路比对发现：串口侧只拿到 MQTT 侧的 1/3，但收到的每一帧字段都对
（raw 自校验 100%、内容冲突 0）。所以问题不在解析，在「字节有没有到」。
这一步要分清两件事：

    A. 字节根本没到 gw 手里（CH340 / USB 线 / 供电 / 协调器 侧丢的）
    B. 字节到了，但被 gw 的读循环/入库吞掉了

做法：把 gw 停掉，用本脚本独占 /dev/ttyUSB0，按同一套字段规则数帧，
同时去 env_mqtt.db 数「同一时间段」MQTT 侧新增了多少条。两边一比就定性。

用法
----
    # 先 Ctrl+C 停掉 ./gw（端口会被独占，网页打不开是正常的）
    python3 tools/serial_probe.py                  # 探 60 秒
    python3 tools/serial_probe.py -t 120            # 探 120 秒
    python3 tools/serial_probe.py -d /dev/ttyUSB1   # 串口号变了
    python3 tools/serial_probe.py --selftest        # 不开串口，只测解析逻辑

两轮对照法（这是本脚本的正确用法，一轮说明不了问题）
------------------------------------------------------
脚本自己会打一行「本次串口帧率 X.XX 帧/秒」，你要做的就是把这个数比两次：

    A 轮（只留串口）  停 ./mqtt_sub；把 ESP32 的 USB 拔掉（飞线不用拆）
                      python3 tools/serial_probe.py -t 60

    B 轮（全开）      ESP32 插回、./mqtt_sub 跑起来
                      python3 tools/serial_probe.py -t 60

    A 满速、B 明显掉  → ESP32 在干扰串口链路（射频 / 供电 / 地环路）
    A 也照样掉        → 与 ESP32 无关，问题在 协调器 → CH340 → USB 这一段
    两轮都满速         → 刚才那次是偶发，跑 -t 300 看复现率

A 轮里 MQTT 没有数据是**正常的**，脚本不会因此退出，仍会打出串口侧结论。

注意
----
只读打开串口，不写任何东西，不碰数据库（env_mqtt.db 用 mode=ro 读）。
"""
import argparse
import os
import select
import sqlite3
import sys
import time

GW_ENV_FRAME_LEN = 24
HEAD = b"st:e"
BAUD = 115200


def u16le(b, i):
    return b[i] | (b[i + 1] << 8)


def sane(fr):
    """和 gw_frame.c 的 gw_env_sane 保持同一套判据。"""
    if fr[8] == 0:                       # node 不能是 0
        return False
    if u16le(fr, 12) > 1:                # pir ∈ {0,1}
        return False
    if u16le(fr, 20) > 1:                # relay ∈ {0,1}
        return False
    if u16le(fr, 14) > 1023:             # lux <= 1023
        return False
    if u16le(fr, 18) > 1023:             # gas <= 1023
        return False
    return True


class Counter(object):
    def __init__(self):
        self.buf = bytearray()
        self.bytes = 0
        self.frames = 0
        self.resync = 0          # 为了重新对齐丢掉的字节数
        self.bad = 0             # 头对、字段不合法的次数
        self.sec = {}            # 秒 -> 帧数
        self.bsec = {}           # 秒 -> 收到的字节数（含无法成帧的）
        self.times = []          # 每一帧解出来的时刻

    def feed(self, data, now):
        self.bytes += len(data)
        k0 = int(now)
        self.bsec[k0] = self.bsec.get(k0, 0) + len(data)
        self.buf.extend(data)
        while True:
            if len(self.buf) < 4:
                return
            pos = self.buf.find(HEAD)
            if pos < 0:
                # 保住最后 3 字节，避免把跨包的帧头劈开
                keep = 3
                drop = len(self.buf) - keep
                if drop > 0:
                    del self.buf[:drop]
                    self.resync += drop
                return
            if pos > 0:
                del self.buf[:pos]
                self.resync += pos
            if len(self.buf) < GW_ENV_FRAME_LEN:
                return
            fr = bytes(self.buf[:GW_ENV_FRAME_LEN])
            if not sane(fr):
                self.bad += 1
                del self.buf[:1]          # 只丢 1 字节，继续找下一个头
                continue
            del self.buf[:GW_ENV_FRAME_LEN]
            self.frames += 1
            self.times.append(now)
            k = int(now)
            self.sec[k] = self.sec.get(k, 0) + 1


def open_port(path):
    # termios 只有 Linux 有，放到函数里 import，
    # 这样 --selftest 在 Windows 上也能跑（方便离线验证解析逻辑）
    import termios
    baud = getattr(termios, "B%d" % BAUD)
    fd = os.open(path, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    attr = termios.tcgetattr(fd)
    attr[0] = 0                    # iflag：关掉所有输入处理（不做 CR/LF 转换）
    attr[1] = 0                    # oflag
    attr[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attr[3] = 0                    # lflag：非规范模式
    attr[4] = baud
    attr[5] = baud
    attr[6][termios.VMIN] = 0
    attr[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attr)
    termios.tcflush(fd, termios.TCIFLUSH)
    return fd


def mqtt_count(db, t0, t1):
    """数 env_mqtt.db 在 [t0, t1] 新增了多少条。打不开就返回 None。"""
    if not os.path.exists(db):
        return None
    try:
        con = sqlite3.connect("file:%s?mode=ro" % db, uri=True, timeout=3.0)
    except sqlite3.Error:
        try:
            con = sqlite3.connect(db, timeout=3.0)
        except sqlite3.Error:
            return None
    try:
        cur = con.execute(
            "SELECT COUNT(*) FROM env_data_mqtt WHERE ts >= ? AND ts <= ?",
            (int(t0), int(t1)))
        return cur.fetchone()[0]
    except sqlite3.Error:
        return None
    finally:
        con.close()


def probe(path, seconds, db, tick=10):
    fd = open_port(path)
    c = Counter()
    print("串口 %s 已打开（115200 8N1，只读），探测 %d 秒" % (path, seconds))
    print("这段时间请让 ./mqtt_sub 继续跑，并且 ESP32 保持连着 broker")
    print()
    t0 = time.time()
    t_end = t0 + seconds
    next_tick = t0 + tick
    last_frames = 0
    lost_fd = False      # 设备从 USB 上掉下去了

    while True:
        now = time.time()
        if now >= t_end:
            break
        try:
            r, _, _ = select.select([fd], [], [], 0.2)
        except (OSError, ValueError):
            lost_fd = True
            break
        if r:
            try:
                data = os.read(fd, 4096)
            except OSError:
                # ENODEV/EIO：CH340 掉线了。这个 fd 再也读不出东西，必须退出
                lost_fd = True
                break
            if data:
                c.feed(data, now)
        if now >= next_tick:
            d = c.frames - last_frames
            last_frames = c.frames
            print("  [%3ds] 累计帧=%4d  本段 %d 帧 = %.2f 帧/秒  坏=%d  重同步=%dB"
                  % (int(now - t0), c.frames, d, float(d) / tick,
                     c.bad, c.resync))
            next_tick = now + tick

    t1 = time.time()
    try:
        os.close(fd)
    except OSError:
        pass
    span = max(1.0, t1 - t0)

    print()
    print("=" * 72)
    print("  探测时长        %.0f 秒" % span)
    if lost_fd:
        print("  设备掉线        ！第 %.0f 秒 CH340 从 USB 上掉下去了，本轮提前结束"
              % span)
        print("                  这时的帧率一定偏低，不能直接跟另一轮比。")
        print("                  立刻查：dmesg | grep -iE \"ch341|ttyUSB\" | tail -20")
        print("                  出现 disconnected / usb_submit_urb failed 就是硬件问题。")
    print("  串口收到字节    %d" % c.bytes)
    print("  串口解出帧      %d 条 = %.2f 帧/秒" % (c.frames, c.frames / span))
    print("  坏帧/重同步     %d 次 / %d 字节" % (c.bad, c.resync))

    # 每秒帧数分布：能看出是「匀速少」还是「成批来」
    if c.sec:
        counts = [c.sec[k] for k in sorted(c.sec)]
        empty = 0
        for i in range(int(t0) + 1, int(t1)):
            if i not in c.sec:
                empty += 1
        print("  有帧的秒数      %d / %d（空秒 %d）"
              % (len(counts), int(t1) - int(t0), empty))
        print("  单秒最多/最少   %d / %d 帧" % (max(counts), min(counts)))

    # 关键分辨：是「一个字节都没到」还是「字节到了但解不出帧」
    n_sec = max(1, int(t1) - int(t0))
    sec_with_bytes = len([k for k in c.bsec if c.bsec[k] > 0])
    sec_with_frame = len(c.sec)
    print("  有字节的秒数    %d / %d" % (sec_with_bytes, n_sec))
    if sec_with_bytes > sec_with_frame:
        print("                  ↑ 这 %d 秒收到了字节但一帧都没解出来"
              % (sec_with_bytes - sec_with_frame))
    print("  平均每帧消耗    %.1f 字节（干净时应等于 24）"
          % (float(c.bytes) / c.frames if c.frames else 0.0))

    # 时间线：一眼看出是「整段静默」还是「一直稀稀拉拉」
    if c.sec or c.bsec:
        line = []
        for i in range(int(t0), int(t1) + 1):
            nf = c.sec.get(i, 0)
            nb = c.bsec.get(i, 0)
            if nf >= 2:
                line.append("*")
            elif nf == 1:
                line.append("#")
            elif nb > 0:
                line.append("-")     # 有字节但没成帧
            else:
                line.append(".")     # 一个字节都没有
        s = "".join(line)
        print()
        print("  时间线（1 秒 1 格）")
        for i in range(0, len(s), 60):
            print("    %s" % s[i:i + 60])
        print("    # = 1 帧   * = 2 帧以上   - = 有字节没成帧   . = 无任何字节")

    # 帧间隔：中位数说明常态节奏，最长间隔说明有没有「洞」
    gaps = []
    mid = mx = None
    over = 0
    if len(c.times) >= 3:
        gaps = [c.times[i + 1] - c.times[i] for i in range(len(c.times) - 1)]
        gs = sorted(gaps)
        mid = gs[len(gs) // 2]
        mx = gs[-1]
        over = len([g for g in gaps if g > 3.0])
        print()
        print("  帧间隔中位数    %.2f 秒（常态节奏）" % mid)
        print("  最长帧间隔      %.1f 秒%s"
              % (mx, "   ← 这里断了这么久" if mx > 3.0 else ""))
        if over:
            print("  超过 3 秒的空洞 %d 处" % over)

    # ---------------- 以下结论：MQTT 在不在都能出 ----------------
    sfps = c.frames / span
    per_frame = (float(c.bytes) / c.frames) if c.frames else 0.0
    dirty = (c.bad + c.resync / float(GW_ENV_FRAME_LEN)) / float(c.frames or 1)
    clean = per_frame < 30 and dirty < 0.15

    n_mqtt = mqtt_count(db, t0, t1)
    mfps = None
    print()
    if n_mqtt is None:
        print("  MQTT 对照       跳过（env_mqtt.db 打不开或没这张表）")
    else:
        mfps = n_mqtt / span
        print("  MQTT 同期新增   %d 条 = %.2f 帧/秒" % (n_mqtt, mfps))
        if mfps <= 0:
            print("                  ↑ 这段时间没增长，本轮只出串口侧结论")
            mfps = None
        else:
            print("  帧率比          串口 / MQTT = %.0f%%" % (sfps / mfps * 100))

    print()
    print("=" * 72)

    # 1) 字节干不干净：决定查「供给」还是查「信号」
    if clean:
        print("串口侧形态      字节干净 —— 平均每帧 %.1f 字节（干净时应为 24），" % per_frame)
        print("                坏帧 %d 次、重同步 %d 字节，都在噪声级别。"
              % (c.bad, c.resync))
        print("                不是「信号被打歪」，是整帧整帧地没送到。")
    else:
        print("串口侧形态      字节不干净 —— 平均每帧 %.1f 字节（干净时应为 24），" % per_frame)
        if dirty:
            print("                坏帧 %d 次、重同步 %d 字节，约每 %.1f 帧坏一次。"
                  % (c.bad, c.resync, 1.0 / dirty))
        print("                这是信号质量/干扰问题，方向是飞线、屏蔽、地线。")

    # 2) 是整段断还是匀速少
    if mx is not None and mx > 3.0:
        print("                最长一次断了 %.1f 秒（超 3 秒空洞 %d 处）→ 整段中断。"
              % (mx, over))
        print("                整段中断的第一嫌疑是 CH340 掉了 USB，用这条命令确认：")
        print("                  dmesg | grep -iE \"ch341|ttyUSB\" | tail -30")
    elif c.frames:
        print("                没有超过 3 秒的空洞 → 一直稀稀拉拉地少，不是整段断。")

    # 3) 这一轮的帧率（要跟另一轮比的就是这个数）
    print()
    print("  本次串口帧率    %.2f 帧/秒        ← 记住这个数，下一轮要跟它比" % sfps)
    print("  满速参考        约 1.0~1.2 帧/秒（只有串口在收时实测）")
    if sfps >= 0.9:
        print("                  → 这一轮串口是满速的。")
    elif sfps >= 0.7:
        print("                  → 这一轮略低，轻微丢。")
    else:
        print("                  → 这一轮明显低于满速。")

    # 4) 只有存在 MQTT 对照时，才谈「gw 有没有责任」
    if mfps is not None:
        r = sfps / mfps
        print()
        if r >= 0.80:
            print("  与 gw 的关系   串口拿到了 MQTT 的 %.0f%%，字节是到了的。" % (r * 100))
            print("                 少的那些更可能是 ./gw 读循环/入库吞掉的，")
            print("                 去比 ./gw 终端里「有效帧=」和「入库=」对不对得上。")
        else:
            print("  与 gw 的关系   绕开 gw 直接读串口，只有 MQTT 的 %.0f%%。" % (r * 100))
            print("                 字节在到达程序之前就没了，gw 无罪。")

    print()
    print("  下一轮跑之前，先决定要测哪一种（两轮各 60 秒）：")
    print("    A 轮  只留串口：停 ./mqtt_sub、把 ESP32 的 USB 拔掉（飞线不动）")
    print("    B 轮  全开    ：ESP32 插回、./mqtt_sub 跑起来")
    print("    两轮都执行  python3 tools/serial_probe.py -t 60")
    print("    然后只比两行「本次串口帧率」：差得多 = ESP32 在干扰；一样 = 与 ESP32 无关。")
    print("=" * 72)
    return 0


def selftest():
    """不开串口，喂合成字节验证解析和计数。"""
    print("自检：用合成字节流验证解析逻辑")
    c = Counter()

    def mk(temp, pir, lux, gas, node=1):
        b = bytearray(24)
        b[0:4] = HEAD
        b[4] = temp % 10
        b[5] = temp // 10
        b[6] = 0
        b[7] = 52
        b[8] = node
        b[12] = pir
        b[14] = lux & 0xFF
        b[15] = lux >> 8
        b[18] = gas & 0xFF
        b[19] = gas >> 8
        b[20] = 0
        return bytes(b)

    # 1) 10 个正常帧，一次喂进去
    blob = b"".join(mk(198, 0, 665, 26) for _ in range(10))
    c.feed(blob, 1000.0)
    print("  10 帧连续喂入       → 帧=%d 坏=%d 重同步=%d （期望 10/0/0）"
          % (c.frames, c.bad, c.resync))

    # 2) 前面有垃圾字节
    c2 = Counter()
    c2.feed(b"\x00\xff" + mk(198, 0, 665, 26), 1000.0)
    print("  2 字节垃圾 + 1 帧   → 帧=%d 坏=%d 重同步=%d （期望 1/0/2）"
          % (c2.frames, c2.bad, c2.resync))

    # 3) 字段不合法（pir=5）应判坏帧，然后重新对齐
    c3 = Counter()
    c3.feed(mk(198, 5, 665, 26) + mk(198, 0, 665, 26), 1000.0)
    print("  pir=5 + 正常帧      → 帧=%d 坏=%d （期望 1/1）"
          % (c3.frames, c3.bad))

    # 4) 分片喂：把一帧拆成 3 段
    c4 = Counter()
    f = mk(198, 1, 665, 26)
    c4.feed(f[:7], 1000.0)
    c4.feed(f[7:16], 1000.0)
    c4.feed(f[16:], 1000.0)
    print("  一帧拆 3 段喂       → 帧=%d 坏=%d （期望 1/0）" % (c4.frames, c4.bad))

    # 5) lux 超范围（>1023）应拒收
    c5 = Counter()
    c5.feed(mk(198, 0, 2000, 26), 1000.0)
    print("  lux=2000            → 帧=%d 坏=%d （期望 0/1）" % (c5.frames, c5.bad))

    print()
    print("自检完成。若上面括号里的期望值都对得上，解析逻辑就是对的。")


def main():
    ap = argparse.ArgumentParser(
        description="绕开 gw 直接量串口原始字节流，并和 MQTT 侧同期帧数对比")
    ap.add_argument("-d", "--device", default="/dev/ttyUSB0", help="串口设备")
    ap.add_argument("-t", "--seconds", type=int, default=60, help="探测多少秒（默认 60）")
    ap.add_argument("--db", default="./env_mqtt.db", help="MQTT 链路的库")
    ap.add_argument("--selftest", action="store_true", help="不开串口，只验证解析逻辑")
    args = ap.parse_args()

    if args.selftest:
        selftest()
        return 0

    if not os.path.exists(args.device):
        print("错误：找不到 %s" % args.device, file=sys.stderr)
        print("      先确认协调器插着，再用 ls /dev/tty* 看真实设备名", file=sys.stderr)
        return 2
    if args.seconds < 10:
        print("--seconds 至少 10 秒，太短了数不准", file=sys.stderr)
        return 2
    try:
        return probe(args.device, args.seconds, args.db)
    except OSError as e:
        print("错误：打开 %s 失败（%s）" % (args.device, e), file=sys.stderr)
        print("      最常见的原因：./gw 还在跑，占着这个端口。先 Ctrl+C 停掉它。",
              file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("\n已中断")
        return 130


if __name__ == "__main__":
    sys.exit(main())
