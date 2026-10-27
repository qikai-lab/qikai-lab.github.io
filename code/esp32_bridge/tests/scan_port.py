"""在多个波特率下扫一个串口，看有没有任何字节。只读，不写。"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM10"
BAUDS = [115200, 74880, 9600, 460800, 57600]

for baud in BAUDS:
    try:
        s = serial.Serial()
        s.port = PORT
        s.baudrate = baud
        s.timeout = 0.2
        # 打开前先声明好控制线状态，尽量不干扰板子
        s.dtr = False
        s.rts = False
        s.open()
    except Exception as e:
        print(f"{baud:>7}: 打不开 -> {e}")
        continue

    buf = b""
    t0 = time.time()
    while time.time() - t0 < 2.0:
        chunk = s.read(512)
        if chunk:
            buf += chunk
    s.close()

    if buf:
        text = "".join(chr(c) if 32 <= c < 127 else "." for c in buf)
        print(f"{baud:>7}: {len(buf):>5} 字节  {text[:80]}")
    else:
        print(f"{baud:>7}:      0 字节")

print("\n说明：ESP32 空片上电会打印 'ESP-ROM:esp32s3-...'，波特率 115200。")
print("      所有波特率都是 0 字节 => 板子没在运行，不是波特率问题。")
