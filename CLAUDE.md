# Mir — 패킷 제너레이터

L2~L7 헤더를 자유롭게 조립한 패킷을 **PF(NIC 포트) 라인레이트로** 전송하고, 업로드한
PCAP을 그대로 재현해 비정상 handshake를 포함한 프로토콜 동작을 **테스트·검증**하는
리눅스 기반 도구.

**성능 보장 단위는 PF 하나다.** 지원 속도 **1G / 10G / 100G**, PF 개수 **N 은 장비가
허용하는 만큼 확장**되며 총 대역은 `PF 라인레이트 × N`. PF 하나당 파드 하나라
**NIC을 늘리는 데 코드 변경이 없다** — 코어·hugepage·PCIe 슬롯만 N배로 커진다.

| PF 속도 | 64B 라인레이트 | PMD | 위치 |
|---|---|---|---|
| 1G | 1.49 Mpps | `igb` | I210 (현재 관리용) |
| **10G** | **14.88 Mpps** | `i40e` | X710 — 검증기 (10G × 4 = 40G) |
| **100G** | **148.81 Mpps** | `ice` | E810 — 목표기 (100G × 4 = **400G**, 595.24 Mpps) |

`400G`는 목표기의 **기준 구성**이지 아키텍처 상한이 아니다.

**기준 프레임은 `ETH + IPv4 + TCP` 최소 헤더 구성 = 64B**(확정). 헤더 합은 54B 지만
이더넷 최소 프레임(64B) 규정 때문에 **패딩 6B 가 강제**되어 64B 가 된다 — 이보다 작은
프레임은 존재할 수 없다. IPv6 도 지원 범위이며 최소 78B 라 pps 가 약 14% 낮으므로,
IPv4 기준을 만족하면 자동으로 충족된다.

