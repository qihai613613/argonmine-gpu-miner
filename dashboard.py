#!/usr/bin/env python3
"""Tiny local proxy/server for the Argonmine dashboard.

It serves dashboard.html and forwards the few explorer API endpoints the page
uses. This avoids browser CORS issues and works even when the browser cannot
reach the explorer directly. Standard library only.
"""

import argparse
import json
import re
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

BASE = "https://explorer.argonmine.com"
ROOT = Path(__file__).resolve().parent
ADDR_RE = re.compile(r"^arg1[0-9a-z]{20,90}$")

_cache = {}
_cache_lock = threading.Lock()


def fetch_json(path: str, ttl: float):
    now = time.time()
    with _cache_lock:
        hit = _cache.get(path)
        if hit and hit[0] > now:
            return hit[1]
    req = urllib.request.Request(
        BASE + path,
        headers={
            "User-Agent": "argonmine-dashboard/1.0",
            "Accept": "application/json",
        },
    )
    with urllib.request.urlopen(req, timeout=10) as r:
        data = json.loads(r.read().decode("utf-8"))
    with _cache_lock:
        _cache[path] = (now + ttl, data)
    return data


class Handler(BaseHTTPRequestHandler):
    server_version = "ArgonmineDashboard/1.0"

    def log_message(self, fmt, *args):
        pass

    def _send(self, code, body, ctype="application/json; charset=utf-8"):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _json(self, obj, code=200):
        self._send(code, json.dumps(obj, ensure_ascii=False))

    def do_GET(self):
        try:
            u = urllib.parse.urlparse(self.path)
            path = u.path
            qs = urllib.parse.parse_qs(u.query)

            if path in ("/", "/index.html", "/dashboard.html"):
                self._send(200, (ROOT / "dashboard.html").read_bytes(),
                           "text/html; charset=utf-8")
                return
            if path == "/api/stats":
                self._json(fetch_json("/api/stats", 3.0))
                return
            if path == "/api/charts":
                try:
                    n = max(10, min(1000, int(qs.get("n", ["288"])[0])))
                except Exception:
                    n = 288
                self._json(fetch_json(f"/api/charts?n={n}", 15.0))
                return
            if path == "/api/miners":
                self._json(fetch_json("/api/miners", 30.0))
                return
            if path.startswith("/api/address/"):
                addr = path[len("/api/address/"):].strip().lower()
                if not ADDR_RE.match(addr):
                    self._json({"error": "invalid address"}, 400)
                    return
                self._json(fetch_json("/api/address/" + urllib.parse.quote(addr), 3.0))
                return
            if path == "/health":
                self._json({"ok": True, "time": time.time()})
                return
            self._json({"error": "not found"}, 404)
        except urllib.error.HTTPError as e:
            self._json({"error": f"upstream HTTP {e.code}"}, 502)
        except urllib.error.URLError as e:
            self._json({"error": f"upstream unreachable: {e.reason}"}, 502)
        except Exception as e:
            self._json({"error": str(e)}, 502)


def main():
    ap = argparse.ArgumentParser(description="Argonmine dashboard")
    ap.add_argument("--host", default="127.0.0.1",
                    help="bind address (default 127.0.0.1; 0.0.0.0 for LAN)")
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--no-browser", action="store_true")
    args = ap.parse_args()

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.daemon_threads = True
    url = f"http://127.0.0.1:{args.port}/"
    print("Argonmine dashboard")
    print(f"  {url}")
    print("  Ctrl+C to stop")
    if not args.no_browser:
        threading.Timer(0.8, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nBye.")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
