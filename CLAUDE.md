# Mir — 패킷 제너레이터

L2~L7 헤더를 자유롭게 조립한 패킷을 **PF(NIC 포트) 라인레이트로** 전송하고, 업로드한
PCAP을 그대로 재현해 비정상 handshake를 포함한 프로토콜 동작을 **테스트·검증**하는
리눅스 기반 도구.

**성능 보장 단위는 PF 하나다.** 지원 속도 **1G / 10G / 100G**, PF 개수 **N 은 장비가
허용하는 만큼 확장**되며 총 대역은 `PF 라인레이트 × N`. PF 하나당 인스턴스 하나라
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

검증기는 목표기의 1/10 스케일이지만 **PF 4개 = 인스턴스 4개** 구조가 동일하다
→ [장비 2단 구성](#장비--검증기와-목표기-2단-구성).

> ⚠️ **전제**: 대상은 항상 본인 소유/테스트 승인된 장비. 제3자 IP 대상 트래픽 생성은 범위 밖.

---

## 아키텍처 (C + DPDK 개발)

3계층을 **별도 프로세스**로 분리한다 (Go↔DPDK cgo 오버헤드 회피).

```
[React + TypeScript GUI]
        │ SSE(텔레메트리 푸시) / REST(명령)
        ▼
[제어부]  mir-control (Go) — 시나리오·규칙·집계 + 함대 관측 (장비당 1개 아님, 전체 1개)
        │ gRPC (스트리밍, mTLS) — 장비 경계를 넘을 수 있다
        ▼
[데이터플레인 인스턴스 × N]  ← NIC PF 하나당 인스턴스 하나 (장비 여러 대에 걸쳐도 된다)
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

**개별 패킷은 프로세스 경계를 넘지 않는다.** 인스턴스당 148 Mpps(전체 595 Mpps)를
밖으로 내보내는 건 성립하지 않으므로 C 가 판정 결과와 요약만 올린다. 그래서 제어
채널이 지연되거나 끊겨도 판정 정확도에 영향이 없다.

이 성질이 **멀티 장비를 싸게 만든다** — 인스턴스끼리 맞출 상태가 없으므로
장비를 늘리는 건 클러스터를 만드는 게 아니라 독립 단위를 늘리는 것이다.
대신 장비 경계를 넘는 순간 **시계 동기(PTP)가 전제**가 된다
→ [deploy/host/60-ptp.md](deploy/host/60-ptp.md).

## 동작 모드

| 모드 | 목적 | 성능 | 레이어 |
|---|---|---|---|
| **A. Stateless 고속** | 대량 트래픽, L2~L4 이상동작 | PF당 line rate (× N) | L2~L4 |
| **B. Stateful 세션** | 실제 TLS+HTTP, handshake 제어 | 세션 수 제한 | L2~L7 |
| **C. Replay 재현** | 업로드 PCAP 그대로 재생/구간 재전송 | 재현 정확도 기준 | L2~L7 |

## 장비 — 검증기와 목표기 2단 구성

같은 코드가 두 장비에서 돈다. **PF 4개 = 인스턴스 4개** 배치가 양쪽 동일하므로
이식 시 바뀌는 것은 PMD 하나뿐이다.

| | ② 검증기 (현재) | ③ 목표기 (**미도입** — 향후 세팅) |
|---|---|---|
| 주소/모델 | `10.10.40.121` · Intel CAR5070 | 미정 |
| NIC | X710 **4×10G** `0000:43:00.0~3` → `i40e` | E810-CQDA1 **100G × 4** → `ice` |
| 총 대역 | **40G** (64B 59.5 Mpps 이론) | **400G** (64B **595.2 Mpps**) |
| CPU | Xeon Gold 5418N **24C/1소켓, HT off** (CPU1 빈 소켓) | **2소켓 × 32코어 이상, HT off** |
| 메모리 | 123GB 가용 (128GB DDR5-4800 8채널) | 256GB, 소켓당 8채널 전부 장착 |
| PCIe | Gen3 x8 실측 (`8GT/s x8`, 카드 최대치) | **x16 슬롯 4개**, 소켓당 2장 균등 배치 |
| OS | **Ubuntu 26.04 LTS · 커널 7.0 · gcc 15.2 · Python 3.14** | 검증기와 동일 계열로 맞춘다 |
| 역할 | Phase 0~6 개발·기능 검증 | Phase 7 성능(400G) 검증 |

**검증기는 2026-08-25 에 Ubuntu 26.04 로 재설치됐고, 그 결과 이전에 막혀 있던
제약이 해소됐다.** gcc 9.4 가 `-march=x86-64-v3` 를 몰라 호스트 빌드가 컨테이너와
다른 ISA(`haswell`)로 내려앉던 문제가 사라졌고, 이제 **컨테이너 없이도 같은 ISA 로
빌드된다** — C 코드를 이미지 빌드 없이 바로 컴파일 검증할 수 있다는 뜻이라
개발 반복 주기에 직접 영향이 있다.

목표기 OS 는 검증기와 같은 계열로 맞춘다. 두 장비의 ISA·툴체인이 갈리면
`-march` 를 낮은 쪽에 맞춰야 해서 성능 검증의 의미가 줄어든다.

- 검증기는 **증설로 400G가 되지 않는다** — CPU1 미장착 + x16 슬롯 3개가 라이저
  미장착으로 PCI 열거조차 안 되고, 그중 `Riser1T` 는 CPU1 측이다.
- ⚠️ **X710 계열은 소형 프레임에서 라인레이트 미달**(Intel 인정 HW 제약). 40G
  이론치를 그대로 믿지 말고 Phase 2 에서 실측치를 먼저 확보한다.
- **검증기 호스트 준비 완료(2026-08-25)** — VT-d 활성 · 1G hugepage 16개 ·
  `isolcpus=2-23` · X710 4포트 vfio-pci 바인딩 · Docker 29.7 설치.
  `40-verify-node.sh` 18 PASS / 0 FAIL. 절차는
  [deploy/host/10-kernel-cmdline.md](deploy/host/10-kernel-cmdline.md).

### 배선 (검증기 ↔ 대상 장비)

X710 4포트는 스위치를 거치지 않고 **점대점**으로 대상 장비에 물려 있다.
대상은 4포트를 `bond0` 으로 묶어 MAC 을 공유하므로, **어느 포트가 어디에
연결됐는지는 출발지 MAC 으로만 판별된다**.

| 검증기 | 출발지 MAC | 대상 (`10.10.40.123`, Rocky Linux 8.10) |
|---|---|---|
| dp0 `0000:43:00.0` | `08:35:71:37:d7:32` | `enp1s0f0` (bond0 슬레이브) |
| dp1 `0000:43:00.1` | `…:d7:33` | 미확정 |
| dp2 `0000:43:00.2` | `…:d7:34` | 미확정 |
| dp3 `0000:43:00.3` | `…:d7:35` | 미확정 |

대상 `bond0` MAC = `08:35:71:38:d4:8e`, IP `192.168.20.12`.
dp0↔f0 은 hello packet 으로 확인했다. 나머지는 같은 방법으로 확정할 수 있다.
대상 접근은 121 을 점프 호스트로 경유한다.

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
`4` handshake 제어 → `4-1` PCAP 리플레이 엔진 → `5a` TCP 데이터경로+평문 HTTP
→ `5b` TLS+HTTPS(mbedTLS) → `6a` GUI 라이브 대시보드 → **`6b` React Flow 빌더**
→ **`6c` 실시간 스트리밍(SSE)·uPlot 심화** → `7` 400G 튜닝 → `8` 클라우드 백엔드 이식.

`0`~`6` 은 검증기(40G)에서, `7` 은 목표기(400G)에서 수행한다.

---

## 실행 형태 — 두 가지를 모두 지원

| 형태 | 대상 | 오케스트레이션 | 절차 |
|---|---|---|---|
| **D — Docker Compose** (정본) | 운영 배포, 다중 PF, **장비 여러 대** | 없음. 장비마다 compose, 제어부는 함대 설정으로 관측 | [DEPLOYMENT.md](docs/DEPLOYMENT.md) |
| **S — 단독 실행 (컨테이너 없음)** | 컨테이너조차 반입 불가한 장비 | systemd 템플릿 유닛 + `taskset` | [INSTALL-STANDALONE.md](docs/INSTALL-STANDALONE.md) |

호스트 준비(VT-d·hugepage·vfio 바인딩·코어 격리)는 두 형태가 동일하다.
*"monolithic" 은 단일 프로세스가 아니다* — Go 와 C 를 한 프로세스로 합치면 cgo 로
DPDK 를 부르게 되어 아키텍처 전제가 무너진다. 합쳐지는 것은 배포 단위다.

### 왜 k8s 를 쓰지 않는가

초기 설계는 k3s 였고 Phase 0 에서 매니페스트까지 작성했으나, 코드와 대조한 뒤
접었다. 근거 셋:

1. **스케줄러가 정할 게 없다.** 데이터플레인은 PF 가 꽂힌 장비에서만 돌 수 있어
   배치를 PCIe 슬롯이 이미 결정했다. k8s 는 워크로드가 노드 간 **교체 가능**할 때
   값을 하는데 여기선 정의상 교체 불가능하다. 장비가 늘어도 마찬가지다.
2. **CPU Manager 가 명시적 cpuset 보다 못하다.** `cpu: "5"` 는 kubelet 이 코어를
   골라 주는 방식이라 어느 코어인지 미리 알 수 없고, kubelet 은 `isolcpus` 를
   모른다 — 격리 집합과 `reserved-cpus` 를 서로 다른 파일에서 손으로 맞춰야 했다.
   지금은 `isolcpus` 가 **유일한 출처**이고 compose 의 `cpuset` 이 그걸 그대로 쓴다.
3. **device plugin 이 없는 문제를 만든다.** PF↔인스턴스 바인딩이 임의가 되어
   축소 시 어느 PF 가 빠질지 알 수 없다. `MIR_DEVICE_SPEC=pci:<BDF>` 로 직접
   지정하면 고정되고, 이 경로는 이미 구현돼 있었다.

**컨테이너는 유지한다** — 목적이 성능이 아니라 **재현성**이기 때문이다. 바꾼 것은
오케스트레이션 층 하나다. (전환을 결정할 당시 검증기는 gcc 9.4 라 호스트에서
`-march=x86-64-v3` 빌드가 아예 불가능했다. 지금은 26.04 로 재설치되어 그 제약이
사라졌지만, 장비마다 툴체인이 갈리는 상황 자체는 언제든 재발한다.)

부수 효과로 제어부의 k8s 의존이 사라져 Go 의존성이 직접 3개로 줄었고,
`internal/k8s` 와 RBAC 표면이 통째로 없어졌다.

### 계층 경계

hugepage 크기·IOMMU·vfio 바인딩·CPU 격리는 전부 호스트 커널에 묶여 있어
컨테이너가 숨겨주지 못한다. 경계를 못박아 둔다.

- **NIC 할당**: PF 전체를 vfio-pci 로 패스스루 (VF 는 spoof check 에 막혀
  L2 임의조작·src IP 스푸핑이 불가능하다). `MIR_DEVICE_SPEC` 으로 BDF 직접 지정
- **코어 배치**: `dataplane` 은 `cpuset` 으로 격리 코어에 고정하고, `agent` 는
  cpuset 없이 공유 풀에 남긴다. 컨테이너가 분리돼 있어 사이드카의 gRPC 스레드가
  busy-poll 코어에 올라갈 물리적 경로가 없다
- **메모리 상한**: 오케스트레이터가 걸어 주던 인스턴스별 hugepage 한도가 없으므로
  `MIR_MEM_MB` 로 EAL 에 직접 건다. 없으면 먼저 뜬 인스턴스가 hugepage 를 전부
  가져가고 나머지가 기동에 실패한다
- **개수·리소스 조정**: compose 파일과 `.env` 가 소유한다. **제어부는 관측만 한다** —
  라이프사이클을 조종하려면 장비마다 `docker.sock`(root 등가)을 받아야 하는데,
  GUI 가 웹으로 노출되는 구조에서 후퇴이고 멀티 장비에서는 성립하지도 않는다

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
│  ├─ cmd/mir-control/  ← 제어부 (함대 관측 + REST)
│  └─ internal/  pb · ipc · agent · registry · fleet · mtls · api · webui
├─ web/                 ← React+TS GUI (Phase 6). Vite 빌드→ control/internal/webui/dist (go:embed)
└─ deploy/
   ├─ host/             ← 커널 파라미터 · vfio 바인딩 · docker 설치 · 인증서 · PTP
   ├─ docker/           ← Dockerfile 3개 (빌드 컨텍스트는 저장소 루트)
   └─ compose/          ← 데이터플레인/제어부 compose · .env 예제 · 함대 설정 예제
```

- **상세 요구사항·설계·미확정 이슈**의 단일 출처(Source of Truth)는
  [docs/REQUIREMENTS.md](docs/REQUIREMENTS.md). 세부 결정은 그 문서를 갱신한다.
- **코드를 건드리기 전에** [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) 를 읽을 것.
  채널 ①~⑤ 의 콜플로우(시퀀스 다이어그램), 스레드·코어 격리가 깨지는 지점,
  그리고 **각 Phase 가 어느 파일을 건드리는지**가 정리돼 있다.

## 현재 상태

- **Phase 0 인프라 골격 완성** — 호스트 준비 스크립트, 3개 이미지, compose 구성,
  제어 채널(C ⇄ 사이드카 ⇄ 제어부), 함대 관측 API 까지 작성됨.
- **k8s → Docker Compose 전환 완료 (코드·문서)** — `deploy/k8s` 와
  `control/internal/k8s` 제거, 함대 설정(`internal/fleet`)·mTLS(`internal/mtls`)
  추가, `registry` 를 `Resolver` 인터페이스로 추상화. **장비 여러 대를 한 제어부가
  묶는 구성이 요구사항에 포함됐다.**
- **✅ Phase 0·1 실물 검증 완료 (2026-08-25, 검증기 4 인스턴스)** — 아래 전부
  실장비 실측이다.
  - 인스턴스별 `lcores` 가 `.env` 의 cpuset 과 정확히 일치 (`[2-6] [7-11]
    [12-16] [17-21]`, 겹침 없음). **오케스트레이터 없이 코어 배타 점유가 된다**
  - hugepage 4GB × 4 = 16/16 소진. `--socket-mem` 선확보라 초과 시 즉시 기동 실패
  - 비특권 vfio (`IOMMU type 1`), `Vector AVX2` 경로, 이미지에 C++ 런타임 부재
  - 실패 상태 3종 재현: `unreachable` / `no-ports` / `pf-mismatch`.
    특히 `no-ports` 는 **`connected=true` 인데 NIC 을 못 잡은 상태**를 구분해 낸다
  - hello packet **송·수신 양단 확인**: 송신 100/100(NIC 카운터 일치) →
    대상에서 `100 captured / 0 dropped`, seq 0~99 누락·중복 없음,
    매직 `MIR1`·패딩·64B 프레임 전부 일치
- **C 코드가 처음으로 컴파일·실행됐다** — gcc 15.2 + DPDK 25.11, 경고 0건.
  `eal_args.c` 는 빌드 시스템 없이 떼어내 하네스로 단독 검증까지 했다.
- **실물이 잡아낸 버그 8건을 고쳤다** — 코드·문서만으로는 나올 수 없던 것들이다.
  `libatomic1` 누락(기동 실패) · stdout 블록 버퍼링(진단 유실) ·
  vfio 바인딩이 재부팅을 못 넘김(Ubuntu 26.04 의 driverctl 패키지 파손) ·
  `pipefail` + `grep -q` 5곳(패턴이 **맞을 때만** 실패) · 인증서 소유권 ·
  compose 의 hello 변수 미통과 · `lsmod` 기반 vfio 검사 · `rte_eth_link_to_str`
  실험적 API.
- **시나리오 송신 엔진은 여전히 미구현** (Phase 2~4-1). hello 는 기동 시 1회
  송신하는 경로 증명일 뿐 속도 제어도 다중 lcore 도 없다. `StartScenario` 는
  성공을 가장하지 않고 "미구현" Ack 를 돌려준다.
- `pcap_reader.h` 는 경로 기반 API 라 공유 볼륨 설계와 이미 맞물려 있다 (Phase 4-1).

## 미확정 이슈 (요약)

~~서버 사양~~(확정 — 위 장비 표) · ~~운영 반입 정본 형태~~(확정 — Docker Compose) ·
대상 환경(직결/루프백/스위치) · 판정 규칙 상세 · TLS 세부(1.2/1.3, 클라이언트 인증서) ·
PCAP 재현 세부(포맷/타이밍/구간) · **PTP 허용 오차**(판정 규칙이 정해져야 산출된다) ·
범위 밖 항목.
→ 전체 목록은 요구사항 정의서 7절 참고.

**멀티 장비 관련해 새로 열린 것**: 조율된 시작(`start_at_ns`)은 proto 에 자리만
잡혀 있고 송신 엔진(Phase 2)과 함께 구현된다. 인증서 갱신·배포 절차도 아직
수동이다.
