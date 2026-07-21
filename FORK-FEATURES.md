# Fork-specific features

This fork adds features on top of upstream RetroArch. Everything listed here
is **not** in upstream — read this file to know what you have.

## 1. AI translation (native, no external tools)

Branch: `codex/native-ai-translation`

In-game text translation using on-device OCR (Apple Vision on macOS) and any
OpenAI-compatible chat endpoint (local or remote LLM).

- Commits: `5f3ff33d5b` (routing), `aeea40f552` (Apple OCR workflow),
  `d47573969f` (model picker), `a6e9737dfc` (Windows desktop support)
- Configure under Settings > AI Service.

## 2. Trainer-friendly network commands (memory access for any core)

`READ_CORE_MEMORY` / `WRITE_CORE_MEMORY` (UDP port 55355,
`network_cmd_enable = true`) originally only worked for cores that expose a
memory map (`RETRO_ENVIRONMENT_GET_MEMORY_MAP`). This fork falls back to plain
`RETRO_MEMORY_SYSTEM_RAM` for cores that don't (e.g. **Genesis Plus GX**), so
memory read/write works on virtually every core. (Commit `3cc94e36cc`)

Enable: Settings > Network > Network Commands = ON (visible with
Settings > User Interface > Show Advanced Settings = ON), then restart.

## 3. Trainer MCP server — AI-agent game trainer (`tools/trainer-mcp/`)

`tools/trainer-mcp/retroarch_mcp.py` is a dependency-free Python MCP server
that bridges the network command interface to AI agents (pi, Claude Code).
Talk to your agent to find values, patch memory, and keep per-game cheats:

- **Cheat Engine-style filtered scan**: `retroarch_scan_start` /
  `retroarch_scan_filter` — narrow candidates by
  `equal / changed / unchanged / increased / decreased / increased_by /
  decreased_by / between`, even when the exact value is invisible
  (health bars etc.)
- **Per-game cheat profiles**: `retroarch_cheat_add / list / apply / remove` —
  saved in `cheats.json` keyed by ROM crc32; re-apply a game's whole setup
  with one call. `mode: "freeze"` keeps values pinned (god mode).
- **Built-in freezer + auto-apply watcher**: `retroarch_freeze`,
  `retroarch_auto_apply` (auto-applies a game's saved cheats when it starts).
- Classic tools: RAM snapshot/diff/search, read/write, save states, pause,
  frame advance, screenshots for visual verification.

Quick start:

1. Enable Network Commands (see above).
2. Register the server with your agent, e.g. for pi
   (`~/.pi/agent/settings.json`):

   ```json
   "retroarch-trainer": {
     "command": "python3",
     "args": ["<this fork>/tools/trainer-mcp/retroarch_mcp.py"],
     "enabled": true
   }
   ```

3. Ask the agent e.g. "make me invincible in this game" — it will scan,
   verify on-screen via screenshot, and save the cheat for next time.

See `tools/trainer-mcp/README.md` for the full tool list and workflow.
Run `python3 tools/trainer-mcp/test_fake_retroarch.py` to verify the server
without launching RetroArch.

## Build notes (macOS)

- Requires the Metal toolchain (`xcodebuild -downloadComponent MetalToolchain`).
  Building without Metal silently falls back to OpenGL and the screen
  flickers constantly.
- Non-Metal builds link QuartzCore explicitly (commit `a37b8f021b`, which
  also fixes the kiosk-mode lockout item visibility).
