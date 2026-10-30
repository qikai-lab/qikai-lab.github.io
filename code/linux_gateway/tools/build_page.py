#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
build_page.py - 把 web/index.html 打成 C 数组，生成 http_page.h

为什么要有这一步（而不是运行时读文件）
--------------------------------------
网关是「拷一个二进制过去就能跑」的工具，不该依赖外部 html 文件躺在某个
相对路径下。打进二进制 = 部署时只需要 ./gw 一个文件。

为什么不把 HTML 直接写成 C 字符串常量
-------------------------------------
HTML 里有大量双引号和反斜杠，手写转义很难维护，改一个样式要盯着一堆 \\"。
这里用 Python 生成字节数组，index.html 可以当普通网页文件编辑。

用法（在 Windows 或 Ubuntu 都行）：
    python tools/build_page.py
    python3 tools/build_page.py

产物：http_page.h（已随包提供，**不需要在 Ubuntu 上专门跑这一步**，
      只有改了 web/index.html 才需要重新生成）
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "web", "index.html")
OUT = os.path.join(ROOT, "http_page.h")

PER_LINE = 16


def main():
    if not os.path.isfile(SRC):
        sys.stderr.write("找不到 %s\n" % SRC)
        return 1

    with open(SRC, "rb") as f:
        data = f.read()

    # 去掉 UTF-8 BOM：浏览器能认，但没必要让它进二进制
    if data.startswith(b"\xef\xbb\xbf"):
        data += b""
        data = data[3:]

    # 统一成 LF，避免 Windows 上编辑后混进 CRLF（HTTP 不影响，但字节数会多）
    data = data.replace(b"\r\n", b"\n")

    # 末尾补一个 '\0'：以后如果想当 C 字符串用也不会越界（长度不含它）
    n = len(data)

    lines = []
    lines.append("/*")
    lines.append(" * http_page.h - 自动生成，**不要手改**")
    lines.append(" *")
    lines.append(" * 源文件 : web/index.html")
    lines.append(" * 生成器 : tools/build_page.py")
    lines.append(" * 大小   : %d 字节" % n)
    lines.append(" *")
    lines.append(" * 改了网页之后要重新跑一遍：python3 tools/build_page.py")
    lines.append(" */")
    lines.append("#ifndef GW_HTTP_PAGE_H")
    lines.append("#define GW_HTTP_PAGE_H")
    lines.append("")
    lines.append("#define GW_INDEX_HTML_LEN  %dU" % n)
    lines.append("")
    lines.append("static const unsigned char g_index_html[GW_INDEX_HTML_LEN] = {")

    for i in range(0, n, PER_LINE):
        chunk = data[i:i + PER_LINE]
        lines.append("    " + "".join("0x%02x," % b for b in chunk))

    lines.append("};")
    lines.append("")
    lines.append("#endif /* GW_HTTP_PAGE_H */")
    lines.append("")

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))

    print("已生成 %s" % OUT)
    print("  HTML %d 字节 -> C 数组约 %d 行" % (n, (n + PER_LINE - 1) // PER_LINE))
    # 网页是 memcpy 进 g_body（HTTP_RESP_MAX 字节）再发的，超了就发不全。
    # 留 2KB 余量给以后加东西，别每次都踩着上限改。
    if n > 30768:
        print("  [错误] HTML 超过 30768 字节，会超出 HTTP_RESP_MAX(32768)，页面会被截断")
        return 2
    if n > 28000:
        print("  [警告] HTML 已到 %d 字节，HTTP_RESP_MAX=32768，余量不足 5KB" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
