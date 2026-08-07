# RetroArch Trainer MCP

RetroArch를 "트레이너 장난감"으로 바꾸는 MCP 서버.
RetroArch의 UDP 네트워크 커맨드 인터페이스(포트 55355)를 MCP 도구로 노출해서,
pi / Claude Code 같은 외부 AI 에이전트가 대화만으로 게임 메모리를 읽고 고칠 수 있게 한다.

```
유저: "골드가 10개 모자라"
AI:   get_status → 게임 파악 → (아는 게임이면 주소 바로 앎)
      또는 snapshot → 유저가 게임에서 값 변경 → snapshot → diff → 주소 특정
      → save_state → write_memory → screenshot으로 확인
```

## RetroArch 쪽 설정 (1회)

1. `Settings > Network > Network Commands` = **ON** (retroarch.cfg: `network_cmd_enable = "true"`)
2. 포트 기본 55355 (`network_cmd_port`)
3. 스크린샷 확인 기능을 쓰려면 `Settings > Directory > Screenshots` 지정 권장
   - 자동 탐지: `RETROARCH_SCREENSHOT_DIR` env → retroarch.cfg 파싱 → 흔한 경로 순

## pi 등록

`~/.pi/agent/settings.json`의 `mcpServers`에 등록 (이 디렉토리 기준):

```json
"retroarch-trainer": {
  "command": "/opt/homebrew/bin/python3",
  "args": ["<이 포크>/tools/trainer-mcp/retroarch_mcp.py"],
  "enabled": true
}
```

pi 재시작 후 `retroarch_*` 도구 사용 가능.

## 도구 목록

### 값 찾기 (Cheat Engine식 필터 스캔 — v0.2)
| 도구 | 역할 |
|---|---|
| `retroarch_scan_start` | 스캔 세션 시작. `value` 주면 정확값 1차 스캔, 없으면 전체가 후보. 16비트 콘솔 값은 `size: 2` 권장 |
| `retroarch_scan_filter` | 후보 좁히기: `equal / not_equal / changed / unchanged / increased / decreased / increased_by / decreased_by / between`. **정확한 값을 몰라도**(철력 게이지 등) "줄었다/늘었다"로 추적 가능 |
| `retroarch_scan_status` | 남은 후보 수 + 후보 주소의 현재 값 확인 |
| `retroarch_snapshot_ram` / `retroarch_diff_snapshots` / `retroarch_search_memory` | 기존 스냅샷+diff 방식 (한 번에 변하는 값엔 여전히 유효) |

### 게임별 치트 저장/불러오기 (v0.2)
| 도구 | 역할 |
|---|---|
| `retroarch_cheat_add` | 발굴한 주소를 이름 붙여 **현재 게임 프로파일에 영구 저장** (`cheats.json`, crc32 키). `mode: "set"`(1회 쓰기) 또는 `"freeze"`(주기적 재쓰기=무적류) |
| `retroarch_cheat_list` | 현재 게임의 저장 치트 + 실행 중 프리저 (`game: "all"`이면 전체) |
| `retroarch_cheat_apply` | **이 게임의 저장 치트 적용** — "설정 불러오기"에 해당. id 지정하면 하나만 |
| `retroarch_cheat_remove` | 삭제 (프리저도 같이 정지) |
| `retroarch_freeze` | 저장 안 하는 즉석 프리저 start/stop/list |
| `retroarch_auto_apply` | **자동 적용 watcher** 켜기/끄기(영구 저장). 켜두면 게임 시작을 감지해서 저장된 치트를 자동 적용하고 이전 게임 프리저는 해제 |

### 게임별 지식 노트 (v0.3)
| 도구 | 역할 |
|---|---|
| `retroarch_game_note` | 게임별 지식 노트(`known_addresses.md`, crc32 매칭) 읽기(`show`)/추가(`add`). 발굴 추론, 미확정 후보, 코어별 주의사항, 공략 힌트를 날짜 붙여 축적 |

