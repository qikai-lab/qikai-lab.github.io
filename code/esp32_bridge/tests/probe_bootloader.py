"""
探测 ESP32-S3 到底能不能进 bootloader。

不依赖 esptool 的复位逻辑，自己控制 DTR/RTS 的四种组合，
每种都试两种做法：
  A. 静态（不动作）直接发同步包
  B. 标准复位时序（EN 拉低 → IO0 拉低 → EN 释放）再发同步包

能收到 0xC0 ... 0x01 的回应，就说明芯片在 bootloader 里，
并且告诉我们哪种控制方式有效。

只读 + 尝试握手，不写入任何 flash 内容。
"""
import struct
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM10"
BAUD = 115200

# ESP32 ROM 的 SYNC 命令体：波特率 + 固定的魔数尾巴
SYNC_DATA = struct.pack("<I", BAUD) + b"\x07\x07\x12\x20" + b"\x55" * 32


def build_sync() -> bytes:
    chk = 0xEF
    for b in SYNC_DATA:
        chk ^= b
    return struct.pack("<BBHI", 0xC0, 0x08, len(SYNC_DATA), chk) + SYNC_DATA + b"\xC0"


CMD = build_sync()


def open_port():
    s = serial.Serial()
    s.port = PORT
    s.baudrate = BAUD
    s.timeout = 0.15
    s.write_timeout = 0.5
    s.open()
    return s


def try_sync(s) -> bytes:
    """连发 4 次同步包，看有没有回应"""
    buf = b""
    for _ in range(4):
        try:
            s.reset_input_buffer()
            s.write(CMD)
        except Exception:
            return b""
        time.sleep(0.06)
        buf += s.read(64)
        if b"\xc0" in buf:
            break
    return buf


results = []

# ---- 组合 1~4：静态电平，不动复位线 ----
for dtr in (False, True):
    for rts in (False, True):
        label = f"静态 DTR={int(dtr)} RTS={int(rts)}"
        try:
            s = open_port()
            s.dtr = dtr
            s.rts = rts
            time.sleep(0.1)
            r = try_sync(s)
            s.close()
        except Exception as e:
            results.append((label, f"打不开: {e}"))
            continue
        results.append((label, f"{len(r)} 字节 {r[:16].hex()}"))

# ---- 组合 5：经典复位时序（esptool 原版） ----
label = "经典复位时序"
try:
    s = open_port()
    s.dtr = False   # IO0 = 高
    s.rts = True    # EN  = 低（复位）
    time.sleep(0.15)
    s.dtr = True    # IO0 = 低
    s.rts = False   # EN  = 高（出复位，进 bootloader）
    time.sleep(0.06)
    s.dtr = False   # IO0 = 高
    time.sleep(0.05)
    r = try_sync(s)
    s.close()
except Exception as e:
    results.append((label, f"打不开: {e}"))
    r = None
if r is not None:
    results.append((label, f"{len(r)} 字节 {r[:16].hex()}"))

# ---- 组合 6：反相复位时序 ----
label = "反相复位时序"
try:
    s = open_port()
    s.dtr = True
    s.rts = False
    time.sleep(0.15)
    s.dtr = False
    s.rts = True
    time.sleep(0.06)
    s.dtr = True
    time.sleep(0.05)
    r = try_sync(s)
    s.close()
except Exception as e:
    results.append((label, f"打不开: {e}"))
    r = None
if r is not None:
    results.append((label, f"{len(r)} 字节 {r[:16].hex()}"))

print(f"端口 {PORT} @ {BAUD}\n")
hit = False
for label, res in results:
    mark = ""
    # 只有真正收到了字节才算有回应（"0 字节" 就是没有）
    got_bytes = "打不开" not in res and not res.startswith("0 字节")
    if got_bytes:
        mark = "   <<< 有回应！"
        if "c001" in res.replace(" ", "").lower():
            mark = "   <<< 同步成功，芯片在 bootloader！"
            hit = True
    print(f"  {label:<24} {res}{mark}")

print()
if hit:
    print("结论：芯片能进 bootloader，问题只是复位方式 -> 用上面标出来的那组参数烧录。")
else:
    print("结论：六种方式全部没有回应。芯片没有进 bootloader，也没在正常运行。")
    print("下一步按顺序查：")
    print("  1. 板上电源指示灯亮不亮（不亮=没供电，换数据线）")
    print("  2. BOOT 键到底在哪、按下时有没有手感/是否松脱")
    print("  3. 拔掉 ESP32 的 USB，看 COM10 会不会消失（验证这个口确实是它）")
