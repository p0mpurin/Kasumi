#!/usr/bin/env python3
"""Hand local builds to a 3DS through Kasumi's own updater.

    python tools/dev_server.py [port]

Serves Kasumi.cia and Kasumi.3dsx from the repository root, plus dev.json
describing them (build number, checksum). On the console, put one line with
this PC's address in sdmc:/3ds/kasumi/dev_server.txt, for example

    192.168.1.10:8642

and Settings > Software update offers every new build made here (`make`,
`make cia`), installed and checked like a release. Delete the file to go
back to GitHub releases. Only for your own console on your own network:
anything on the network can download the builds while this runs.
"""
import hashlib
import http.server
import json
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACKAGES = {"cia": "Kasumi.cia", "3dsx": "Kasumi.3dsx"}
_hashes = {}


def local_ip():
    # A UDP "connect" sends nothing; it only picks the outgoing interface.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        try:
            s.connect(("192.168.0.1", 9))
            return s.getsockname()[0]
        except OSError:
            return "127.0.0.1"


def app_build():
    try:
        text = open(os.path.join(ROOT, "include", "app_paths.h"), encoding="utf-8").read()
        match = re.search(r'#define APP_BUILD "([^"]+)"', text)
        return match.group(1) if match else "?"
    except OSError:
        return "?"


def git_summary():
    try:
        head = subprocess.run(["git", "log", "-1", "--format=%h %s"], cwd=ROOT, capture_output=True,
                              text=True, timeout=5).stdout.strip()
        changed = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT, capture_output=True,
                                 text=True, timeout=5).stdout.strip().splitlines()
        return head, len(changed)
    except (OSError, subprocess.SubprocessError):
        return "", 0


def package_info(name):
    path = os.path.join(ROOT, name)
    try:
        stat = os.stat(path)
    except OSError:
        return None
    key = (path, stat.st_mtime_ns, stat.st_size)
    if key not in _hashes:
        with open(path, "rb") as f:
            _hashes[key] = hashlib.sha256(f.read()).hexdigest()
    return {"file": name, "size": stat.st_size, "sha256": _hashes[key], "mtime": stat.st_mtime}


def manifest():
    info = {kind: package_info(name) for kind, name in PACKAGES.items()}
    newest = max((p["mtime"] for p in info.values() if p), default=time.time())
    built = time.strftime("%Y-%m-%d %H:%M", time.localtime(newest))
    head, changed = git_summary()
    notes = f"A build from your PC, made {built}.\n"
    if head:
        notes += f"- Last commit: {head}\n"
    if changed:
        notes += f"- Plus {changed} uncommitted change{'s' if changed != 1 else ''}\n"
    out = {"build": app_build(), "built": built, "notes": notes}
    for kind, p in info.items():
        if p:
            out[kind] = {k: p[k] for k in ("file", "size", "sha256")}
    return out


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/dev.json":
            body = json.dumps(manifest(), indent=2).encode()
            self.reply(200, "application/json", body)
        elif path.lstrip("/") in PACKAGES.values():
            try:
                with open(os.path.join(ROOT, path.lstrip("/")), "rb") as f:
                    body = f.read()
            except OSError:
                self.reply(404, "text/plain", b"not built yet")
                return
            self.reply(200, "application/octet-stream", body)
        else:
            self.reply(404, "text/plain", b"not here")

    def reply(self, status, content_type, body):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        print(f"{time.strftime('%H:%M:%S')}  {self.client_address[0]}  {fmt % args}")


def main():
    sys.stdout.reconfigure(line_buffering=True)
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8642
    address = f"{local_ip()}:{port}"
    print(f"Kasumi dev server, build {app_build()}, serving {ROOT}")
    print(f"Put this one line in sdmc:/3ds/kasumi/dev_server.txt:\n\n    {address}\n")
    print("Then Settings > Software update on the 3DS. Ctrl+C stops the server.")
    http.server.ThreadingHTTPServer(("0.0.0.0", port), Handler).serve_forever()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
