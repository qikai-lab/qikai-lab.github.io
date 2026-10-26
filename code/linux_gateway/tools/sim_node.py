#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sim_node.py - 模拟 CC2530 终端节点，按真实 EnvMsg 协议往串口/虚拟串口发数据

协议：st: 固定 24 字节（与 CC2530 固件 EnvMsg 结构体完全一致）
    偏移  长度  字段
    0     3     's' 't' ':'
    3     1     'e'
    4     1     温度小数位
    5     1     温度整数位
    6     1     湿度小数位
    7     1     湿度整数位
    8     1     节点号
    9     3     x / y / z（恒 0）
    12    2     人体红外 0/1（小端）
    14    2     光照 ADC 0~1023（小端）
    16    2     备用（恒 0）
    18    2     烟雾 ADC 0~1023（小端）
    20    2     继电器 0/1（小端）
    22    2     未使用（恒 0）

用途：没有 CC2530 硬件时开发和测试 Linux 网关

用法：
    python3 sim_node.py <设备路径> [选项]

选项：
    --interval 1.0    发帧间隔（秒），默认 1.0，和真实节点一致
    --nodes 1         模拟几个节点（帧交替发送），默认 1
    --noise           注入垃圾字节 + 截断帧，验证网关的帧头重同步
    --dry-run         不写设备，只在终端打印将要发送的字节（离线自检用）

例：
    # Ubuntu 上造假串口对（先开 socat，记下两个 /dev/pts/N）
    socat -d -d pty,raw,echo=0 pty,raw,echo=0
    python3 sim_node.py /dev/pts/6
    ./gw -d /dev/pts/5

    # 多节点 + 噪音，验证健壮性
    python3 sim_node.py /dev/pts/6 --nodes 3 --noise

    # 只想看字节长什么样
    python3 sim_node.py /dev/null --dry-run
