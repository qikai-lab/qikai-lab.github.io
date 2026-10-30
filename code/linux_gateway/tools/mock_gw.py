#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
_mock_gw.py - 假网关，用来在没有板子的情况下验证网页

为什么要这个
------------
改网页（加坐标轴、悬停读数）不能每次都插板子验证。这里用假数据
把 5 个接口按真实格式吐出来，网页拿到的 JSON 结构和真 gw 一模一样，
所以能验证"坐标轴画没画出来、悬停出不出数、掉线缺口断没断"。

特意在 1h 数据里挖了两段：
  · 第 40~46 个桶整段消失      → 验证曲线会不会断开（而不是拉条斜线）
  · 第 30~34 个桶里 n 只有 6   → 验证"桶内帧数很少"这种半掉线也画得出来

用法：
    python tools/mock_gw.py      # 然后浏览器开 http://127.0.0.1:8811/
"""
import json
import math
import os
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = os.path.join(os.path.dirname(HERE), "web", "index.html")
PORT = 8811


def now():
    return int(time.time())


def api_latest():
    return {
        "ok": 1, "valid": 1, "ts": now(), "time": time.strftime("%H:%M:%S"),
        "node": 1, "temp": 23.4, "hum": 55.2, "lux": 412, "lux_pct": 40,
        "gas": 36, "pir": 0, "relay": 0, "gas_med": 36, "gas_base": 34,
        "gas_pct": 259, "alarm": "normal", "alarm_code": 0, "alarm_peak": 0,
        "alarm_total": 2, "alarm_sec": 0, "frames": 18422, "uptime": 3600,
    }


def api_history():
    items = []
    for i in range(60):
        items.append({
            "ts": now() - 59 + i, "node": 1,
            "temp": round(23.2 + math.sin(i / 6.0) * 0.6, 1),
            "hum": round(55.0 + math.sin(i / 9.0) * 2.0, 1),
            "lux": 400 + i, "gas": 36 + (i % 3), "pir": 0, "relay": 0,
        })
    return {"ok": 1, "count": len(items), "items": items}


def api_alarms():
    items = [
        {"start": now() - 5400, "dur": 46, "peak": 312, "base": 34, "trig": 86, "open": 0},
        {"start": now() - 240, "dur": 12, "peak": 141, "base": 35, "trig": 88, "open": 0},
    ]
    return {"ok": 1, "count": len(items), "items": items}


def api_trend(win):
    bucket = 30 if win <= 3600 else (180 if win <= 21600 else 900)
    n = win // bucket
    base = now() - win
    items = []
    for i in range(n):
        # 1h 视图：第 40~46 个桶整段没有 → 曲线必须断开
        if win == 3600 and 40 <= i <= 46:
            continue
        # 1h 视图：第 30~34 个桶只有 6 帧 → 半掉线，但仍有数据
        cnt = 6 if (win == 3600 and 30 <= i <= 34) else bucket
        items.append({
            "t": base + i * bucket, "n": cnt,
            "temp": round(23.0 + math.sin(i / 8.0) * 1.2, 1),
            "hum": round(54.0 + math.cos(i / 11.0) * 3.0, 1),
            "tmin": 22.5, "tmax": 24.5, "lux": 300, "gas": 35, "gmax": 60, "pir": 0,
        })
    return {"ok": 1, "range": win, "bucket": bucket, "from": base, "to": now(),
            "count": len(items), "items": items}


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, body, ctype="application/json; charset=utf-8"):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)

        if u.path in ("/", "/index.html"):
            with open(PAGE, "rb") as f:
                self._send(f.read(), "text/html; charset=utf-8")
        elif u.path == "/api/latest":
            self._send(json.dumps(api_latest()))
        elif u.path == "/api/history":
            self._send(json.dumps(api_history()))
        elif u.path == "/api/alarms":
            self._send(json.dumps(api_alarms()))
        elif u.path == "/api/trend":
            r = (q.get("range") or ["1h"])[0]
            win = 86400 if r.startswith("24") else 3600
            self._send(json.dumps(api_trend(win)))
        else:
            self.send_response(404)
            self.end_headers()


if __name__ == "__main__":
    print("fake gateway on http://127.0.0.1:%d/" % PORT)
    HTTPServer(("127.0.0.1", PORT), H).serve_forever()
