#!/usr/bin/env python3
"""RetroArch Trainer MCP server.

Bridges MCP (stdio JSON-RPC) to RetroArch's UDP network command interface
(port 55355, `network_cmd_enable = true`).

Lets an external agent play "trainer hacker":
  - inspect the running game (GET_STATUS: system, rom name, crc32)
  - snapshot / diff / search emulated RAM
  - read / write memory
  - save & load states, pause, frame-advance, screenshot for visual verify

No dependencies beyond the Python 3 standard library.
"""

import hashlib
import json
import os
import re
import socket
import sys
import threading
import time

# ---------------------------------------------------------------------------
# Configuration (env overrides)
# ---------------------------------------------------------------------------

RA_HOST = os.environ.get("RETROARCH_CMD_HOST", "127.0.0.1")
RA_PORT = int(os.environ.get("RETROARCH_CMD_PORT", "55355"))
UDP_TIMEOUT = float(os.environ.get("RETROARCH_CMD_TIMEOUT", "0.8"))
READ_CHUNK = 512          # bytes per READ_CORE_MEMORY request
WRITE_CHUNK = 128         # bytes per WRITE_CORE_MEMORY request
MAX_READ = 64 * 1024      # per-call cap for retroarch_read_memory
MAX_SNAPSHOT = 32 * 1024 * 1024  # absolute snapshot cap (PS2 RAM)
MAX_DIFF_REGIONS = 200
MAX_SEARCH_HITS = 500
MAX_SCAN_CANDIDATES_SHOWN = 40

CHEATS_PATH = os.environ.get(
    "RETROARCH_TRAINER_CHEATS",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "cheats.json"))
NOTES_PATH = os.environ.get(
    "RETROARCH_TRAINER_NOTES",
    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                 "known_addresses.md"))
MAX_NOTES_IN_STATUS = 4000  # cap known_notes size in get_status replies
AUTO_APPLY_POLL_SEC = 2.0

SERVER_NAME = "retroarch-trainer"
SERVER_VERSION = "0.3.0"
SUPPORTED_PROTOCOL_VERSIONS = ["2024-11-05", "2025-03-26", "2025-06-18"]

STATE = {
    "snapshots": {},      # name -> bytes
    "snapshot_meta": {},  # name -> {start, taken_at, game}
}

# Cheat Engine-style scan session (see retroarch_scan_start / _scan_filter)
SCAN = None  # {base, size, endian, signed, candidates(list of offsets), prev(bytes), game}

# In-process freezers: tag -> {"stop": Event, "thread": Thread, "spec": dict}
FREEZERS = {}
FREEZERS_LOCK = threading.Lock()

# Per-game cheat DB (loaded lazily from CHEATS_PATH)
CHEAT_DB = None
CHEAT_DB_LOCK = threading.Lock()

AUTO_APPLY = {"thread": None, "stop": None, "last_key": None}

# ---------------------------------------------------------------------------
# Logging (stdout is the MCP channel - never write anything else there)
# ---------------------------------------------------------------------------

def log(msg):
    sys.stderr.write(f"[retroarch-trainer] {msg}\n")
    sys.stderr.flush()

# ---------------------------------------------------------------------------
# RetroArch UDP command interface
# ---------------------------------------------------------------------------

