#!/usr/bin/env python3
"""Fake RetroArch UDP responder + MCP handshake test.

Simulates RetroArch's network command interface with a synthetic 128KB RAM,
then drives the MCP server over stdio to verify the full tool workflow:
status -> snapshot -> diff/search -> write -> verify.
"""

import json
import socket
import subprocess
import sys
import threading
import time

PORT = 55399  # avoid clashing with a real RetroArch
RAM_SIZE = 128 * 1024


class FakeRetroArch(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.ram = bytearray(RAM_SIZE)
        # plant a "gold" value: 1530 = 0x05FA at 0x1234 (16-bit LE)
        self.ram[0x1234] = 0xFA
        self.ram[0x1235] = 0x05
        # decoy: same value elsewhere
        self.ram[0x9000] = 0xFA
        self.ram[0x9001] = 0x05
        self.paused = False
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", PORT))

    def run(self):
        while True:
            data, addr = self.sock.recvfrom(65535)
            cmd = data.decode().strip()
            verb, _, rest = cmd.partition(" ")
            reply = None
            if verb == "GET_STATUS":
                state = "PAUSED" if self.paused else "PLAYING"
                reply = f"GET_STATUS {state} snes9x,Chrono Trigger (USA).sfc,crc32=deadc0de\n"
            elif verb == "VERSION":
                reply = "1.21.0\n"
            elif verb == "READ_CORE_MEMORY":
                a_s, n_s = rest.split()
                a, n = int(a_s, 16), int(n_s)
                if a >= RAM_SIZE:
                    reply = f"READ_CORE_MEMORY {a:x} -1 no descriptor for address\n"
                else:
                    chunk = self.ram[a:a + n]
                    hexs = "".join(f" {b:02x}" for b in chunk)
                    reply = f"READ_CORE_MEMORY {a:x}{hexs}\n"
            elif verb == "WRITE_CORE_MEMORY":
                toks = rest.split()
                a = int(toks[0], 16)
                n = 0
                for t in toks[1:]:
                    if a + n >= RAM_SIZE:
                        break
                    self.ram[a + n] = int(t, 16)
                    n += 1
                reply = f"WRITE_CORE_MEMORY {a:x} {n}\n"
            elif verb == "PAUSE_TOGGLE":
                self.paused = not self.paused
            if reply is not None:
                self.sock.sendto(reply.encode(), addr)


def rpc(proc, method, params=None, req_id=[0]):
    req_id[0] += 1
    msg = {"jsonrpc": "2.0", "id": req_id[0], "method": method}
    if params is not None:
        msg["params"] = params
    proc.stdin.write(json.dumps(msg) + "\n")
    proc.stdin.flush()
    while True:
        line = proc.stdout.readline()
        if not line:
            raise RuntimeError("server closed stdout")
        resp = json.loads(line)
        if resp.get("id") == req_id[0]:
            return resp


def call(proc, name, args):
    resp = rpc(proc, "tools/call", {"name": name, "arguments": args})
    payload = json.loads(resp["result"]["content"][0]["text"])
    return payload, resp["result"].get("isError", False)


DB_PATH = "/tmp/retroarch-trainer-test-cheats.json"
NOTES_PATH = "/tmp/retroarch-trainer-test-notes.md"


def main():
    fake = FakeRetroArch()
    fake.start()
    time.sleep(0.1)

    if __import__("os").path.exists(DB_PATH):
        __import__("os").unlink(DB_PATH)
    if __import__("os").path.exists(NOTES_PATH):
        __import__("os").unlink(NOTES_PATH)
    env = dict(**__import__("os").environ,
               RETROARCH_CMD_PORT=str(PORT),
               RETROARCH_TRAINER_CHEATS=DB_PATH,
               RETROARCH_TRAINER_NOTES=NOTES_PATH)
    server_py = __import__("os").path.join(
        __import__("os").path.dirname(__import__("os").path.abspath(__file__)),
        "retroarch_mcp.py")
    proc = subprocess.Popen(
        [sys.executable, server_py],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, text=True, env=env)

    init = rpc(proc, "initialize", {"protocolVersion": "2025-06-18",
                                    "capabilities": {},
                                    "clientInfo": {"name": "test", "version": "0"}})
    assert "result" in init, init
    print("initialize OK:", init["result"]["serverInfo"],
          "proto:", init["result"]["protocolVersion"])

    tools = rpc(proc, "tools/list")
    names = [t["name"] for t in tools["result"]["tools"]]
    print(f"tools/list OK: {len(names)} tools")
    assert "retroarch_snapshot_ram" in names

    status, err = call(proc, "retroarch_get_status", {})
    assert not err, status
    print("status:", status["status"].strip())
    assert "Chrono Trigger" in status["status"]

    snap1, err = call(proc, "retroarch_snapshot_ram",
                      {"name": "before", "max_bytes": RAM_SIZE})
    assert not err and snap1["size"] == RAM_SIZE, snap1
    print(f"snapshot OK: {snap1['size']} bytes sha1={snap1['sha1'][:12]}...")

    # user spends gold in-game: fake changes 1530 -> 1420 at 0x1234 only
    fake.ram[0x1234] = 0x8C  # 1420 = 0x058C
    fake.ram[0x1235] = 0x05

    snap2, err = call(proc, "retroarch_snapshot_ram",
                      {"name": "after", "max_bytes": RAM_SIZE})
    assert not err, snap2

    diff, err = call(proc, "retroarch_diff_snapshots",
                     {"before": "before", "after": "after"})
    assert not err, diff
    print(f"diff OK: {diff['changed_regions']} region(s):",
          [r["address"] for r in diff["regions"]])
    assert diff["regions"][0]["address"] == "0x1234"

    # fresh snapshot has gold=1420 at 0x1234 and 1530 decoy at 0x9000
    search, err = call(proc, "retroarch_search_memory",
                       {"snapshot": "after", "value": 1420, "size": 2})
    assert not err, search
    print(f"search OK: {search['matches']} match(es): {search['addresses']}")
    assert "0x1234" in search["addresses"] and "0x9000" not in search["addresses"]

    wr, err = call(proc, "retroarch_write_memory",
                   {"address": "0x1234", "data": "0f 27"})  # 9999
    assert not err and wr["written"] == 2, wr
    assert fake.ram[0x1234] == 0x0F and fake.ram[0x1235] == 0x27
    print("write OK: gold set to", fake.ram[0x1234] | (fake.ram[0x1235] << 8))

    rd, err = call(proc, "retroarch_read_memory",
                   {"address": "0x1230", "length": 16})
    assert not err and rd["length"] == 16, rd
    assert "0f 27" in rd["hex"]
    print("read OK: dump verified")

    def wait_paused(want, timeout=2.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if fake.paused == want:
                return True
            time.sleep(0.02)
        return False

    pz, err = call(proc, "retroarch_pause", {"paused": True})
    assert wait_paused(True), pz
    pz, err = call(proc, "retroarch_pause", {"paused": False})
    assert wait_paused(False), pz
    print("pause/resume OK")

    # --- filtered scan (Cheat Engine style) ---
    fake.ram[0x1234] = 0x0F  # gold back to 9999? set 3855... just set known
    fake.ram[0x1235] = 0x27
    sc, err = call(proc, "retroarch_scan_start", {"size": 2, "value": 9999})
    assert not err and sc["candidates"] >= 1, sc
    print(f"scan_start OK: {sc['candidates']} candidate(s) for value 9999")
    # spend gold: 9999 -> 8000 at 0x1234 only
    fake.ram[0x1234] = 0x40  # 8000 = 0x1F40
    fake.ram[0x1235] = 0x1F
    fl, err = call(proc, "retroarch_scan_filter", {"condition": "decreased"})
    assert not err, fl
    addrs = [a["address"] for a in fl["addresses"]]
    assert "0x1234" in addrs, fl
    print(f"scan_filter decreased OK: {fl['candidates']} left -> {addrs[:4]}")
    fl, err = call(proc, "retroarch_scan_filter",
                   {"condition": "equal", "value": 8000})
    assert not err and fl["candidates"] >= 1, fl
    assert any(a["address"] == "0x1234" and a["value"] == 8000
               for a in fl["addresses"]), fl
    print("scan_filter equal OK: value readback verified")
    st, err = call(proc, "retroarch_scan_status", {})
    assert not err and st["active"], st
    print("scan_status OK")

    # --- cheat profile save / apply / freeze ---
    ca, err = call(proc, "retroarch_cheat_add", {
        "name": "Rich", "address": "0x1234", "value": 9999, "size": 2,
        "mode": "set", "notes": "test gold cheat"})
    assert not err and ca["key"] == "deadc0de", ca
    print(f"cheat_add OK: saved id={ca['cheat']['id']} for {ca['game']}")

    ca2, err = call(proc, "retroarch_cheat_add", {
        "name": "God mode", "address": "0x1234", "value": 9999, "size": 2,
        "mode": "freeze", "interval": 0.05})
    assert not err, ca2

    fake.ram[0x1234] = 0x00  # simulate gold spent to 0
    fake.ram[0x1235] = 0x00
    ap, err = call(proc, "retroarch_cheat_apply", {})
    assert not err, ap
    assert fake.ram[0x1234] == 0x0F and fake.ram[0x1235] == 0x27, ap
    statuses = {a["id"]: a["status"] for a in ap["applied"]}
    assert statuses.get("rich") == "set" and statuses.get("god-mode") == "frozen", ap
    print(f"cheat_apply OK: {statuses}")

    # freezer keeps rewriting: game decrements, freezer restores
    time.sleep(0.15)
    fake.ram[0x1234] = 0x00
    fake.ram[0x1235] = 0x00
    time.sleep(0.2)
    assert fake.ram[0x1234] == 0x0F and fake.ram[0x1235] == 0x27, \
        "freezer did not restore value"
    print("freeze loop OK: value restored after game overwrite")

    fz, err = call(proc, "retroarch_freeze", {"action": "list"})
    assert not err and any("god-mode" in t for t in fz["freezers"]), fz
    print(f"freeze list OK: {list(fz['freezers'])}")

    cl, err = call(proc, "retroarch_cheat_list", {})
    assert not err and len(cl["cheats"]) == 2, cl
    print(f"cheat_list OK: {[c['id'] for c in cl['cheats']]}")

    cr, err = call(proc, "retroarch_cheat_remove", {"id": "god-mode"})
    assert not err and cr["removed"] == "god-mode", cr
    fz, err = call(proc, "retroarch_freeze", {"action": "list"})
    assert not err and not any("god-mode" in t for t in fz["freezers"]), fz
    print("cheat_remove OK (freezer stopped too)")

    # persisted DB on disk has the right game key and only 'rich' left
    with open(DB_PATH, encoding="utf-8") as f:
        db = json.load(f)
    assert "deadc0de" in db["games"], db
    ids = [c["id"] for c in db["games"]["deadc0de"]["cheats"]]
    assert ids == ["rich"], ids
    print("cheats.json persistence OK")

    aa, err = call(proc, "retroarch_auto_apply", {"enabled": True})
    assert not err and aa["enabled"], aa
    aa, err = call(proc, "retroarch_auto_apply", {"enabled": False})
    assert not err and not aa["enabled"], aa
    print("auto_apply toggle OK")

    # --- per-game knowledge notes ---
    gn, err = call(proc, "retroarch_game_note", {"action": "show"})
    assert not err and gn["notes"] is None, gn
    print("game_note show OK: empty for unknown game")

    gn, err = call(proc, "retroarch_game_note",
                   {"action": "add",
                    "note": "골드 16-bit LE at 0x1234 (snes9x WRAM)"})
    assert not err and gn["new_section"], gn
    assert "deadc0de" in gn["section"] or "Chrono Trigger" in gn["section"], gn
    print(f"game_note add OK: new section {gn['section']!r}")

    gn, err = call(proc, "retroarch_game_note",
                   {"action": "add", "note": "두번째 노트"})
    assert not err and not gn["new_section"], gn
    print("game_note add OK: appended to existing section")

    gn, err = call(proc, "retroarch_game_note", {"action": "show"})
    assert not err and "0x1234" in gn["notes"] and "두번째 노트" in gn["notes"], gn
    print("game_note show OK: both notes present")

    # get_status now carries known_notes + enriched saved_cheats
    st2, err = call(proc, "retroarch_get_status", {})
    assert not err and "known_notes" in st2, st2
    assert "0x1234" in st2["known_notes"], st2["known_notes"]
    rich = [c for c in st2["saved_cheats"] if c["id"] == "rich"]
    assert rich and rich[0]["address"] == "0x1234" and rich[0]["value"] == 9999, st2
    print("get_status OK: known_notes + saved_cheats details attached")

    # notes file persisted on disk with crc32 marker
    with open(NOTES_PATH, encoding="utf-8") as f:
        md = f.read()
    assert "crc32: `deadc0de`" in md and "0x1234" in md, md
    print("known_addresses.md persistence OK")

    proc.stdin.close()
    proc.wait(timeout=5)
    print("\nALL TESTS PASSED")


if __name__ == "__main__":
    main()
