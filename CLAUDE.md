# Mir — 패킷 제너레이터

L2~L7 헤더를 자유롭게 조립한 패킷을 **고속(NIC당 100G)** 으로 전송하고, 업로드한
PCAP을 그대로 재현해 비정상 handshake를 포함한 프로토콜 동작을 **테스트·검증**하는
리눅스 기반 도구.

> ⚠️ **전제**: 대상은 항상 본인 소유/테스트 승인된 장비. 제3자 IP 대상 트래픽 생성은 범위 밖.

---

## 아키텍처 (C + DPDK 개발)

3계층을 **별도 프로세스**로 분리한다 (Go↔DPDK cgo 오버헤드 회피).

```
[React + TypeScript GUI]
        │ WebSocket / REST
        ▼
[제어부 파드]  mir-control (Go) — 시나리오·규칙·집계 + k8s API로 개수·리소스 조정
        │ gRPC (스트리밍)
        ▼
[데이터플레인 파드 × N]  ← NIC PF 하나당 파드 하나
  ├─ agent      (Go)     사이드카. gRPC ⇄ unix socket 릴레이. 공유 풀 코어
  └─ dataplane  (C/DPDK) NIC bind·패킷 빌더·TX/RX·실시간 판정. 배타 코어
```

| 계층 | 기술 | 역할 |
|---|---|---|
| 데이터플레인 | **C + DPDK** | 100G line rate 커널 우회 송수신. 순수 C (C++ 의존성 없음) |
| 사이드카 | **Go** | gRPC 종단. C 를 순수 C 로 유지하고 스레드 격리를 구조적으로 보장 |
| 제어부 | **Go** | 시나리오·집계·판정, 단일 바이너리(`go:embed`로 UI 내장) |
| GUI | **React + TS** | React Flow 시나리오 빌더, uPlot/WebGL 실시간 차트 |

**판정 위치**: µs 단위 실시간 반응(ACK 생략·SYN-ACK 즉시 판정) → C / 세션 단위 룰·오케스트레이션 → Go.

**개별 패킷은 프로세스 경계를 넘지 않는다.** 148 Mpps 를 밖으로 내보내는 건
성립하지 않으므로 C 가 판정 결과와 요약만 올린다. 그래서 제어 채널이 지연되거나
끊겨도 판정 정확도에 영향이 없다.

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

## 실행 형태 — 베어메탈 단일 노드 k8s

컨테이너화의 목적은 성능이 아니라 **재현성**이다. hugepage 크기·IOMMU·vfio
바인딩·CPU 격리는 전부 호스트 커널에 묶여 있어 컨테이너가 숨겨주지 못하므로,
"호스트가 해야 하는 것"과 "k8s 가 해주는 것"의 경계를 문서로 못박아 둔다.

- **NIC 할당**: PF 전체를 vfio-pci 로 패스스루 (VF 는 spoof check 에 막혀
  L2 임의조작·src IP 스푸핑이 불가능하다)
- **코어 배치**: `dataplane` 컨테이너는 cpu 를 **정수**로 요청해 배타 코어를 받고,
  `agent` 는 **분수**(`500m`)로 요청해 공유 풀에 남는다. 이 한 줄이 사이드카의
  gRPC 스레드가 busy-poll 코어를 침범하지 못하게 만든다
- **개수·리소스 조정**: 제어부가 k8s API 로 수행. 무중단은 불가능하고
  (Pod 불변성 + static CPU manager 에서 in-place resize 불가 + DPDK lcore 고정),
  **재시작 범위를 데이터플레인으로만 한정**하는 것이 목표다

절차와 트러블슈팅은 [docs/DEPLOYMENT.md](docs/DEPLOYMENT.md).

---

## 저장소 구조

```
Mir/
├─ CLAUDE.md            ← 이 파일 (프로젝트 개요 / 진입점)
├─ README.md
├─ LICENSE
├─ buf.yaml / buf.gen.yaml   ← proto 코드 생성 (buf generate)
├─ proto/
│  └─ dataplane.proto   ← 두 hop 공용 스키마 + gRPC service
├─ docs/
│  ├─ REQUIREMENTS.md   ← 상세 요구사항·설계·Open Issues (원본 SoT)
│  └─ DEPLOYMENT.md     ← 호스트 준비 ~ 배포 ~ 검증 ~ 트러블슈팅
├─ dataplane/           ← C/DPDK. 순수 C, meson 빌드
│  └─ src/  eal_args · stats · ipc_server · pcap_reader · main
├─ control/             ← 단일 Go 모듈(module mir). 두 바이너리가 스키마 공유
│  ├─ cmd/mir-agent/    ← 사이드카 (릴레이)
│  ├─ cmd/mir-control/  ← 제어부 (조정 API + 사이드카 연결 관리)
│  └─ internal/  pb · ipc · agent · registry · k8s · api
└─ deploy/
   ├─ host/             ← 커널 파라미터 · vfio 바인딩 · k3s 설치 · 사전점검
   ├─ docker/           ← Dockerfile 3개 (빌드 컨텍스트는 저장소 루트)
   └─ k8s/  base + overlays/baremetal
```

- **상세 요구사항·설계·미확정 이슈**의 단일 출처(Source of Truth)는
  [docs/REQUIREMENTS.md](docs/REQUIREMENTS.md). 세부 결정은 그 문서를 갱신한다.

## 현재 상태

- **Phase 0 인프라 골격 완성** — 호스트 준비 스크립트, 3개 이미지, k8s 매니페스트,
  제어 채널(C ⇄ 사이드카 ⇄ 제어부), 개수·리소스 조정 API 까지 작성됨.
- 완료 기준은 *컨테이너 안에서 `rte_eal_init` 이 성공하고 NIC 포트가 인식되는 것*.
  **실물 하드웨어에서 아직 미검증** — 로컬에서는 Go 빌드/vet 과 kustomize 렌더만 확인.
- **TX/RX 송신 엔진은 미구현** (Phase 1 이후). `StartScenario` 는 성공을 가장하지 않고
  "미구현" Ack 를 돌려준다.
- `pcap_reader.h` 는 경로 기반 API 라 공유 볼륨 설계와 이미 맞물려 있다 (Phase 4-1).

## 미확정 이슈 (요약)

서버 사양 · 대상 환경(직결/루프백/스위치) · 판정 규칙 상세 · TLS 세부(1.2/1.3,
클라이언트 인증서) · PCAP 재현 세부(포맷/타이밍/구간) · 범위 밖 항목.
→ 전체 목록은 요구사항 정의서 7절 참고.
