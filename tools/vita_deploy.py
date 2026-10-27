#!/usr/bin/env python3
"""Put a Kasumi build on a PS Vita over VitaShell's FTP server.

On the Vita: VitaShell, SELECT (FTP on); it shows the address and port.

    python tools/vita_deploy.py 192.168.1.23            upload Kasumi.vpk, install it
    python tools/vita_deploy.py 192.168.1.23 --upload   upload only
    python tools/vita_deploy.py 192.168.1.23 --log      fetch Kasumi's diagnostic logs

VitaShell installs an uploaded VPK when sent its PROM command (VitaShell
must stay open on the Vita while it works). Logs land in build-vita/logs/.
"""
import argparse
import ftplib
import os
import sys

ROOT = os.path.join(os.path.dirname(__file__), "..")
REMOTE_VPK = "/ux0:/data/Kasumi.vpk"
DATA_DIR = "/ux0:/data/kasumi"
LOGS = ["kasumi-diagnostic.txt", "kasumi-diagnostic-previous.txt"]


def connect(host, port):
    ftp = ftplib.FTP()
    ftp.connect(host, port, timeout=30)
    ftp.login()
    return ftp


def upload(ftp, path):
    size = os.path.getsize(path)
    sent = [0]

    def progress(block):
        sent[0] += len(block)
        print(f"\r  {sent[0] * 100 // size:3d}%  {sent[0] // 1024} / {size // 1024} KiB", end="", flush=True)

    with open(path, "rb") as f:
        ftp.storbinary("STOR " + REMOTE_VPK, f, blocksize=64 * 1024, callback=progress)
    print()


def install(ftp):
    # VitaShell extracts and promotes the package; it takes a while and
    # answers when done.
    ftp.sock.settimeout(300)
    # PROM takes the Vita's own path, without the leading slash the other
    # FTP commands use ("/ux0:/..." answers 500 ERROR PROMOTING).
    print(" ", ftp.sendcmd("PROM " + REMOTE_VPK.lstrip("/")))


def fetch_logs(ftp):
    out = os.path.join(ROOT, "build-vita", "logs")
    os.makedirs(out, exist_ok=True)
    for name in LOGS:
        target = os.path.join(out, name)
        try:
            with open(target, "wb") as f:
                ftp.retrbinary(f"RETR {DATA_DIR}/{name}", f.write)
            print("  saved", os.path.relpath(target, ROOT))
        except ftplib.error_perm as error:
            os.remove(target)
            print(f"  {name}: {error}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("host")
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--vpk", default=os.path.join(ROOT, "Kasumi.vpk"))
    parser.add_argument("--upload", action="store_true", help="upload only, don't install")
    parser.add_argument("--log", action="store_true", help="fetch the diagnostic logs only")
    args = parser.parse_args()
    ftp = connect(args.host, args.port)
    try:
        if args.log:
            fetch_logs(ftp)
            return 0
        print("uploading", os.path.relpath(args.vpk, ROOT), "to", REMOTE_VPK)
        upload(ftp, args.vpk)
        if not args.upload:
            print("installing (VitaShell shows its progress on the Vita)")
            install(ftp)
    finally:
        try:
            ftp.quit()
        except (ftplib.Error, OSError):
            ftp.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