"""
import argparse
import math
import os
import random
import struct
import sys
import time

FRAME_LEN = 24
HEAD = b"st:"
TYPE_ENV = ord("e")


def build_frame(node: int, temp_x10: int, hum_x10: int,
                pir: int, lux: int, gas: int, relay: int) -> bytes:
    """按 EnvMsg 结构体拼 24 字节帧（小端）"""
    f = bytearray(FRAME_LEN)
    f[0:3] = HEAD
    f[3]    = TYPE_ENV
    f[4]    = temp_x10 % 10          # 温度小数位
    f[5]    = (temp_x10 // 10) & 0xFF  # 温度整数位
    f[6]    = hum_x10 % 10           # 湿度小数位
    f[7]    = (hum_x10 // 10) & 0xFF   # 湿度整数位
    f[8]    = node & 0xFF
    # f[9:12] = x, y, z 恒 0
    struct.pack_into("<H", f, 12, pir & 0xFFFF)
    struct.pack_into("<H", f, 14, lux & 0xFFFF)
    # f[16:18] = rsv2 恒 0
    struct.pack_into("<H", f, 18, gas & 0xFFFF)
    struct.pack_into("<H", f, 20, relay & 0xFFFF)
    # f[22:24] = adc 恒 0
    return bytes(f)


class NodeState:
    """单个节点的"物理量"，让模拟数据像真的一样缓慢漂移"""

    def __init__(self, idx: int, smoke: bool = False):
        self.idx = idx
        self.node = idx + 1
        self.t0 = time.time()
        self.temp_base = 21.0 + random.uniform(-1.5, 1.5)
        self.hum_base = 51.0 + random.uniform(-6, 6)
        self.lux_base = 858 + random.randint(-40, 40)
        self.gas_base = 26 + random.randint(-4, 4)
        self.pir_until = 0.0      # PIR 保持到哪个时刻（HC-SR501 触发后保持 2~3 秒）
        self.smoke = smoke        # 定期制造一次烟雾事件，用来验 M4 告警链路

    def sample(self):
        """返回 (temp_x10, hum_x10, pir, lux, gas, relay)"""
        t = time.time() - self.t0

        # 温湿度：20 分钟周期的缓慢漂移 + 小抖动（模拟真实环境）
        temp = self.temp_base + 1.2 * math.sin(t / 1200.0) + random.uniform(-0.15, 0.15)
        hum = self.hum_base + 4.0 * math.sin(t / 900.0) + random.uniform(-0.4, 0.4)
        temp_x10 = max(0, int(round(temp * 10)))
        hum_x10 = min(1009, max(0, int(round(hum * 10))))

        # 光照：小幅抖动，偶尔有人走过挡一下
        lux = self.lux_base + random.randint(-2, 2)
        if random.random() < 0.03:
            lux -= random.randint(150, 400)
        lux = min(1023, max(0, lux))

        # 烟雾：MQ-2 静态时就是这么小幅跳动
        gas = self.gas_base + random.randint(-2, 2)

        # --smoke：每 60 秒来一次"烟雾事件"——
        #   前 2 秒在基线附近，接着 8 秒涨到 3 倍多，然后 12 秒慢慢落回。
        #   这样网关端应该看到 WARN → ALARM → 持续 → 解除 的完整过程。
        if self.smoke:
            c = t % 60.0
            if 10.0 <= c < 18.0:
                gas = int(self.gas_base * 3.5)
            elif 18.0 <= c < 30.0:
                k = (30.0 - c) / 12.0          # 1 → 0 线性回落
                gas = int(self.gas_base * (1 + 2.5 * k))
            # 另外塞一个孤立尖峰，专门验中值滤波吃不吃得掉
            if 40.0 <= c < 41.0:
                gas = 900

        gas = min(1023, max(0, gas))

        # 人体红外：3% 概率触发，触发后保持 2~3 秒（和 HC-SR501 行为一致）
        now = time.time()
        if now < self.pir_until:
            pir = 1
        else:
            pir = 0
            if random.random() < 0.03:
                pir = 1
                self.pir_until = now + random.uniform(2.0, 3.0)

        relay = 0   # 继电器由 S1 手动控制，模拟里保持 OFF
        return temp_x10, hum_x10, pir, lux, gas, relay


def fmt_frame(f: bytes) -> str:
    return " ".join(f"{b:02X}" for b in f)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="模拟 CC2530 终端节点（st: 24 字节协议）",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dev", help="串口设备，如 /dev/pts/6 或 /dev/ttyUSB0")
    ap.add_argument("--interval", type=float, default=1.0, help="发帧间隔秒，默认 1.0")
    ap.add_argument("--nodes", type=int, default=1, help="模拟节点数，默认 1")
    ap.add_argument("--noise", action="store_true", help="注入垃圾字节和截断帧")
    ap.add_argument("--smoke", action="store_true",
                    help="定期制造烟雾事件（每 60 秒一轮）+ 一个孤立尖峰，用来验 M4 告警")
    ap.add_argument("--dry-run", action="store_true", help="不写设备，只打印")
    args = ap.parse_args()

    if not args.dry_run:
        try:
            fd = os.open(args.dev, os.O_RDWR | os.O_NOCTTY)
        except OSError as e:
            print(f"[sim] 打开 {args.dev} 失败: {e}", file=sys.stderr)
            return 1
        # 做成 raw 模式，避免内核做任何字符转换
        try:
            import tty
            tty.setraw(fd)
        except Exception:
            pass
    else:
        fd = None

    nodes = [NodeState(i, smoke=args.smoke)
             for i in range(max(1, args.nodes))]

    print(f"[sim] 协议     : st: {FRAME_LEN} 字节")
    print(f"[sim] 目标设备 : {args.dev}{'  (dry-run，不实际写)' if args.dry_run else ''}")
    print(f"[sim] 节点数   : {len(nodes)}")
    print(f"[sim] 间隔     : {args.interval}s")
    print(f"[sim] 噪音注入 : {'开' if args.noise else '关'}")
    print("[sim] Ctrl+C 退出")
    print("-" * 72)

    sent = 0
    try:
        while True:
            for nd in nodes:
                temp_x10, hum_x10, pir, lux, gas, relay = nd.sample()
                frame = build_frame(nd.node, temp_x10, hum_x10, pir, lux, gas, relay)

                # 噪音：先塞一坨垃圾字节，模拟串口干扰
                if args.noise and random.random() < 0.15:
                    junk = bytes(random.randint(0, 255)
                                 for _ in range(random.randint(1, 6)))
                    if fd is not None:
                        os.write(fd, junk)
                    print(f"[sim] NOISE  注入 {len(junk)} 字节垃圾")

                if fd is not None:
                    os.write(fd, frame)
                sent += 1

                print(f"[sim] #{sent:<5d} n{nd.node}  {temp_x10 // 10}.{temp_x10 % 10}C "
                      f"{hum_x10 // 10}.{hum_x10 % 10}%  lux={lux:4d}  gas={gas:4d}  "
                      f"PIR={pir}  RLY={relay}")

                # 噪音：发一个缺了几个字节的残帧，验证网关的 stat_short
                if args.noise and random.random() < 0.10:
                    cut = random.randint(6, 16)
                    if fd is not None:
                        os.write(fd, frame[:cut])
                    print(f"[sim] TRUNC  故意只发前 {cut} 字节（网关应记残帧并恢复）")

                if args.dry_run:
                    print(f"        {fmt_frame(frame)}")

            time.sleep(max(0.05, args.interval))

    except KeyboardInterrupt:
        print(f"\n[sim] 已停止，共发送 {sent} 帧")
    except OSError as e:
        print(f"\n[sim] 写失败: {e}", file=sys.stderr)
        return 1
    finally:
        if fd is not None:
            os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