검증기는 목표기의 1/10 스케일이지만 **PF 4개 = 파드 4개** 구조가 동일하다
→ [장비 2단 구성](#장비--검증기와-목표기-2단-구성).

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
| 데이터플레인 | **C + DPDK** | PF당 line rate 커널 우회 송수신. 순수 C (C++ 의존성 없음) |
| 사이드카 | **Go** | gRPC 종단. C 를 순수 C 로 유지하고 스레드 격리를 구조적으로 보장 |
| 제어부 | **Go** | 시나리오·집계·판정, 단일 바이너리(`go:embed`로 UI 내장) |
| GUI | **React + TS** | React Flow 시나리오 빌더, uPlot/WebGL 실시간 차트 |

**판정 위치**: µs 단위 실시간 반응(ACK 생략·SYN-ACK 즉시 판정) → C / 세션 단위 룰·오케스트레이션 → Go.

**개별 패킷은 프로세스 경계를 넘지 않는다.** 파드당 148 Mpps(전체 595 Mpps)를
밖으로 내보내는 건 성립하지 않으므로 C 가 판정 결과와 요약만 올린다. 그래서 제어
채널이 지연되거나 끊겨도 판정 정확도에 영향이 없다.

## 동작 모드

| 모드 | 목적 | 성능 | 레이어 |
|---|---|---|---|
| **A. Stateless 고속** | 대량 트래픽, L2~L4 이상동작 | PF당 line rate (× N) | L2~L4 |
| **B. Stateful 세션** | 실제 TLS+HTTP, handshake 제어 | 세션 수 제한 | L2~L7 |
| **C. Replay 재현** | 업로드 PCAP 그대로 재생/구간 재전송 | 재현 정확도 기준 | L2~L7 |

## 장비 — 검증기와 목표기 2단 구성

같은 코드가 두 장비에서 돈다. **PF 4개 = 파드 4개** 배치가 양쪽 동일하므로
이식 시 바뀌는 것은 PMD 하나뿐이다.

| | ② 검증기 (현재) | ③ 목표기 (**미도입** — 향후 세팅) |
|---|---|---|
| 주소/모델 | `10.10.40.121` · Intel CAR5070 | 미정 |
| NIC | X710 **4×10G** `0000:43:00.0~3` → `i40e` | E810-CQDA1 **100G × 4** → `ice` |
| 총 대역 | **40G** (64B 59.5 Mpps 이론) | **400G** (64B **595.2 Mpps**) |
| CPU | Xeon Gold 5418N **24C/1소켓** (CPU1 빈 소켓) | **2소켓 × 32코어 이상, HT off** |
| 메모리 | 128GB DDR5-4800 8채널 | 256GB, 소켓당 8채널 전부 장착 |
| PCIe | Gen3 x8 (X710 실측) | **x16 슬롯 4개**, 소켓당 2장 균등 배치 |
| OS | Ubuntu 20.04 (EOL) · gcc 9.4 · Python 3.8 | **장비 세팅 시 확정.** 하한 = gcc 11+ / Python 3.10+ |
| 역할 | Phase 0~6 개발·기능 검증 | Phase 7 성능(400G) 검증 |

**목표기의 OS 버전은 지금 확정하지 않는다.** 장비가 들어온 뒤 정하되, 위 하한선은
검증기에서 실제로 막힌 지점에서 나온 것이라 타협 대상이 아니다 — gcc 9.4 는
`-march=x86-64-v3` 를 몰라 호스트 빌드가 컨테이너와 다른 ISA(`haswell`)로
내려앉았다. 근거와 도입 후 체크리스트는
[REQUIREMENTS.md](docs/REQUIREMENTS.md) 7절 Open Issue 2 ③.

- 검증기는 **증설로 400G가 되지 않는다** — CPU1 미장착 + x16 슬롯 3개가 라이저
  미장착으로 PCI 열거조차 안 되고, 그중 `Riser1T` 는 CPU1 측이다.
- ⚠️ **X710 계열은 소형 프레임에서 라인레이트 미달**(Intel 인정 HW 제약). 40G
  이론치를 그대로 믿지 말고 Phase 2 에서 실측치를 먼저 확보한다.
- 검증기 현재 상태: **VT-d 비활성 · hugepage 0 · DPDK 미설치 · 코어 격리 설정 무효**
  (`isolcpus=0-23` 이 HT 형제 관계상 격리가 안 된다). 조치 절차는
  [deploy/host/10-kernel-cmdline.md](deploy/host/10-kernel-cmdline.md).

상세 실측값·목표기 요구사양은 [REQUIREMENTS.md](docs/REQUIREMENTS.md) 7절 Open Issue 2.

## 실행 환경 / 백엔드

코드 한 벌로 베어메탈 + 3대 클라우드에서 PF 라인레이트. DPDK PMD를 갈아끼우는
**백엔드 추상화 계층**으로 구현, 실행 시 환경 감지.

- 베어메탈: Intel PMD (100G E810=ice, 10G X710=i40e, 1G I210=igb)
- **데이터 경로 3-tier**: `vfio`(정본, 100G급) / `af_xdp`(10G급) / `af_packet`(1G급).
  PMD 만 바뀌므로 **코드는 그대로**고 EAL 인자만 달라진다. Tier 2·3 은 hugepage·IOMMU
  없이 뜨지만 **NIC 을 커널이 계속 소유해 handshake 제어 시 커널이 RST 를 쏜다** —
  기능 개발용이고 성능·정확도 검증은 Tier 1 에서. → [REQUIREMENTS 4-1-1](docs/REQUIREMENTS.md)
- AWS(ENA) → Azure(MANA, 200G, 커널 6.14+) → GCP(GVE) 순서 지원 · 폴백 **AF_XDP**
- **클라우드 제약**: L2 임의조작·src IP 스푸핑은 격리 정책상 불가(성능 아닌 설계 이유). L4 handshake 제어·커스텀 L7은 가능.

## 개발 로드맵

`Phase 0` 환경/HW → `1` DPDK hello packet → `2` L2~L4 고속 송신 → `3` 수신 캡처+판정 →
`4` handshake 제어 → **`4-1` PCAP 리플레이 엔진** → `5` TLS+HTTP stateful →
`6` GUI → `7` 400G 튜닝 → `8` 클라우드 백엔드 이식.

`0`~`6` 은 검증기(40G)에서, `7` 은 목표기(400G)에서 수행한다.

---

## 실행 형태 — 두 가지를 모두 지원

| 형태 | 대상 | 오케스트레이션 | 절차 |
|---|---|---|---|
| **K — 베어메탈 단일 노드 k8s(k3s)** | 운영 배포, 다중 PF | k8s Deployment. 제어부가 k8s API 로 개수·리소스 조정 | [DEPLOYMENT.md](docs/DEPLOYMENT.md) |
| **S — 단독 실행 (k8s 없음)** | 개발·검증기, k8s 반입 불가 장비 | systemd 템플릿 유닛 + `taskset` | [INSTALL-STANDALONE.md](docs/INSTALL-STANDALONE.md) |

호스트 준비(VT-d·hugepage·vfio 바인딩·코어 격리)는 두 형태가 동일하고, k3s 설치
지점부터 갈린다. S 는 K 에서 **k8s 계층만 걷어낸 것**이다. *"monolithic" 은 단일
프로세스가 아니다* — Go 와 C 를 한 프로세스로 합치면 cgo 로 DPDK 를 부르게 되어
아키텍처 전제가 무너진다. 합쳐지는 것은 배포 단위다.
→ [docs/REQUIREMENTS.md](docs/REQUIREMENTS.md) 4-3-5절.

**S 형태의 현재 상태**: 데이터플레인·사이드카는 코드 변경 없이 뜬다(lcore ←
`sched_getaffinity`, 장치 ← `MIR_DEVICE_SPEC`, prefix ← `HOSTNAME`).
**제어부는 아직 못 뜬다** — k8s 설정이 없으면 종료하고 registry 가 EndpointSlice
전용이다. 최종 목표는 사이드카 없이 제어부가 ③ 에 직결하는 것이지만, 당분간은
사이드카를 로컬 gRPC 로 유지한다.

### K 형태 — 계층 경계

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
│  ├─ ARCHITECTURE.md   ← 구현 관점: 채널·콜플로우·스레드 모델·로드맵 매핑
│  └─ DEPLOYMENT.md     ← 호스트 준비 ~ 배포 ~ 검증 ~ 트러블슈팅
├─ dataplane/           ← C/DPDK. 순수 C, meson 빌드
│  └─ src/  eal_args · stats · ipc_server · port · tx_hello · pcap_reader · main
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
- **코드를 건드리기 전에** [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) 를 읽을 것.
  채널 ①~⑤ 의 콜플로우(시퀀스 다이어그램), 스레드·코어 격리가 깨지는 지점,
  그리고 **각 Phase 가 어느 파일을 건드리는지**가 정리돼 있다.

## 현재 상태

- **Phase 0 인프라 골격 완성** — 호스트 준비 스크립트, 3개 이미지, k8s 매니페스트,
  제어 채널(C ⇄ 사이드카 ⇄ 제어부), 개수·리소스 조정 API 까지 작성됨.
- **Phase 1 hello packet 작성됨** — 포트 구성·start·링크 확인(`port.c`)과
  EtherType 0x88B5 프레임 송신(`tx_hello.c`). `MIR_HELLO_TX_COUNT` 로만 켜지며
  기본값은 비활성이라 파드 기동만으로 선로에 프레임이 나가지 않는다.
  텔레메트리의 포트별 수치는 이제 NIC 하드웨어 카운터(`rte_eth_stats_get`)에서 온다.
- **실물 하드웨어에서 아직 미검증** — 로컬에 DPDK 툴체인이 없어 C 코드는
  컴파일 검증조차 되지 않았다. 확인된 것은 Go 빌드/vet 과 kustomize 렌더뿐이다.
  실물 검증 절차는 [docs/DEPLOYMENT.md](docs/DEPLOYMENT.md) 5절 3·3-1단계.
- **검증기 하드웨어 점검 완료(2026-08-01)** — `10.10.40.121` 은 DPDK 가용 장비로
  확인됐다(X710 4포트 i40e, 베어메탈, `CONFIG_VFIO_PCI=y`). 다만 **호스트 준비가
  전혀 안 된 상태**다: VT-d 비활성 · HugePages 0 · DPDK/meson/ninja/libnuma-dev
  미설치 · 코어 격리 설정 무효. 재부팅을 수반하는 선행 작업이 남아 있다.
- **시나리오 송신 엔진은 여전히 미구현** (Phase 2~4-1). hello 는 기동 시 1회
  송신하는 경로 증명일 뿐 속도 제어도 다중 lcore 도 없다. `StartScenario` 는
  성공을 가장하지 않고 "미구현" Ack 를 돌려준다.
- `pcap_reader.h` 는 경로 기반 API 라 공유 볼륨 설계와 이미 맞물려 있다 (Phase 4-1).

## 미확정 이슈 (요약)

~~서버 사양~~(확정 — 위 장비 표) · 대상 환경(직결/루프백/스위치) · 판정 규칙 상세 ·
TLS 세부(1.2/1.3, 클라이언트 인증서) · PCAP 재현 세부(포맷/타이밍/구간) ·
단독 실행 형태(S)의 범위 · 범위 밖 항목.
→ 전체 목록은 요구사항 정의서 7절 참고.