`retroarch_get_status`는 현재 게임의 `saved_cheats`(주소/값/모드 포함)와
`known_notes`(해당 게임 섹션 전체)를 응답에 자동으로 포함한다 — 게임을 켜면
AI가 재발굴 없이 이전 지식을 바로 이어받는다.

### 기본 메모리/제어
| 도구 | 역할 |
|---|---|
| `retroarch_get_status` | 실행 중 게임 (시스템, ROM명, crc32) + **저장된 치트와 지식 노트도 같이 표시** |
| `retroarch_read_memory` | 라이브 메모리 헥스 덤프 (구조 분석용) |
| `retroarch_write_memory` | 메모리 쓰기 (쓰기 전 save_state 권장) |
| `retroarch_screenshot` | 스크린샷 찍고 PNG 경로 반환 (AI가 눈으로 확인) |
| `retroarch_save_state` / `retroarch_load_state` | 실험 전 백업 / 롤백 |
| `retroarch_pause` / `retroarch_frame_advance` | 일시정지 / 프레임 스텝 |
| `retroarch_snapshot_info` | 캐시된 스냅샷 목록 |
| `retroarch_command` | 원시 명령 (RESET, FAST_FORWARD 등) |

## 게임별 치트 DB (`cheats.json`)

`cheats.json`은 ROM의 **crc32**를 키로 게임을 식별한다 (crc32가 없으면 ROM 파일명).
발굴한 주소는 `retroarch_cheat_add`로 저장하면 다음 세션에서 `retroarch_cheat_apply`
한 방으로 복원되고, `retroarch_auto_apply`를 켜두면 게임 실행 감지 시 자동 적용된다.

```json
"2d1766e9": {   // Splatterhouse 2 (USA)
  "cheats": [
    {"id": "god-mode", "address": "0xf6", "value": 4,
     "mode": "freeze", "interval": 0.1, "enabled": true}
  ]
}
```

`RETROARCH_TRAINER_CHEATS` 환경변수로 DB 경로 변경 가능.
기존 `freezer.py`(외부 프로세스 프리저)는 계속 쓸 수 있지만, MCP 내장 프리저가
상태 조회/일괄 해제/프로파일 연동이 되므로 신규 작업은 내장 쪽 권장.

## 새 값 찾기 워크플로우 예시 (철력 게이지, 정확한 수치 모를 때)

1. `retroarch_scan_start` (size 2, 값 없이) → 후보 = RAM 전체
2. 아무것도 안 하고 `retroarch_scan_filter` `unchanged` → 움직이는 값 제거
3. 게임에서 피격 → `decreased` → 후보 급감
4. 회복 아이템 → `increased` … 1개 남을 때까지 반복
5. `retroarch_save_state` → `retroarch_write_memory`로 확인 → 스크린샷 검증
6. 확정되면 `retroarch_cheat_add` (mode `"freeze"`)로 저장

## 환경변수

- `RETROARCH_CMD_HOST` / `RETROARCH_CMD_PORT` (기본 127.0.0.1:55355)
- `RETROARCH_TRAINER_CHEATS` (치트 DB 경로, 기본 `cheats.json`)
- `RETROARCH_TRAINER_NOTES` (지식 노트 경로, 기본 `known_addresses.md`)
- `RETROARCH_SCREENSHOT_DIR`
- `RETROARCH_CMD_TIMEOUT` (UDP 응답 대기 초, 기본 0.8)

## 테스트

RetroArch 없이도 가짜 UDP 응답기로 전체 플로우 검증 가능:

```sh
python3 test_fake_retroarch.py
```

## 알려진 한계

- **버튼 입력 주입 없음**: 네트워크 명령에 joypad 입력이 없어서 "A 눌러줘"는 사람이 함
- 스냅샷은 메모리 디스크립터 공간 0x0부터 연속 읽기. VRAM/SRAM 등 별도 영역은
  `retroarch_read_memory`로 개별 접근
- UDP라 큰 응답은 512바이트 청크로 나눠 읽음 (128KB ≈ 256회 왕복, 로컬이라 1초 내외)
- Android에서는 Termux 또는 같은 LAN의 PC에서 이 서버를 실행
