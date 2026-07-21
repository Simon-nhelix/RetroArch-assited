# Known Memory Addresses (per game)

트레이너 실험으로 발굴한 주소 모음. GET_STATUS의 crc32로 게임 식별.

> **v0.2부터는 `cheats.json`이 기계 판독용 정본** — `retroarch_cheat_add`로 저장하고
> `retroarch_cheat_apply`/`retroarch_auto_apply`로 복원된다. 이 파일은 사람이 읽는
> 발굴 메모(추론 과정, 미확정 주소) 용도로 유지.

## Splatterhouse 2 (USA) — Mega Drive / Genesis
- crc32: `2d1766e9`
- core: genesis_plus_gx (메모리 디스크립터 미지원 → SYSTEM_RAM 폴백으로 접근)

| 주소 (WRAM offset) | 용도 | 비고 |
|---|---|---|
| `0x00f6` | 철력 (하트 개수, word LE) | 쓰기로 화면 즉시 반영 확인. 무적 = 주기적으로 4 쓰기 |
| `0x00e2` | 라이프/컨티뉴 관련 | 2→1 변화 확인. 정확한 의미 추가 실험 필요 |
| `0x00e4` | 화면 표시 숫자와 일치한 적 있음 (04) | 미확정 |

### 발굴 과정 메모
- 라이프 검색(byte==2, 102개) → 사망 후 2→1 diff (3개: 0xe2, 0x3591, 0xe1fa)
- 0xe2에 5 쓰기 → 화면 4 표시 (큞램프?) — 단 라이프인지 컨티뉴인지 애매
- 철렫: 4하트→2하트 diff에서 0xf6이 유일한 4→2 → 쓰기 테스트로 확정
- 0xf6은 게임이 값을 덮어쓰지 않음 (진짜 변수). 덮어쓰는 주소는 임시 카운터
