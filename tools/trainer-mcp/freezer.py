#!/usr/bin/env python3
"""Memory freezer: writes fixed values to RetroArch RAM in a loop.

Usage:
  freezer.py start <addr_hex> <value> [interval_sec] [tag]
  freezer.py stop  [tag]
  freezer.py status

Example (Splatterhouse 2 god mode):
  freezer.py start f6 4 0.1 health
"""

import os
import signal
import socket
import sys
import time

RA_HOST = os.environ.get("RETROARCH_CMD_HOST", "127.0.0.1")
RA_PORT = int(os.environ.get("RETROARCH_CMD_PORT", "55355"))
PID_DIR = "/tmp/retroarch-freezer"


def write_mem(addr, data):
    hexbytes = " ".join(f"{b:02x}" for b in data)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1)
    try:
        s.sendto(f"WRITE_CORE_MEMORY {addr:x} {hexbytes}".encode(),
                 (RA_HOST, RA_PORT))
        try:
            return s.recvfrom(4096)[0].decode().strip()
        except socket.timeout:
            return None
    finally:
        s.close()


def pidfile(tag):
    os.makedirs(PID_DIR, exist_ok=True)
    return os.path.join(PID_DIR, f"{tag}.pid")


def start(addr_s, value_s, interval, tag):
    addr = int(addr_s, 16)
    value = int(value_s, 0)
    size = 2 if value > 0xFF else 1
    data = value.to_bytes(size, "little")
    pf = pidfile(tag)
    if os.path.exists(pf):
        try:
            old = int(open(pf).read().strip())
            os.kill(old, 0)
            print(f"already running (pid {old})"); return
        except (ValueError, ProcessLookupError):
            pass
    pid = os.fork() if hasattr(os, "fork") else None
    if pid:
        open(pf, "w").write(str(pid))
        print(f"freezer '{tag}' started: 0x{addr:04x} <- {value} "
              f"every {interval}s (pid {pid})")
        return
    # child: detach and loop
    os.setsid()
    sys.stdin.close()
    sys.stdout = open(os.devnull, "w")
    sys.stderr = open(os.devnull, "w")
    while True:
        write_mem(addr, data)
        time.sleep(interval)


def stop(tag):
    pf = pidfile(tag)
    if not os.path.exists(pf):
        print("not running"); return
    try:
        pid = int(open(pf).read().strip())
        os.kill(pid, signal.SIGTERM)
        print(f"freezer '{tag}' stopped (pid {pid})")
    except (ValueError, ProcessLookupError):
        print("stale pid file")
    os.unlink(pf)


def status():
    if not os.path.isdir(PID_DIR):
        print("no freezers"); return
    found = False
    for f in os.listdir(PID_DIR):
        if f.endswith(".pid"):
            tag = f[:-4]
            pid = int(open(os.path.join(PID_DIR, f)).read().strip())
            try:
                os.kill(pid, 0)
                print(f"{tag}: running (pid {pid})")
            except ProcessLookupError:
                print(f"{tag}: dead (stale pid {pid})")
            found = True
    if not found:
        print("no freezers")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    action = sys.argv[1]
    if action == "start":
        addr = sys.argv[2]
        value = sys.argv[3]
        interval = float(sys.argv[4]) if len(sys.argv) > 4 else 0.1
        tag = sys.argv[5] if len(sys.argv) > 5 else f"{addr}"
        start(addr, value, interval, tag)
    elif action == "stop":
        stop(sys.argv[2] if len(sys.argv) > 2 else "health")
    elif action == "status":
        status()
    else:
        print(__doc__); sys.exit(1)
