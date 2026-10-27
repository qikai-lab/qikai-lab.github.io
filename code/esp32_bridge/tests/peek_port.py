"""临时诊断脚本：直接读 COM10 看 ESP32-S3 有没有在发启动日志。
不写任何东西，只读 4 秒。
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM10"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

try:
    s = serial.Serial(PORT, BAUD, timeout=0.2)
except Exception as e:
    print(f"打不开 {PORT}: {e}")
    print("（如果提示 Access is denied / 拒绝访问，说明串口被别的程序占着，先关掉 Monitor）")
    sys.exit(1)

# 打开串口时 pyserial 会拉 DTR/RTS，先归位，避免把板子按在复位里
try:
    s.dtr = False
    s.rts = False
except Exception:
    pass

buf = b""
t0 = time.time()
while time.time() - t0 < 4.0:
    chunk = s.read(512)
    if chunk:
        buf += chunk

s.close()

print(f"端口 {PORT} @ {BAUD}，读 4 秒")
print(f"收到字节数: {len(buf)}")
if not buf:
    print("→ 一个字节都没有。板子没在往这个口吐数据。")
    print("  可能是：波特率不对 / 这根 USB 只供电 / 插的不是调试口")
else:
    print(f"→ 前 {min(len(buf), 240)} 字节（repr，能看出 ASCII 就说明是日志）:")
    print(repr(buf[:240]))
    # 找出所有能读的 ASCII 片段
    text = "".join(chr(c) if 32 <= c < 127 else "." for c in buf)
    print("→ 转成可见字符:")
    print(text[:600])