def ra_command(cmd, expect_reply=False, timeout=UDP_TIMEOUT):
    """Send one command line to RetroArch. Returns the reply string or None."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.settimeout(timeout)
        sock.sendto(cmd.encode("utf-8"), (RA_HOST, RA_PORT))
        if not expect_reply:
            return None
        try:
            data, _ = sock.recvfrom(65535)
            return data.decode("utf-8", "replace")
        except socket.timeout:
            return None
    finally:
        sock.close()


def ra_read_memory(address, length):
    """Returns (bytes, error). Reads a single chunk via READ_CORE_MEMORY."""
    if length <= 0:
        return b"", None
    reply = ra_command(f"READ_CORE_MEMORY {address:x} {length}",
                       expect_reply=True)
    if reply is None:
        return None, "no reply (is RetroArch running with network_cmd_enable?)"
    parts = reply.split()
    if len(parts) < 2 or parts[0] != "READ_CORE_MEMORY":
        return None, f"unexpected reply: {reply.strip()!r}"
    if "-1" in parts[2:3] or len(parts) == 2:
        tail = " ".join(parts[2:])
        return None, tail or "no data"
    try:
        return bytes(int(tok, 16) for tok in parts[2:]), None
    except ValueError:
        return None, f"unparseable reply: {reply.strip()!r}"


def ra_write_memory(address, data):
    """Writes bytes in chunks. Returns (written_total, error)."""
    total = 0
    while total < len(data):
        chunk = data[total:total + WRITE_CHUNK]
        hexbytes = " ".join(f"{b:02x}" for b in chunk)
        reply = ra_command(f"WRITE_CORE_MEMORY {address + total:x} {hexbytes}",
                           expect_reply=True)
        if reply is None:
            return total, "no reply"
        parts = reply.split()
        if len(parts) >= 3 and parts[0] == "WRITE_CORE_MEMORY":
            try:
                n = int(parts[-1])
            except ValueError:
                return total, f"unparseable reply: {reply.strip()!r}"
            if n == 0:
                tail = " ".join(parts[2:-1])
                return total, tail or "write failed (readonly or no descriptor)"
            total += n
            if n < len(chunk):
                return total, "write truncated by memory descriptor"
        else:
            return total, f"unexpected reply: {reply.strip()!r}"
    return total, None


def ra_snapshot(start, max_bytes, progress=None):
    """Reads RAM from `start` until an error or short read. Returns (bytes, error)."""
    out = bytearray()
    addr = start
    while len(out) < max_bytes:
        want = min(READ_CHUNK, max_bytes - len(out))
        data, err = ra_read_memory(addr, want)
        if err:
            if out:
                break  # treat end-of-descriptor as end of snapshot
            return None, f"read failed at 0x{addr:x}: {err}"
        out.extend(data)
        addr += len(data)
        if len(data) < want:
            break  # clamped by descriptor: end of memory
    return bytes(out), None

# ---------------------------------------------------------------------------
# Game identity (per-game cheat profiles are keyed by this)
# ---------------------------------------------------------------------------

def parse_game_identity(status_reply):
    """Parses 'GET_STATUS PLAYING snes9x,Rom Name.sfc,crc32=deadc0de'.
    Returns dict {state, core, rom, crc32, key} or None."""
    if not status_reply:
        return None
    text = status_reply.strip()
    m = re.match(r"^(?:GET_STATUS\s+)?(\w+)\s+(.*)$", text)
    if not m:
        return None
    state, rest = m.group(1), m.group(2)
    crc = None
    m_crc = re.search(r"(?:^|,)crc32=([0-9a-fA-F]+)", rest)
    if m_crc:
        crc = m_crc.group(1).lower()
        rest = rest[:m_crc.start()].rstrip(",")
    core, _, rom = rest.partition(",")
    rom = rom.strip().strip('"')
    core = core.strip()
    if state.upper() not in ("PLAYING", "PAUSED"):
        return {"state": state, "core": None, "rom": None, "crc32": None,
                "key": None}
    key = crc or (f"rom:{rom}" if rom else None)
    return {"state": state, "core": core or None, "rom": rom or None,
            "crc32": crc, "key": key}


def current_game():
    """Returns (identity_dict, error)."""
    reply = ra_command("GET_STATUS", expect_reply=True)
    if reply is None:
        return None, "no reply from RetroArch"
    ident = parse_game_identity(reply)
    if not ident or not ident.get("key"):
        return None, f"no content running? status: {reply.strip()!r}"
    return ident, None

# ---------------------------------------------------------------------------
# Per-game cheat profile DB (cheats.json)
# ---------------------------------------------------------------------------

def cheat_db_load():
    global CHEAT_DB
    with CHEAT_DB_LOCK:
        if CHEAT_DB is None:
            try:
                with open(CHEATS_PATH, "r", encoding="utf-8") as f:
                    CHEAT_DB = json.load(f)
            except (OSError, json.JSONDecodeError):
                CHEAT_DB = {}
            CHEAT_DB.setdefault("settings", {})
            CHEAT_DB.setdefault("games", {})
        return CHEAT_DB


def cheat_db_save():
    with CHEAT_DB_LOCK:
        db = CHEAT_DB or {"settings": {}, "games": {}}
        tmp = CHEATS_PATH + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(db, f, ensure_ascii=False, indent=2)
        os.replace(tmp, CHEATS_PATH)


def cheat_db_game_entry(ident, create=False):
    db = cheat_db_load()
    games = db["games"]
    key = ident["key"]
    entry = games.get(key)
    if entry is None and create:
        entry = {"rom": ident.get("rom"), "core": ident.get("core"),
                 "crc32": ident.get("crc32"), "cheats": []}
        games[key] = entry
    elif entry is not None:
        # keep rom/core fresh (renamed roms etc.)
        if ident.get("rom"):
            entry["rom"] = ident["rom"]
        if ident.get("core"):
            entry["core"] = ident["core"]
    return key, entry


def slugify(name):
    s = re.sub(r"[^0-9a-zA-Z가-힣]+", "-", str(name)).strip("-").lower()
    return s or "cheat"

# ---------------------------------------------------------------------------
# Per-game knowledge notes (known_addresses.md)
# ---------------------------------------------------------------------------

NOTES_LOCK = threading.Lock()
_CRC_RE = re.compile(r"crc32:\s*`?([0-9a-fA-F]{8})`?")


def notes_split_sections(text):
    """Split notes markdown into (preamble_lines, [[header, body_lines]]).
    A section starts at a '## ' header; '### ' stays inside the body."""
    preamble, sections, current = [], [], None
    for line in text.splitlines():
        if line.startswith("## "):
            current = [line, []]
            sections.append(current)
        elif current is not None:
            current[1].append(line)
        else:
            preamble.append(line)
    return preamble, sections


def notes_find_section(sections, ident):
    """Index of the section matching ident (crc32 first, then rom name
    substring in the header), or None."""
    crc = (ident or {}).get("crc32")
    rom = (ident or {}).get("rom")
    if crc:
        for i, (_header, body) in enumerate(sections):
            for line in body:
                m = _CRC_RE.search(line)
                if m and m.group(1).lower() == crc:
                    return i
    if rom:
        rom_l = rom.lower()
        for i, (header, _body) in enumerate(sections):
            if rom_l in header.lower():
                return i
    return None


def game_notes_read(ident):
    """Returns the markdown section matching the game, or None."""
    try:
        with open(NOTES_PATH, "r", encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return None
    _pre, sections = notes_split_sections(text)
    idx = notes_find_section(sections, ident)
    if idx is None:
        return None
    header, body = sections[idx]
    return "\n".join([header] + body).strip()


def game_notes_append(ident, note):
    """Appends a dated bullet to the game's section (created if missing).
    Returns (section_header, created_new_section)."""
    with NOTES_LOCK:
        try:
            with open(NOTES_PATH, "r", encoding="utf-8") as f:
                text = f.read()
        except OSError:
            text = ("# Known Memory Addresses (per game)\n\n"
                    "트레이너 실험으로 발굴한 주소/공략 노트 모음. "
                    "GET_STATUS의 crc32로 게임 식별.\n")
        preamble, sections = notes_split_sections(text)
        dated = f"- ({time.strftime('%Y-%m-%d')}) {note}"
        idx = notes_find_section(sections, ident)
        created = False
        if idx is None:
            created = True
            header = f"## {ident.get('rom') or ident.get('key') or 'unknown game'}"
            body = []
            if ident.get("crc32"):
                body.append(f"- crc32: `{ident['crc32']}`")
            if ident.get("core"):
                body.append(f"- core: {ident['core']}")
            body += ["", dated]
            sections.append([header, body])
            idx = len(sections) - 1
        else:
            body = sections[idx][1]
            while body and not body[-1].strip():
                body.pop()
            body.append(dated)
        out = list(preamble)
        if out and out[-1].strip():
            out.append("")
        for header, body in sections:
            out.append(header)
            out.extend(body)
            out.append("")
        tmp = NOTES_PATH + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            f.write("\n".join(out).rstrip() + "\n")
        os.replace(tmp, NOTES_PATH)
        return sections[idx][0], created

# ---------------------------------------------------------------------------
# In-process freezer (freeze = rewrite a value in a loop)
# ---------------------------------------------------------------------------

def _freeze_loop(tag, address, data, interval, stop_event):
    log(f"freezer '{tag}' started: 0x{address:x} <- {data.hex(' ')} "
        f"every {interval}s")
    while not stop_event.is_set():
        ra_write_memory(address, data)
        stop_event.wait(interval)
    log(f"freezer '{tag}' stopped")


def freeze_start(tag, address, data, interval):
    with FREEZERS_LOCK:
        old = FREEZERS.get(tag)
        if old:
            old["stop"].set()
        stop_event = threading.Event()
        th = threading.Thread(target=_freeze_loop,
                              args=(tag, address, data, interval, stop_event),
                              daemon=True)
        FREEZERS[tag] = {"stop": stop_event, "thread": th,
                         "spec": {"address": f"0x{address:x}",
                                  "data": data.hex(" "),
                                  "interval": interval,
                                  "started_at": time.time()}}
        th.start()


def freeze_stop(tag):
    with FREEZERS_LOCK:
        entry = FREEZERS.pop(tag, None)
    if entry:
        entry["stop"].set()
        return True
    return False


def freeze_stop_all():
    with FREEZERS_LOCK:
        tags = list(FREEZERS)
    stopped = [t for t in tags if freeze_stop(t)]
    return stopped


def freeze_list():
    with FREEZERS_LOCK:
        return {tag: dict(entry["spec"]) for tag, entry in FREEZERS.items()}

# ---------------------------------------------------------------------------
# Cheat application (set once / start freeze) for a game profile
# ---------------------------------------------------------------------------

def cheat_value_bytes(cheat):
    size = int(cheat.get("size", 1))
    endian = cheat.get("endian", "little")
    value = int(cheat.get("value", 0))
    return value.to_bytes(size, endian, signed=value < 0)


def apply_cheat_entry(key, cheat):
    """Applies one cheat dict. Returns (status_string, detail)."""
    address = parse_address(cheat["address"])
    mode = cheat.get("mode", "set")
    data = cheat_value_bytes(cheat)
    if mode == "freeze":
        tag = f"{key}:{cheat['id']}"
        freeze_start(tag, address, data, float(cheat.get("interval", 0.1)))
        return "frozen", tag
    written, err = ra_write_memory(address, data)
    if err:
        return "error", err
    return "set", f"wrote {written} byte(s)"


def apply_cheats_for_key(key, only_id=None):
    db = cheat_db_load()
    entry = db["games"].get(key)
    if not entry:
        return None
    applied, skipped, errors = [], [], []
    for cheat in entry.get("cheats", []):
        if only_id and cheat.get("id") != only_id:
            continue
        if not cheat.get("enabled", True):
            skipped.append(cheat.get("id"))
            continue
        status, detail = apply_cheat_entry(key, cheat)
        rec = {"id": cheat.get("id"), "status": status, "detail": detail}
        (errors if status == "error" else applied).append(rec)
    return {"game": entry.get("rom"), "key": key,
            "applied": applied, "skipped_disabled": skipped,
            "errors": errors}

# ---------------------------------------------------------------------------
# Auto-apply watcher: applies a game's profile when it starts
# ---------------------------------------------------------------------------

def _auto_apply_loop(stop_event):
    log("auto-apply watcher started")
    while not stop_event.wait(AUTO_APPLY_POLL_SEC):
        ident, err = current_game()
        if err:
            continue
        key = ident["key"]
        if key == AUTO_APPLY["last_key"]:
            continue
        AUTO_APPLY["last_key"] = key
        stopped = freeze_stop_all()  # game changed: drop old freezers
        if stopped:
            log(f"auto-apply: stopped freezers {stopped}")
        result = apply_cheats_for_key(key)
        if result and result["applied"]:
            log(f"auto-apply: {key} -> "
                f"{[a['id'] for a in result['applied']]}")
    log("auto-apply watcher stopped")


def auto_apply_set_enabled(enabled):
    db = cheat_db_load()
    db["settings"]["auto_apply"] = bool(enabled)
    cheat_db_save()
    running = AUTO_APPLY["thread"] is not None and AUTO_APPLY["thread"].is_alive()
    if enabled and not running:
        stop_event = threading.Event()
        AUTO_APPLY["stop"] = stop_event
        AUTO_APPLY["last_key"] = None
        th = threading.Thread(target=_auto_apply_loop, args=(stop_event,),
                              daemon=True)
        AUTO_APPLY["thread"] = th
        th.start()
        return "started"
    if not enabled and running:
        AUTO_APPLY["stop"].set()
        AUTO_APPLY["thread"] = None
        freeze_stop_all()
        return "stopped"
    return "already " + ("running" if enabled else "off")


def auto_apply_start_if_enabled():
    db = cheat_db_load()
    if db["settings"].get("auto_apply"):
        auto_apply_set_enabled(True)

# ---------------------------------------------------------------------------
# Screenshot helper
# ---------------------------------------------------------------------------

SCREENSHOT_DIR_CANDIDATES = [
    os.path.expanduser("~/Library/Application Support/RetroArch/screenshots"),
    os.path.expanduser("~/Documents/RetroArch/screenshots"),
    os.path.expanduser("~/.config/retroarch/screenshots"),
]


def find_screenshot_dir():
    env_dir = os.environ.get("RETROARCH_SCREENSHOT_DIR")
    if env_dir and os.path.isdir(env_dir):
        return env_dir
    # Try to parse retroarch.cfg for screenshot_directory
    cfg_candidates = [
        os.path.expanduser("~/Library/Application Support/RetroArch/config/retroarch.cfg"),
        os.path.expanduser("~/.config/retroarch/retroarch.cfg"),
    ]
    for cfg in cfg_candidates:
        try:
            with open(cfg, "r", encoding="utf-8", errors="replace") as f:
                for line in f:
                    m = re.match(r'\s*screenshot_directory\s*=\s*"(.+)"', line)
                    if m:
                        d = os.path.expanduser(m.group(1))
                        if d != ":\\" and os.path.isdir(d):
                            return d
        except OSError:
            continue
    for d in SCREENSHOT_DIR_CANDIDATES:
        if os.path.isdir(d):
            return d
    return None

# ---------------------------------------------------------------------------
# Tool implementations
# ---------------------------------------------------------------------------

def parse_address(v):
    """Accepts int (decimal) or string (hex, optional 0x prefix)."""
    if isinstance(v, int):
        if v < 0:
            raise ValueError("address must be >= 0")
        return v
    s = str(v).strip().lower()
    if s.startswith("0x"):
        return int(s, 16)
    return int(s, 16)  # strings are always hex


def parse_bytes_arg(data):
    """Accepts a list of ints or a hex string like '3f 00 9c'."""
    if isinstance(data, list):
        bs = bytes(int(b) & 0xFF for b in data)
    else:
        toks = re.split(r"[\s,]+", str(data).strip())
        bs = bytes(int(t, 16) & 0xFF for t in toks if t)
    if not bs:
        raise ValueError("empty data")
    return bs


def hexdump(data, base=0, width=16):
    lines = []
    for off in range(0, len(data), width):
        row = data[off:off + width]
        hexs = " ".join(f"{b:02x}" for b in row)
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in row)
        lines.append(f"{base + off:08x}  {hexs:<{width * 3}} {asc}")
    return "\n".join(lines)


def tool_get_status(_args):
    reply = ra_command("GET_STATUS", expect_reply=True)
    version = ra_command("VERSION", expect_reply=True)
    if reply is None:
        return {"error": "No reply from RetroArch. Is it running with "
                         "'Network Commands' enabled (Settings > Network)?"},
    out = {"status": reply.strip()}
    ident = parse_game_identity(reply)
    if ident:
        out["game"] = ident
        db = cheat_db_load()
        entry = db["games"].get(ident["key"] or "")
        if entry:
            out["saved_cheats"] = [
                {"id": c.get("id"), "name": c.get("name"),
                 "address": c.get("address"), "value": c.get("value"),
                 "size": c.get("size"), "mode": c.get("mode"),
                 "enabled": c.get("enabled", True),
                 "notes": c.get("notes")}
                for c in entry.get("cheats", [])]
        notes = game_notes_read(ident)
        if notes:
            if len(notes) > MAX_NOTES_IN_STATUS:
                notes = (notes[:MAX_NOTES_IN_STATUS] +
                         "\n... (truncated; full text in known_addresses.md)")
            out["known_notes"] = notes
            out["known_notes_source"] = NOTES_PATH
    if version:
        out["retroarch_version"] = version.strip()
    return out


def tool_game_note(args):
    action = str(args.get("action", "show")).lower()
    ident, err = current_game()
    if err:
        return {"error": err}
    if action == "show":
        notes = game_notes_read(ident)
        if notes is None:
            return {"game": ident, "notes": None,
                    "hint": "no notes yet for this game; "
                            "use action='add' to record discoveries"}
        return {"game": ident, "notes": notes, "source": NOTES_PATH}
    if action == "add":
        note = str(args.get("note", "")).strip()
        if not note:
            return {"error": "missing 'note'"}
        header, created = game_notes_append(ident, note)
        return {"game": ident, "added": note, "section": header,
                "new_section": created, "source": NOTES_PATH}
    return {"error": f"unknown action: {action!r} (use 'show' or 'add')"}


def tool_command(args):
    cmd = str(args.get("command", "")).strip()
    if not cmd:
        return {"error": "missing 'command'"}
    if "\n" in cmd:
        return {"error": "one command at a time"}
    verb = cmd.split()[0].upper()
    replies_expected = verb in (
        "GET_STATUS", "GET_CONFIG_PARAM", "VERSION",
        "READ_CORE_MEMORY", "WRITE_CORE_MEMORY",
        "READ_CORE_RAM", "WRITE_CORE_RAM")
    reply = ra_command(cmd, expect_reply=replies_expected)
    if replies_expected and reply is None:
        return {"error": f"no reply to {verb}"}
    return {"sent": cmd, "reply": reply.strip() if reply else None}


def tool_read_memory(args):
    address = parse_address(args.get("address", 0))
    length = int(args.get("length", 256))
    if length > MAX_READ:
        length = MAX_READ
    out = bytearray()
    addr = address
    while len(out) < length:
        want = min(READ_CHUNK, length - len(out))
        data, err = ra_read_memory(addr, want)
        if err:
            if out:
                break
            return {"error": f"read failed at 0x{addr:x}: {err}"}
        out.extend(data)
        addr += len(data)
        if len(data) < want:
            break
    return {"address": f"0x{address:x}", "length": len(out),
            "hex": " ".join(f"{b:02x}" for b in out),
            "dump": hexdump(bytes(out), base=address)}


def tool_write_memory(args):
    address = parse_address(args.get("address", 0))
    data = parse_bytes_arg(args.get("data", ""))
    written, err = ra_write_memory(address, data)
    res = {"address": f"0x{address:x}", "requested": len(data),
           "written": written}
    if err:
        res["warning"] = err
    return res


def tool_snapshot_ram(args):
    name = str(args.get("name", "snap"))
    start = parse_address(args.get("start", 0))
    max_bytes = int(args.get("max_bytes", 2 * 1024 * 1024))
    if max_bytes > MAX_SNAPSHOT:
        max_bytes = MAX_SNAPSHOT
    status = ra_command("GET_STATUS", expect_reply=True)
    data, err = ra_snapshot(start, max_bytes)
    if err:
        return {"error": err}
    STATE["snapshots"][name] = data
    STATE["snapshot_meta"][name] = {
        "start": start, "taken_at": time.time(),
        "game": status.strip() if status else "unknown",
    }
    return {"name": name, "size": len(data),
            "sha1": hashlib.sha1(data).hexdigest(),
            "game": STATE["snapshot_meta"][name]["game"],
            "note": "snapshot cached server-side; use diff/search tools on it"}


def tool_diff_snapshots(args):
    before_name = str(args.get("before", ""))
    after_name = str(args.get("after", ""))
    before = STATE["snapshots"].get(before_name)
    after = STATE["snapshots"].get(after_name)
    if before is None or after is None:
        return {"error": f"unknown snapshot(s); have: "
                         f"{sorted(STATE['snapshots'])} or take new ones"}
    base = STATE["snapshot_meta"].get(before_name, {}).get("start", 0)
    n = min(len(before), len(after))
    regions = []
    region_start = None
    for i in range(n):
        if before[i] != after[i]:
            if region_start is None:
                region_start = i
        else:
            if region_start is not None:
                regions.append((region_start, i))
                region_start = None
    if region_start is not None:
        regions.append((region_start, n))
    out = []
    for s, e in regions[:MAX_DIFF_REGIONS]:
        out.append({
            "address": f"0x{base + s:x}",
            "length": e - s,
            "before": before[s:e].hex(" "),
            "after": after[s:e].hex(" "),
        })
    return {"changed_regions": len(regions), "showing": len(out),
            "regions": out,
            "hint": "narrow candidates: snapshot again after another in-game "
                    "change, then re-diff or search within these addresses"}


def tool_search_memory(args):
    snap_name = str(args.get("snapshot", ""))
    snap = STATE["snapshots"].get(snap_name)
    if snap is None:
        return {"error": f"unknown snapshot; have: {sorted(STATE['snapshots'])}"}
    base = STATE["snapshot_meta"].get(snap_name, {}).get("start", 0)
    value = args.get("value")
    size = int(args.get("size", 1))
    endian = str(args.get("endian", "little"))
    signed = bool(args.get("signed", False))
    if size not in (1, 2, 3, 4, 8):
        return {"error": "size must be 1, 2, 3, 4 or 8 bytes"}
    needle = int(value).to_bytes(size, endian, signed=signed)
    hits = []
    pos = 0
    while True:
        idx = snap.find(needle, pos)
        if idx < 0:
            break
        hits.append(f"0x{base + idx:x}")
        pos = idx + 1
        if len(hits) >= MAX_SEARCH_HITS:
            break
    total_note = ("truncated" if len(hits) >= MAX_SEARCH_HITS
                  else f"{len(hits)} total")
    return {"value": value, "size": size, "endian": endian,
            "matches": len(hits), "total": total_note, "addresses": hits,
            "hint": "if too many matches: take another snapshot after the "
                    "value changes in-game, then diff_snapshots or search "
                    "the new snapshot and intersect the addresses"}


# ---------------------------------------------------------------------------
# Cheat Engine-style filtered scan (find values you can't see exactly)
# ---------------------------------------------------------------------------

def _scan_read_value(buf, off, size, endian, signed):
    return int.from_bytes(buf[off:off + size], endian, signed=signed)


def tool_scan_start(args):
    """Initializes a scan session: snapshots RAM and (optionally) seeds
    candidates with an exact-value first scan."""
    global SCAN
    size = int(args.get("size", 1))
    endian = str(args.get("endian", "little"))
    signed = bool(args.get("signed", False))
    if size not in (1, 2, 3, 4, 8):
        return {"error": "size must be 1, 2, 3, 4 or 8 bytes"}
    start = parse_address(args.get("start", 0))
    max_bytes = int(args.get("max_bytes", 2 * 1024 * 1024))
    if max_bytes > MAX_SNAPSHOT:
        max_bytes = MAX_SNAPSHOT
    ident, _ = current_game()
    data, err = ra_snapshot(start, max_bytes)
    if err:
        return {"error": err}
    value = args.get("value")
    if value is not None:
        needle = int(value).to_bytes(size, endian, signed=signed or int(value) < 0)
        candidates = []
        pos = 0
        while True:
            idx = data.find(needle, pos)
            if idx < 0:
                break
            candidates.append(idx)
            pos = idx + 1
    else:
        candidates = list(range(0, len(data) - size + 1))
    SCAN = {"base": start, "size": size, "endian": endian,
            "signed": signed, "candidates": candidates, "prev": data,
            "game": (ident or {}).get("rom"),
            "started_at": time.time()}
    return {"candidates": len(candidates), "region_size": len(data),
            "size": size, "endian": endian, "signed": signed,
            "first_scan_value": value,
            "game": SCAN["game"],
            "hint": "change the value in-game (or wait), then call "
                    "retroarch_scan_filter with a condition: equal, "
                    "not_equal, changed, unchanged, increased, decreased, "
                    "increased_by, decreased_by, between"}


def tool_scan_filter(args):
    """Takes a fresh snapshot of the scanned region and narrows candidates."""
    global SCAN
    if not SCAN:
        return {"error": "no active scan; call retroarch_scan_start first"}
    condition = str(args.get("condition", "changed"))
    valid = ("equal", "not_equal", "changed", "unchanged", "increased",
             "decreased", "increased_by", "decreased_by", "between")
    if condition not in valid:
        return {"error": f"condition must be one of {valid}"}
    value = args.get("value")
    value2 = args.get("value2")
    needs_value = ("equal", "not_equal", "increased_by", "decreased_by",
                   "between")
    if condition in needs_value and value is None:
        return {"error": f"condition '{condition}' needs 'value'"}
    if condition == "between" and value2 is None:
        return {"error": "condition 'between' needs 'value' and 'value2'"}
    data, err = ra_snapshot(SCAN["base"], len(SCAN["prev"]))
    if err:
        return {"error": err}
    if len(data) != len(SCAN["prev"]):
        return {"error": "memory region size changed; restart the scan"}
    size, endian, signed = SCAN["size"], SCAN["endian"], SCAN["signed"]
    prev = SCAN["prev"]
    keep = []
    for off in SCAN["candidates"]:
        cur = _scan_read_value(data, off, size, endian, signed)
        old = _scan_read_value(prev, off, size, endian, signed)
        if condition == "equal":
            ok = cur == int(value)
        elif condition == "not_equal":
            ok = cur != int(value)
        elif condition == "changed":
            ok = cur != old
        elif condition == "unchanged":
            ok = cur == old
        elif condition == "increased":
            ok = cur > old
        elif condition == "decreased":
            ok = cur < old
        elif condition == "increased_by":
            ok = cur - old == int(value)
        elif condition == "decreased_by":
            ok = old - cur == int(value)
        else:  # between
            ok = int(value) <= cur <= int(value2)
        if ok:
            keep.append(off)
    SCAN["candidates"] = keep
    SCAN["prev"] = data
    out = {"condition": condition, "candidates": len(keep)}
    shown = keep[:MAX_SCAN_CANDIDATES_SHOWN]
    out["addresses"] = [
        {"address": f"0x{SCAN['base'] + off:x}",
         "value": _scan_read_value(data, off, size, endian, signed)}
        for off in shown]
    if len(keep) > len(shown):
        out["note"] = f"showing first {len(shown)}; keep filtering to narrow"
    else:
        out["note"] = ("all candidates shown; verify by writing with "
                       "retroarch_write_memory (take retroarch_save_state "
                       "first) or freeze the winner")
    return out


def tool_scan_status(_args):
    if not SCAN:
        return {"active": False}
    out = {"active": True, "game": SCAN["game"],
           "candidates": len(SCAN["candidates"]),
           "size": SCAN["size"], "endian": SCAN["endian"],
           "region": f"0x{SCAN['base']:x}+0x{len(SCAN['prev']):x}"}
    data, err = ra_snapshot(SCAN["base"], len(SCAN["prev"]))
    if not err:
        shown = SCAN["candidates"][:MAX_SCAN_CANDIDATES_SHOWN]
        out["addresses"] = [
            {"address": f"0x{SCAN['base'] + off:x}",
             "value": _scan_read_value(data, off, SCAN["size"],
                                       SCAN["endian"], SCAN["signed"])}
            for off in shown]
    return out

# ---------------------------------------------------------------------------
# Per-game cheat profile tools
# ---------------------------------------------------------------------------

def tool_cheat_add(args):
    """Adds/updates a named cheat for the CURRENT game and persists it."""
    name = args.get("name")
    if not name:
        return {"error": "missing 'name'"}
    if "address" not in args:
        return {"error": "missing 'address'"}
    if "value" not in args:
        return {"error": "missing 'value'"}
    ident, err = current_game()
    if err:
        return {"error": err}
    mode = str(args.get("mode", "set"))
    if mode not in ("set", "freeze"):
        return {"error": "mode must be 'set' or 'freeze'"}
    key, entry = cheat_db_game_entry(ident, create=True)
    cheat_id = str(args.get("id") or slugify(name))
    cheat = {
        "id": cheat_id,
        "name": str(name),
        "address": f"0x{parse_address(args['address']):x}",
        "value": int(args["value"]),
        "size": int(args.get("size", 1)),
        "endian": str(args.get("endian", "little")),
        "mode": mode,
        "interval": float(args.get("interval", 0.1)),
        "enabled": bool(args.get("enabled", True)),
        "notes": str(args.get("notes", "")),
        "updated_at": time.time(),
    }
    if cheat["size"] not in (1, 2, 3, 4, 8):
        return {"error": "size must be 1, 2, 3, 4 or 8 bytes"}
    cheats = entry.setdefault("cheats", [])
    replaced = False
    for i, old in enumerate(cheats):
        if old.get("id") == cheat_id:
            cheats[i] = cheat
            replaced = True
            break
    if not replaced:
        cheats.append(cheat)
    cheat_db_save()
    apply_now = bool(args.get("apply_now", False))
    result = {"game": entry.get("rom"), "key": key,
              "cheat": {k: v for k, v in cheat.items() if k != "updated_at"},
              "replaced_existing": replaced,
              "saved_to": CHEATS_PATH}
    if apply_now:
        result["apply"] = apply_cheats_for_key(key, only_id=cheat_id)
    return result


def tool_cheat_list(args):
    db = cheat_db_load()
    scope = str(args.get("game", "current"))
    if scope == "all":
        return {"games": db["games"],
                "settings": db["settings"],
                "path": CHEATS_PATH}
    ident, err = current_game()
    if err:
        return {"error": err}
    key = ident["key"]
    entry = db["games"].get(key)
    return {"game": ident, "key": key,
            "cheats": entry.get("cheats", []) if entry else [],
            "freezers_running": freeze_list(),
            "auto_apply": db["settings"].get("auto_apply", False)}


def tool_cheat_remove(args):
    cheat_id = args.get("id") or (slugify(args["name"]) if args.get("name")
                                  else None)
    if not cheat_id:
        return {"error": "missing 'id' (or 'name')"}
    ident, err = current_game()
    if err:
        return {"error": err}
    key, entry = cheat_db_game_entry(ident)
    if not entry:
        return {"error": f"no profile for this game (key {key})"}
    cheats = entry.get("cheats", [])
    kept = [c for c in cheats if c.get("id") != cheat_id]
    if len(kept) == len(cheats):
        return {"error": f"no cheat with id '{cheat_id}'",
                "have": [c.get("id") for c in cheats]}
    entry["cheats"] = kept
    cheat_db_save()
    freeze_stop(f"{key}:{cheat_id}")
    return {"removed": cheat_id, "remaining": len(kept)}


def tool_cheat_apply(args):
    ident, err = current_game()
    if err:
        return {"error": err}
    key = ident["key"]
    only_id = None
    if args.get("id") or args.get("name"):
        only_id = str(args.get("id") or slugify(args["name"]))
    result = apply_cheats_for_key(key, only_id=only_id)
    if result is None:
        return {"error": f"no saved profile for this game (key {key}); "
                         "add one with retroarch_cheat_add"}
    if only_id and not result["applied"] and not result["errors"]:
        result["note"] = (f"no enabled cheat with id '{only_id}' "
                          f"(skipped: {result['skipped_disabled']})")
    return result


def tool_freeze(args):
    """Ad-hoc freezer control (not saved to the profile)."""
    action = str(args.get("action", "list"))
    if action == "list":
        return {"freezers": freeze_list()}
    if action == "stop":
        tag = args.get("tag")
        if not tag or str(tag) == "all":
            return {"stopped": freeze_stop_all()}
        if freeze_stop(str(tag)):
            return {"stopped": [str(tag)]}
        return {"error": f"no freezer tagged '{tag}'",
                "running": list(freeze_list())}
    if action == "start":
        if "address" not in args or "value" not in args:
            return {"error": "start needs 'address' and 'value'"}
        address = parse_address(args["address"])
        value = int(args["value"])
        size = int(args.get("size", 2 if value > 0xFF else 1))
        endian = str(args.get("endian", "little"))
        interval = float(args.get("interval", 0.1))
        tag = str(args.get("tag") or f"adhoc:{address:x}")
        data = value.to_bytes(size, endian, signed=value < 0)
        freeze_start(tag, address, data, interval)
        return {"started": tag, "address": f"0x{address:x}",
                "data": data.hex(" "), "interval": interval,
                "note": "freezer runs while this MCP server (pi session) "
                        "lives; save it with retroarch_cheat_add "
                        "mode='freeze' for persistence"}
    return {"error": "action must be start, stop or list"}


def tool_auto_apply(args):
    enabled = args.get("enabled")
    if enabled is None:
        db = cheat_db_load()
        running = (AUTO_APPLY["thread"] is not None
                   and AUTO_APPLY["thread"].is_alive())
        return {"enabled_in_settings": db["settings"].get("auto_apply", False),
                "watcher_running": running,
                "last_game_key": AUTO_APPLY["last_key"]}
    status = auto_apply_set_enabled(bool(enabled))
    return {"enabled": bool(enabled), "watcher": status,
            "note": "when a game starts, its saved enabled cheats are "
                    "applied automatically and previous freezers stopped"}


def tool_snapshot_info(_args):
    return {name: {"size": len(STATE["snapshots"][name]),
                   **{k: (f"0x{v:x}" if k == "start" else v)
                      for k, v in meta.items()}}
            for name, meta in STATE["snapshot_meta"].items()}


def tool_screenshot(args):
    directory = args.get("directory") or find_screenshot_dir()
    if not directory or not os.path.isdir(directory):
        return {"error": "screenshot directory not found; set "
                         "RETROARCH_SCREENSHOT_DIR or pass 'directory'"}
    before = set()
    try:
        before = {os.path.join(directory, f)
                  for f in os.listdir(directory) if f.endswith(".png")}
    except OSError:
        pass
    newest_before = None
    if before:
        newest_before = max(before, key=os.path.getmtime)
        newest_mtime = os.path.getmtime(newest_before)
    else:
        newest_mtime = 0
    ra_command("SCREENSHOT")
    deadline = time.time() + 3.0
    while time.time() < deadline:
        try:
            pngs = [os.path.join(directory, f)
                    for f in os.listdir(directory) if f.endswith(".png")]
        except OSError:
            pngs = []
        fresh = [p for p in pngs if os.path.getmtime(p) > newest_mtime + 1e-6
                 or p not in before]
        if fresh:
            path = max(fresh, key=os.path.getmtime)
            time.sleep(0.2)  # let the file finish writing
            return {"path": path,
                    "note": "open this image to visually verify the result"}
        time.sleep(0.15)
    return {"error": "no new screenshot appeared",
            "directory": directory,
            "hint": "check that a screenshot directory is configured"}


def tool_save_state(args):
    slot = args.get("slot")
    if slot is None:
        ra_command("SAVE_STATE")
        return {"sent": "SAVE_STATE"}
    ra_command(f"SAVE_STATE_SLOT {int(slot)}")
    return {"sent": f"SAVE_STATE_SLOT {int(slot)}"}


def tool_load_state(args):
    slot = args.get("slot")
    if slot is None:
        ra_command("LOAD_STATE")
        return {"sent": "LOAD_STATE"}
    ra_command(f"LOAD_STATE_SLOT {int(slot)}")
    return {"sent": f"LOAD_STATE_SLOT {int(slot)}"}


def tool_pause(args):
    paused = args.get("paused")
    if paused is None:
        ra_command("PAUSE_TOGGLE")
        return {"sent": "PAUSE_TOGGLE"}
    status = ra_command("GET_STATUS", expect_reply=True) or ""
    is_paused = "PAUSED" in status.split()[1:2]
    if is_paused != bool(paused):
        ra_command("PAUSE_TOGGLE")
    return {"paused": bool(paused)}


def tool_frame_advance(args):
    frames = int(args.get("frames", 1))
    frames = max(1, min(frames, 600))
    ra_command("PAUSE_TOGGLE")  # ensure paused? FRAMEADVANCE works from pause
    for _ in range(frames):
        ra_command("FRAMEADVANCE")
        time.sleep(0.01)
    return {"advanced_frames": frames,
            "note": "game is paused; use retroarch_pause to resume"}

# ---------------------------------------------------------------------------
# Tool registry
# ---------------------------------------------------------------------------

TOOLS = [
    {
        "name": "retroarch_get_status",
        "description": "Get the currently running game: state (PLAYING/PAUSED), "
                       "core/system id, rom filename and crc32. The reply also "
                       "includes everything already known about this game: "
                       "'saved_cheats' (address/value/mode from cheats.json) "
                       "and 'known_notes' (discovered memory addresses, core "
                       "quirks and hints from known_addresses.md). Use the "
                       "game name to recall additional known memory maps, "
                       "cheat codes or encodings for that game.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "retroarch_snapshot_ram",
        "description": "Read emulated RAM into a named server-side snapshot. "
                       "Typical trainer workflow: snapshot before and after an "
                       "in-game change (e.g. lose one life), then "
                       "retroarch_diff_snapshots to find the address. WRAM "
                       "usually starts at 0x0; sizes: NES 2KB, SNES 128KB, "
                       "GBA 288KB, PS1 2MB.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {"type": "string", "description": "snapshot name"},
                "start": {"description": "start address (hex string or int)",
                          "type": ["string", "integer"], "default": 0},
                "max_bytes": {"type": "integer",
                              "description": "cap (default 2MB)"},
            },
        },
    },
    {
        "name": "retroarch_diff_snapshots",
        "description": "Compare two snapshots and return only the changed "
                       "memory regions (address, before/after bytes). The "
                       "classic way to locate a value: diff across one known "
                       "change, then re-diff after another change to narrow "
                       "to a single candidate.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "before": {"type": "string"},
                "after": {"type": "string"},
            },
            "required": ["before", "after"],
        },
    },
    {
        "name": "retroarch_search_memory",
        "description": "Search a snapshot for an exact value (the user tells "
                       "you a visible number, e.g. 1530 gold). Returns "
                       "candidate addresses. If there are too many, take "
                       "another snapshot after the value changes and "
                       "intersect/diff. Mind endianness and game encodings "
                       "(BCD, etc.) - strong models should reason about them.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "snapshot": {"type": "string"},
                "value": {"type": "integer", "description": "value to find"},
                "size": {"type": "integer", "description": "bytes: 1,2,3,4,8",
                         "default": 1},
                "endian": {"type": "string", "enum": ["little", "big"],
                           "default": "little"},
                "signed": {"type": "boolean", "default": False},
            },
            "required": ["snapshot", "value"],
        },
    },
    {
        "name": "retroarch_read_memory",
        "description": "Read live emulated memory and return a hex dump. "
                       "Use around a discovered address to reverse-engineer "
                       "structures (stat blocks, inventories): dump ~256 "
                       "bytes and reason about the layout.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "address": {"type": ["string", "integer"],
                            "description": "hex string or int"},
                "length": {"type": "integer", "default": 256},
            },
            "required": ["address"],
        },
    },
    {
        "name": "retroarch_write_memory",
        "description": "Write bytes into emulated memory. Safety: take a "
                       "save state first (retroarch_save_state) so a bad "
                       "write can be rolled back with retroarch_load_state. "
                       "Verify afterwards with retroarch_screenshot.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "address": {"type": ["string", "integer"]},
                "data": {"description": "hex string '3f 00' or byte array",
                         "type": ["string", "array"]},
            },
            "required": ["address", "data"],
        },
    },
    {
        "name": "retroarch_screenshot",
        "description": "Capture the game screen and return the PNG file path. "
                       "Open the image to visually verify that a memory write "
                       "had the intended effect (or to read on-screen values "
                       "the user asks about).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "directory": {"type": "string",
                              "description": "override screenshot dir"},
            },
        },
    },
    {
        "name": "retroarch_save_state",
        "description": "Save a save state. Always do this before the first "
                       "memory write of a session - it is the undo button.",
        "inputSchema": {
            "type": "object",
            "properties": {"slot": {"type": "integer"}},
        },
    },
    {
        "name": "retroarch_load_state",
        "description": "Load a save state to roll back after a bad write or "
                       "a crash-looking glitch.",
        "inputSchema": {
            "type": "object",
            "properties": {"slot": {"type": "integer"}},
        },
    },
    {
        "name": "retroarch_pause",
        "description": "Pause or resume the game. Pause before reading live "
                       "memory so values don't change mid-read.",
        "inputSchema": {
            "type": "object",
            "properties": {"paused": {"type": "boolean",
                           "description": "omit to toggle"}},
        },
    },
    {
        "name": "retroarch_frame_advance",
        "description": "Advance N frames while paused (frame-stepping). "
                       "Useful for observing a value change frame by frame.",
        "inputSchema": {
            "type": "object",
            "properties": {"frames": {"type": "integer", "default": 1}},
        },
    },
    {
        "name": "retroarch_snapshot_info",
        "description": "List cached snapshots (name, size, when taken, game).",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "retroarch_scan_start",
        "description": "Start a Cheat Engine-style scan session: snapshots RAM "
                       "and keeps every offset as a candidate (or seeds "
                       "candidates with an exact-value first scan when 'value' "
                       "is given). Then narrow down with retroarch_scan_filter. "
                       "Use this instead of search+diff when you don't know the "
                       "exact value (health bars, hidden timers) - filter by "
                       "increased/decreased/changed across in-game events.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "size": {"type": "integer",
                         "description": "value size in bytes: 1,2,3,4,8 "
                                        "(default 1; use 2 for most 16-bit "
                                        "console values)"},
                "endian": {"type": "string", "enum": ["little", "big"],
                           "default": "little"},
                "signed": {"type": "boolean", "default": False},
                "value": {"type": "integer",
                          "description": "optional exact first-scan value"},
                "start": {"type": ["string", "integer"], "default": 0},
                "max_bytes": {"type": "integer"},
            },
        },
    },
    {
        "name": "retroarch_scan_filter",
        "description": "Narrow the active scan: re-reads RAM and keeps only "
                       "candidates matching the condition vs the previous "
                       "read. Typical flow: scan_start -> change nothing -> "
                       "filter 'unchanged' -> lose health in-game -> filter "
                       "'decreased' -> repeat until 1 candidate remains. "
                       "Returns candidate addresses with current values when "
                       "few remain.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "condition": {"type": "string",
                              "enum": ["equal", "not_equal", "changed",
                                       "unchanged", "increased", "decreased",
                                       "increased_by", "decreased_by",
                                       "between"]},
                "value": {"type": "integer",
                          "description": "for equal/not_equal/increased_by/"
                                         "decreased_by/between (lower bound)"},
                "value2": {"type": "integer",
                           "description": "upper bound for 'between'"},
            },
            "required": ["condition"],
        },
    },
    {
        "name": "retroarch_scan_status",
        "description": "Show the active scan session: candidate count and "
                       "live values at the remaining candidate addresses.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "retroarch_cheat_add",
        "description": "Save a named cheat to the CURRENT game's persistent "
                       "profile (keyed by rom crc32, stored in cheats.json). "
                       "mode 'set' writes the value once on apply; mode "
                       "'freeze' rewrites it continuously (god mode). Same "
                       "id/name overwrites the existing entry. Found addresses "
                       "should always end up here so the next session (or the "
                       "auto-apply watcher) can restore them.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {"type": "string",
                         "description": "human name, e.g. 'Infinite health'"},
                "id": {"type": "string",
                       "description": "stable id (default: slugified name)"},
                "address": {"type": ["string", "integer"]},
                "value": {"type": "integer"},
                "size": {"type": "integer", "default": 1,
                         "description": "bytes: 1,2,3,4,8"},
                "endian": {"type": "string", "enum": ["little", "big"],
                           "default": "little"},
                "mode": {"type": "string", "enum": ["set", "freeze"],
                         "default": "set"},
                "interval": {"type": "number", "default": 0.1,
                             "description": "freeze rewrite interval (sec)"},
                "enabled": {"type": "boolean", "default": True},
                "notes": {"type": "string"},
                "apply_now": {"type": "boolean", "default": False,
                              "description": "apply immediately after saving"},
            },
            "required": ["name", "address", "value"],
        },
    },
    {
        "name": "retroarch_cheat_list",
        "description": "List saved cheats. Default: current game's profile "
                       "plus running freezers; game='all' dumps every saved "
                       "game profile and settings.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "game": {"type": "string", "enum": ["current", "all"],
                         "default": "current"},
            },
        },
    },
    {
        "name": "retroarch_cheat_remove",
        "description": "Remove a cheat from the current game's profile by id "
                       "(or name). Also stops its freezer if running.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "id": {"type": "string"},
                "name": {"type": "string"},
            },
        },
    },
    {
        "name": "retroarch_cheat_apply",
        "description": "Apply the current game's saved profile: 'set' cheats "
                       "are written once, 'freeze' cheats get a background "
                       "freezer. Pass id/name to apply just one. This is the "
                       "'load settings for this game' action.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "id": {"type": "string"},
                "name": {"type": "string"},
            },
        },
    },
    {
        "name": "retroarch_freeze",
        "description": "Ad-hoc value freezer (not saved). action 'start' "
                       "rewrites a value every interval; 'stop' (tag or "
                       "'all'); 'list'. Prefer retroarch_cheat_add with "
                       "mode='freeze' for anything worth keeping - freezers "
                       "die with this MCP server (the pi session).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {"type": "string",
                           "enum": ["start", "stop", "list"],
                           "default": "list"},
                "tag": {"type": "string"},
                "address": {"type": ["string", "integer"]},
                "value": {"type": "integer"},
                "size": {"type": "integer"},
                "endian": {"type": "string", "enum": ["little", "big"],
                           "default": "little"},
                "interval": {"type": "number", "default": 0.1},
            },
            "required": ["action"],
        },
    },
    {
        "name": "retroarch_auto_apply",
        "description": "Toggle the auto-apply watcher (persisted). When on, "
                       "the server polls GET_STATUS and applies the game's "
                       "saved enabled cheats as soon as it starts, stopping "
                       "the previous game's freezers. Call without arguments "
                       "for status.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "enabled": {"type": "boolean"},
            },
        },
    },
    {
        "name": "retroarch_game_note",
        "description": "Read or append per-game knowledge notes stored in "
                       "known_addresses.md (matched by rom crc32). Use "
                       "action='show' to read every discovered address, "
                       "core quirk and walkthrough hint for the running "
                       "game; use action='add' with 'note' to record a new "
                       "finding (dated bullet, section auto-created). "
                       "retroarch_get_status already includes these notes "
                       "as 'known_notes' - add to them whenever you "
                       "confirm something new so the next session starts "
                       "with full knowledge.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {"type": "string",
                           "enum": ["show", "add"],
                           "default": "show"},
                "note": {"type": "string",
                         "description": "note text for action='add'"},
            },
        },
    },
    {
        "name": "retroarch_command",
        "description": "Send a raw RetroArch network command (e.g. RESET, "
                       "FAST_FORWARD, MENU_TOGGLE, CHEAT_TOGGLE). Prefer the "
                       "dedicated tools when one exists.",
        "inputSchema": {
            "type": "object",
            "properties": {"command": {"type": "string"}},
            "required": ["command"],
        },
    },
]

TOOL_FUNCS = {
    "retroarch_get_status": tool_get_status,
    "retroarch_snapshot_ram": tool_snapshot_ram,
    "retroarch_diff_snapshots": tool_diff_snapshots,
    "retroarch_search_memory": tool_search_memory,
    "retroarch_read_memory": tool_read_memory,
    "retroarch_write_memory": tool_write_memory,
    "retroarch_screenshot": tool_screenshot,
    "retroarch_save_state": tool_save_state,
    "retroarch_load_state": tool_load_state,
    "retroarch_pause": tool_pause,
    "retroarch_frame_advance": tool_frame_advance,
    "retroarch_snapshot_info": tool_snapshot_info,
    "retroarch_scan_start": tool_scan_start,
    "retroarch_scan_filter": tool_scan_filter,
    "retroarch_scan_status": tool_scan_status,
    "retroarch_cheat_add": tool_cheat_add,
    "retroarch_cheat_list": tool_cheat_list,
    "retroarch_cheat_remove": tool_cheat_remove,
    "retroarch_cheat_apply": tool_cheat_apply,
    "retroarch_freeze": tool_freeze,
    "retroarch_auto_apply": tool_auto_apply,
    "retroarch_game_note": tool_game_note,
    "retroarch_command": tool_command,
}

# ---------------------------------------------------------------------------
# MCP stdio JSON-RPC loop
# ---------------------------------------------------------------------------

def rpc_result(req_id, result):
    return {"jsonrpc": "2.0", "id": req_id, "result": result}


def rpc_error(req_id, code, message):
    return {"jsonrpc": "2.0", "id": req_id,
            "error": {"code": code, "message": message}}


def handle_request(msg):
    method = msg.get("method", "")
    req_id = msg.get("id")
    is_notification = "id" not in msg

    if method == "initialize":
        params = msg.get("params") or {}
        client_ver = params.get("protocolVersion", SUPPORTED_PROTOCOL_VERSIONS[0])
        version = client_ver if client_ver in SUPPORTED_PROTOCOL_VERSIONS \
            else SUPPORTED_PROTOCOL_VERSIONS[-1]
        return rpc_result(req_id, {
            "protocolVersion": version,
            "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
            "instructions": (
                "You are a game-trainer copilot driving RetroArch. Workflow: "
                "1) retroarch_get_status to learn the game; it also lists "
                "cheats already saved for it ('saved_cheats') and all notes "
                "previously discovered for it ('known_notes': memory "
                "addresses, core quirks, hints) - offer "
                "retroarch_cheat_apply and build on the notes instead of "
                "re-discovering. "
                "2) To find a NEW value: retroarch_scan_start (use the "
                "visible value and size 2 for 16-bit consoles when known), "
                "then ask the user to change it in-game and narrow with "
                "retroarch_scan_filter (equal/increased/decreased/changed/ "
                "unchanged/between) until one candidate remains. Classic "
                "snapshot+diff also works for one-shot changes. "
                "3) retroarch_save_state before the first write. "
                "4) retroarch_write_memory, then retroarch_screenshot to "
                "verify. 5) retroarch_load_state to undo. "
                "6) ALWAYS save confirmed addresses with retroarch_cheat_add "
                "(mode 'freeze' for god-mode style) so they persist per game "
                "in cheats.json and can be re-applied next session or by the "
                "auto-apply watcher. Record reasoning, unconfirmed candidates "
                "and walkthrough hints with retroarch_game_note action='add' "
                "so they show up in get_status next time. "
                "Reason about endianness, BCD and "
                "multi-byte values when searching."),
        })

    if method == "ping":
        return rpc_result(req_id, {})

    if method == "tools/list":
        return rpc_result(req_id, {"tools": TOOLS})

    if method == "tools/call":
        params = msg.get("params") or {}
        name = params.get("name", "")
        args = params.get("arguments") or {}
        func = TOOL_FUNCS.get(name)
        if not func:
            return rpc_error(req_id, -32602, f"unknown tool: {name}")
        try:
            result = func(args)
            text = json.dumps(result, ensure_ascii=False, indent=2)
            is_error = isinstance(result, dict) and "error" in result
            return rpc_result(req_id, {
                "content": [{"type": "text", "text": text}],
                "isError": is_error,
            })
        except Exception as e:  # never let a tool crash the server
            log(f"tool {name} raised: {e!r}")
            return rpc_result(req_id, {
                "content": [{"type": "text",
                             "text": json.dumps({"error": f"{type(e).__name__}: {e}"})}],
                "isError": True,
            })

    if is_notification:
        return None  # e.g. notifications/initialized, cancelled, etc.

    return rpc_error(req_id, -32601, f"method not found: {method}")


def main():
    log(f"starting; RetroArch UDP target {RA_HOST}:{RA_PORT}, "
        f"cheats db {CHEATS_PATH}")
    auto_apply_start_if_enabled()
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            log(f"bad json: {line[:200]!r}")
            continue
        response = handle_request(msg)
        if response is not None:
            sys.stdout.write(json.dumps(response) + "\n")
            sys.stdout.flush()


if __name__ == "__main__":
    main()
