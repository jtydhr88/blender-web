"""
Blender Web HTTPS/WSS Proxy — enables WebCodecs on remote (non-localhost) clients.

Generates a self-signed certificate on first run, then proxies:
  HTTPS (port+2) → HTTP (port)
  WSS   (port+2) → WS  (port)

Remote clients connect to https://IP:7683 instead of http://IP:7681.
"""

import bpy
import os
import sys
import ssl
import socket
import threading
import subprocess
import asyncio
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.request import urlopen, Request
from urllib.error import URLError

WEB_PORT = int(os.environ.get("BLENDER_WEB_PORT", "7681"))
HTTPS_PORT = WEB_PORT + 2  # 7683 by default
BIND = os.environ.get("BLENDER_WEB_BIND", "127.0.0.1")

CERT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "certs")
CERT_FILE = os.path.join(CERT_DIR, "blender_web.pem")
KEY_FILE = os.path.join(CERT_DIR, "blender_web_key.pem")


def _log(msg):
    sys.stderr.write(f"[BlenderWebHTTPS] {msg}\n")
    sys.stderr.flush()


def _generate_cert():
    """Generate a self-signed certificate using openssl."""
    os.makedirs(CERT_DIR, exist_ok=True)

    if os.path.exists(CERT_FILE) and os.path.exists(KEY_FILE):
        _log(f"Using existing cert: {CERT_FILE}")
        return True

    # Try openssl from common Windows locations.
    openssl_paths = [
        "openssl",
        r"C:\Program Files\Git\mingw64\bin\openssl.exe",
        r"C:\Program Files\Git\usr\bin\openssl.exe",
        r"C:\msys64\mingw64\bin\openssl.exe",
        r"D:\Program Files\Git\mingw64\bin\openssl.exe",
    ]

    for openssl_bin in openssl_paths:
        try:
            cmd = [
                openssl_bin, "req", "-x509", "-newkey", "rsa:2048",
                "-keyout", KEY_FILE, "-out", CERT_FILE,
                "-days", "365", "-nodes",
                "-subj", "/CN=BlenderWebDisplay/O=Blender/C=US",
            ]
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
            if result.returncode == 0:
                _log(f"Generated self-signed cert: {CERT_FILE}")
                return True
        except FileNotFoundError:
            continue
        except Exception as e:
            _log(f"openssl error: {e}")
            continue

    _log("openssl not found (install Git for Windows or add openssl to PATH)")
    return False


class ProxyHandler(BaseHTTPRequestHandler):
    """Proxy HTTP requests to the Blender Web Display server."""

    def log_message(self, format, *args):
        pass

    def do_GET(self):
        target = f"http://127.0.0.1:{WEB_PORT}{self.path}"
        try:
            req = Request(target)
            for key, val in self.headers.items():
                if key.lower() not in ("host", "connection"):
                    req.add_header(key, val)
            with urlopen(req, timeout=5) as resp:
                body = resp.read()
                self.send_response(resp.status)
                for key, val in resp.getheaders():
                    if key.lower() not in ("transfer-encoding", "connection"):
                        self.send_header(key, val)
                self.end_headers()
                self.wfile.write(body)
        except Exception as e:
            self.send_error(502, f"Proxy error: {e}")


def _run_wss_proxy():
    """Simple WSS-to-WS proxy using raw sockets + SSL."""

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(CERT_FILE, KEY_FILE)

    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server_sock.bind((BIND, HTTPS_PORT))
    server_sock.listen(5)
    _log(f"WSS/HTTPS proxy on https://{BIND}:{HTTPS_PORT}")

    def handle_client(client_ssl):
        """Handle one client: detect if WebSocket upgrade or plain HTTP, then proxy."""
        try:
            client_ssl.settimeout(10)
            data = client_ssl.recv(8192)
            req_line = data.split(b'\r\n')[0].decode('utf-8', errors='replace') if data else ''
            _log(f"Request: {req_line[:80]}")
            if not data:
                client_ssl.close()
                return

            request = data.decode("utf-8", errors="replace")

            if "Upgrade: websocket" in request or "Upgrade: WebSocket" in request:
                _log("WebSocket upgrade detected, proxying...")
                backend = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                backend.connect(("127.0.0.1", WEB_PORT))
                backend.sendall(data)  # Forward the upgrade request.

                # Read backend's upgrade response.
                resp = backend.recv(4096)
                client_ssl.sendall(resp)

                # Bidirectional relay using two threads (SSL sockets
                # need blocking sends to handle large H.264 frames).
                relay_alive = [True]

                def relay(src, dst, name):
                    try:
                        src.settimeout(1.0)
                        while relay_alive[0]:
                            try:
                                chunk = src.recv(262144)
                                if not chunk:
                                    break
                                dst.sendall(chunk)
                            except (socket.timeout, ssl.SSLWantReadError):
                                continue
                            except Exception:
                                break
                    except Exception:
                        pass
                    relay_alive[0] = False

                t1 = threading.Thread(target=relay, args=(client_ssl, backend, "c->b"), daemon=True)
                t2 = threading.Thread(target=relay, args=(backend, client_ssl, "b->c"), daemon=True)
                t1.start()
                t2.start()
                t1.join()
                t2.join()

                backend.close()
            else:
                # Plain HTTPS: proxy to HTTP backend.
                backend = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                backend.connect(("127.0.0.1", WEB_PORT))
                backend.sendall(data)
                backend.settimeout(5)

                while True:
                    try:
                        chunk = backend.recv(65536)
                        if not chunk:
                            break
                        client_ssl.sendall(chunk)
                    except socket.timeout:
                        break
                    except Exception:
                        break
                backend.close()

        except Exception as e:
            _log(f"Client handler error: {e}")
        finally:
            try:
                client_ssl.close()
            except Exception:
                pass

    while True:
        try:
            client_sock, addr = server_sock.accept()
            try:
                client_ssl = ctx.wrap_socket(client_sock, server_side=True)
                threading.Thread(target=handle_client, args=(client_ssl,), daemon=True).start()
            except ssl.SSLError:
                client_sock.close()
        except Exception:
            pass


def _start():
    if not _generate_cert():
        _log("Cannot generate certificate, HTTPS disabled")
        return

    t = threading.Thread(target=_run_wss_proxy, daemon=True)
    t.start()

    _log(f"Remote clients: https://<your-ip>:{HTTPS_PORT}")
    _log("(Accept the self-signed certificate warning in the browser)")


def register():
    _start()


def unregister():
    pass
