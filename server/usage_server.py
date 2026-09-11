#!/usr/bin/env python3
"""Serve Claude and ChatGPT plan allowance as JSON for the RationAI display.

    python3 usage_server.py            # http://0.0.0.0:8787/usage
    python3 usage_server.py --once     # print the JSON and exit

Nothing leaves this machine except one tiny Haiku probe to api.anthropic.com,
using the OAuth token Claude Code already stores here. The display just reads
the JSON over the LAN, so no credentials ever reach the ESP32.
"""
import argparse
import atexit
import json
import shutil
import socket
import subprocess
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import activity
import claude_live
import codex_live
import codex_logs
import history

CODEX_DIR = Path.home() / ".codex" / "sessions"


def snapshot(force=False):
    claude = demo_claude(time.time() - _started) if DEMO else claude_live.usage(force=force)

    # Only real readings feed the trend; a stale replay would flatten the slope.
    if claude.get("available") and not claude.get("stale_for"):
        for name in ("session", "week", "credits"):
            window = claude.get(name)
            if isinstance(window, dict):
                history.record(name, window.get("percent"))
                trend = history.summary(name, window.get("percent"), window.get("resets_in"))
                if trend:
                    window["trend"] = trend

    codex = codex_live.usage(force=force)
    # The live endpoint is the real allowance; the session logs are only a
    # last resort, and go stale the moment you stop using Codex.
    if not codex.get("available"):
        fallback = codex_logs.usage()
        if fallback.get("available"):
            fallback["reason"] = codex.get("reason", "live endpoint unavailable")
            codex = fallback

    if codex.get("available") and not codex.get("stale_for"):
        for name in ("session", "week", "credits"):
            window = codex.get(name)
            if isinstance(window, dict):
                history.record("codex_" + name, window.get("percent"))
                trend = history.summary("codex_" + name, window.get("percent"),
                                        window.get("resets_in"))
                if trend:
                    window["trend"] = trend

    return {
        "generated_at": int(time.time()),
        "claude": claude,
        "codex": codex,
    }


class Handler(BaseHTTPRequestHandler):
    def _send(self, body, status=200):
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        # Claude Code hook events land here. Body is untrusted input from a
        # local hook, so it is parsed defensively and only known event names
        # are acted on -- nothing in it is ever executed.
        if self.path.split("?")[0] != "/event":
            self.send_error(404)
            return
        try:
            length = min(int(self.headers.get("Content-Length") or 0), 64_000)
            payload = json.loads(self.rfile.read(length) or b"{}")
            ok = activity.record_event(payload) if isinstance(payload, dict) else False
        except (ValueError, json.JSONDecodeError):
            ok = False
        self._send(json.dumps({"ok": ok}).encode())

    def do_GET(self):
        path = self.path.split("?")[0]

        # The live channel: hook state, burn rate, session cost. No network and
        # no API calls, so the device can poll it every second.
        if path == "/live":
            try:
                body = json.dumps(activity.snapshot()).encode()
            except Exception as e:
                body = json.dumps({"error": str(e)}).encode()
            self._send(body)
            return

        if path not in ("/", "/usage"):
            self.send_error(404)
            return
        try:
            body = json.dumps(snapshot()).encode()
        except Exception as e:  # never let one bad poll kill the display
            body = json.dumps({"error": str(e)}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        # One line per quota poll. /live runs every second or two, so logging
        # it would drown everything worth reading.
        line = fmt % args
        if "/live" in line and not __import__("os").environ.get("RATIONAI_LOG_LIVE"):
            return
        print(f"{time.strftime('%H:%M:%S')} {self.client_address[0]} {line}", flush=True)


# Advertising a service (rather than relying on the machine's hostname) is what
# lets the device find whichever laptop is running the server, on any network,
# with nothing configured on the device.
SERVICE_TYPE = "_rationai._tcp"


def advertise(port):
    """Publish a Bonjour service via macOS's built-in dns-sd, if present."""
    if not shutil.which("dns-sd"):
        return None
    name = f"RationAI on {socket.gethostname().split('.')[0]}"
    try:
        proc = subprocess.Popen(
            ["dns-sd", "-R", name, SERVICE_TYPE, ".", str(port)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
    except OSError:
        return None
    atexit.register(lambda: proc.terminate())
    print(f"advertising {SERVICE_TYPE} as {name!r} on port {port}", flush=True)
    return proc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--once", action="store_true", help="print JSON and exit")
    ap.add_argument("--demo", action="store_true",
                    help="serve a synthetic fast-burning account instead of the real one")
    args = ap.parse_args()

    global DEMO
    DEMO = args.demo

    if args.once:
        print(json.dumps(snapshot(force=True), indent=2))
        return

    advertise(args.port)
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"serving usage on http://{args.host}:{args.port}/usage")
    server.serve_forever()


if __name__ == "__main__":
    main()
