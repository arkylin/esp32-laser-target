#!/usr/bin/env python3
"""给手机用的本地 HTTPS 静态服务器（Web Bluetooth 只在安全上下文里可用）。

用法（在工程根目录执行）：
    # 1) 生成自签证书，只需一次。Git for Windows / Linux / macOS 自带 openssl：
    openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
        -keyout tools/key.pem -out tools/cert.pem -subj "/CN=laser-web"

    # 2) 启动（默认 8443 端口，服务目录 web/）
    python tools/serve_web.py

    # 3) 手机连同一个 Wi-Fi，Chrome 打开脚本打印出来的 https://192.168.x.x:8443/
    #    证书不受信任时点「高级 → 继续访问」，之后 Web Bluetooth 就能用。

没有 tools/cert.pem 时脚本会退化成 http，并在日志里提示手机端会被浏览器拦下。
"""
import http.server
import socket
import ssl
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "web"
CERT = Path(__file__).resolve().parent / "cert.pem"
KEY = Path(__file__).resolve().parent / "key.pem"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8443


def lan_ip() -> str:
    """取本机在局域网里的地址（不会真的发包）。"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=str(ROOT), **kw)


if __name__ == "__main__":
    if not (ROOT / "index.html").exists():
        sys.exit(f"找不到 {ROOT / 'index.html'}")

    ctx = None
    scheme = "http"
    if CERT.exists() and KEY.exists():
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(CERT, KEY)
        scheme = "https"
    else:
        print("[!] 缺 tools/cert.pem，退化成 http —— 手机 Chrome 会拒绝 Web Bluetooth。")
        print("    先跑一遍文件顶部注释里的 openssl 命令再启动。")

    httpd = http.server.ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    if ctx:
        httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)

    print(f"[+] 服务目录 {ROOT}")
    print(f"[+] 电脑访问 {scheme}://localhost:{PORT}/")
    print(f"[+] 手机访问 {scheme}://{lan_ip()}:{PORT}/   （同一 Wi-Fi）")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[+] 已停止")
