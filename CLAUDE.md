# Mir — 패킷 제너레이터

L2~L7 헤더를 자유롭게 조립한 패킷을 **고속(NIC당 100G)** 으로 전송하고, 업로드한
PCAP을 그대로 재현해 비정상 handshake를 포함한 프로토콜 동작을 **테스트·검증**하는
리눅스 기반 도구.

> ⚠️ **전제**: 대상은 항상 본인 소유/테스트 승인된 장비. 제3자 IP 대상 트래픽 생성은 범위 밖.

---

## 아키텍처 (B안: C + DPDK 직접 개발)

3계층을 **별도 프로세스**로 분리한다 (Go↔DPDK cgo 오버헤드 회피).

```
[React + TypeScript GUI]
        │ WebSocket / REST
        ▼
[Go 제어부]  시나리오·규칙·상태머신·텔레메트리 집계·백엔드 선택
        │ unix socket / hugepage 공유메모리 ring
        ▼
[C 데이터플레인 / DPDK]  NIC bind·패킷 빌더·TX/RX 루프·실시간 판정·응답 캡처
```

| 계층 | 기술 | 역할 |
|---|---|---|
| 데이터플레인 | **C + DPDK** | 100G line rate 커널 우회 송수신 |
| 제어부 | **Go** | 시나리오·집계·판정, 단일 바이너리(`go:embed`로 UI 내장) |
| GUI | **React + TS** | React Flow 시나리오 빌더, uPlot/WebGL 실시간 차트 |

**판정 위치**: µs 단위 실시간 반응(ACK 생략·SYN-ACK 즉시 판정) → C / 세션 단위 룰·오케스트레이션 → Go.

## 동작 모드

| 모드 | 목적 | 성능 | 레이어 |
|---|---|---|---|
| **A. Stateless 고속** | 대량 트래픽, L2~L4 이상동작 | 100G line rate | L2~L4 |
| **B. Stateful 세션** | 실제 TLS+HTTP, handshake 제어 | 세션 수 제한 | L2~L7 |
| **C. Replay 재현** | 업로드 PCAP 그대로 재생/구간 재전송 | 재현 정확도 기준 | L2~L7 |

## 실행 환경 / 백엔드

코드 한 벌로 베어메탈 + 3대 클라우드에서 100G급. DPDK PMD를 갈아끼우는 **백엔드
추상화 계층**으로 구현, 실행 시 환경 감지.

- 베어메탈: Intel PMD (100G E810=ice, 40G XL710=i40e)
- AWS(ENA) → Azure(MANA, 200G, 커널 6.14+) → GCP(GVE) 순서 지원 · 폴백 **AF_XDP**
- **클라우드 제약**: L2 임의조작·src IP 스푸핑은 격리 정책상 불가(성능 아닌 설계 이유). L4 handshake 제어·커스텀 L7은 가능.

## 개발 로드맵

`Phase 0` 환경/HW → `1` DPDK hello packet → `2` L2~L4 고속 송신 → `3` 수신 캡처+판정 →
`4` handshake 제어 → **`4-1` PCAP 리플레이 엔진** → `5` TLS+HTTP stateful →
`6` GUI → `7` 100G 튜닝 → `8` 클라우드 백엔드 이식.

---

## 저장소 구조

```
Mir/
├─ CLAUDE.md            ← 이 파일 (프로젝트 개요 / 진입점)
├─ LICENSE
├─ docs/                ← 문서 모음
│  ├─ README.md
│  └─ REQUIREMENTS.md   ← 상세 요구사항·설계·Open Issues (원본 SoT)
└─ dataplane/           ← C 데이터플레인 (작업 중)
   └─ src/
```

- **상세 요구사항·설계·미확정 이슈**의 단일 출처(Source of Truth)는
  [docs/REQUIREMENTS.md](docs/REQUIREMENTS.md). 세부 결정은 그 문서를 갱신한다.

## 현재 상태

- 요구사항 정의서 초안 확정, 문서 구조 정리 완료.
- `dataplane/`는 PCAP 리플레이(Phase 4-1) 착수 준비 단계 — 아직 미완성.

## 미확정 이슈 (요약)

서버 사양 · 대상 환경(직결/루프백/스위치) · 판정 규칙 상세 · TLS 세부(1.2/1.3,
클라이언트 인증서) · PCAP 재현 세부(포맷/타이밍/구간) · 범위 밖 항목.
→ 전체 목록은 요구사항 정의서 7절 참고.
