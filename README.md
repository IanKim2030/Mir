# Mir — 패킷 제너레이터

L2~L7 헤더를 자유롭게 조립한 패킷을 **NIC 포트(PF) 라인레이트로** 전송하고,
업로드한 PCAP 을 그대로 재현하며, 비정상 handshake 를 포함한 프로토콜 동작을
**테스트·검증**하는 리눅스 기반 도구. DPDK 로 커널을 우회해 소켓 API 로는 불가능한
handshake 통제(ACK 생략, 조기 RST 등)를 직접 한다.

> ⚠️ **전제**: 대상은 항상 본인 소유/테스트 승인된 장비. 제3자 IP 대상 트래픽 생성은 범위 밖.

**성능 보장 단위는 PF 하나**다. 지원 속도 1G/10G/100G, PF 개수 N 은 장비가 허용하는
만큼 확장되며 총 대역은 `PF 라인레이트 × N`. PF 하나당 인스턴스 하나라 NIC 을 늘리는
데 코드 변경이 없다.

## 동작 모드

| 모드 | 목적 | 레이어 |
|---|---|---|
| **A. Stateless 고속** | 대량 트래픽, L2~L4 이상동작 | L2~L4 (PF당 라인레이트 × N) |
| **B. Stateful 세션** | 실제 TLS+HTTP, handshake 통제 | L2~L7 (세션 수 제한) |
| **C. Replay 재현** | 업로드 PCAP 재생 / 구간 재전송 | L2~L7 |

## 아키텍처

3계층을 별도 프로세스로 분리한다 (Go↔DPDK cgo 오버헤드 회피).

```
[React + TS GUI]  ── SSE(텔레메트리) / REST(명령) ──▶  [mir-control (Go)]  전체 1개
                                                            │ gRPC 스트리밍 · mTLS (장비 경계를 넘음)
                                                            ▼
                    [인스턴스 × N]  ← NIC PF 하나당 하나 (장비 여러 대에 걸쳐도 됨)
                      ├─ mir-agent     (Go)     사이드카. gRPC ⇄ unix socket 릴레이
                      └─ mir-dataplane (C/DPDK) NIC bind·패킷 빌더·TX/RX·실시간 판정 (배타 코어)
```

- **데이터플레인 = 순수 C + DPDK** (C++ 의존성 없음 — TLS 도 순수 C 인 mbedTLS).
- **개별 패킷은 프로세스 경계를 넘지 않는다** — C 가 판정 결과·요약만 올린다.
  그래서 제어 채널이 끊겨도 판정 정확도에 영향이 없고, 멀티 장비가 싸진다.
- 제어부는 **단일 Go 바이너리**로 React GUI 를 `go:embed` 로 내장해 서빙한다.

상세는 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), 요구사항·설계 결정의 단일
출처는 [docs/REQUIREMENTS.md](docs/REQUIREMENTS.md).

## 현재 상태

**Phase 0~6 완료 — 전부 검증기(X710 4×10G) 실물 실측으로 검증.** 세부는
[CLAUDE.md](CLAUDE.md#현재-상태). 요약:

- L2~L4 고속 송신(64B ~12–14 Mpps, X710 소형프레임 HW 한계까지) · 수신 판정 ·
  handshake 통제 · PCAP 리플레이(pcap/pcapng) · TCP 데이터경로 + HTTP ·
  **TLS 1.2/1.3 + mTLS**(mbedTLS) · **React GUI**(대시보드 + React Flow 빌더 +
  SSE 실시간 스트리밍).
- **Phase 7 (400G 튜닝)** — 목표기(E810 100G×4) 미도입이라 대기. X710 로는 64B
  라인레이트가 실리콘에서 막힌다.
- **Phase 8 (클라우드 백엔드 이식)** — 미착수.

## 빠른 시작

호스트 준비(VT-d·hugepage·vfio 바인딩·코어 격리)가 선행이다 —
[deploy/host/](deploy/host/) 와 [docs/DEPLOYMENT.md](docs/DEPLOYMENT.md).

```bash
# 1. proto 코드 생성
buf generate

# 2. GUI 빌드 (control 에 go:embed 됨)
cd web && npm install && npm run build && cd ..

# 3. 이미지 빌드 (컨텍스트 = 저장소 루트)
docker build -f deploy/docker/Dockerfile.dataplane -t mir/dataplane:dev .
docker build -f deploy/docker/Dockerfile.control   -t mir/control:dev .
docker build -f deploy/docker/Dockerfile.agent     -t mir/agent:dev .

# 4. 배포 (장비마다 데이터플레인, 전체에 제어부 1개)
cd deploy/compose
cp dataplane.env.example .env && $EDITOR .env      # 장비별 PF·cpuset·mem
docker compose -f docker-compose.dataplane.yml up -d
docker compose -f docker-compose.control.yml   up -d

# 5. GUI → http://<제어부 장비>:8080
```

컨테이너조차 반입 불가한 장비는 단독 실행(S) 형태 →
[docs/INSTALL-STANDALONE.md](docs/INSTALL-STANDALONE.md).

## 저장소 구조

```
Mir/
├─ CLAUDE.md               ← 프로젝트 개요 / 진입점 (여기부터 읽어도 됨)
├─ proto/dataplane.proto   ← 두 hop 공용 스키마 + gRPC service
├─ dataplane/              ← C/DPDK. 순수 C, meson 빌드
├─ control/                ← 단일 Go 모듈. mir-control / mir-agent + 내장 GUI(webui)
├─ web/                    ← React+TS GUI (Vite → control/internal/webui/dist)
├─ deploy/  host · docker · compose
└─ docs/    REQUIREMENTS · ARCHITECTURE · DEPLOYMENT · INSTALL-STANDALONE
```

## 개발 시 유의

- **코드를 건드리기 전에** [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) 를 읽을 것 —
  채널 콜플로우, 스레드·코어 격리가 깨지는 지점, Phase↔파일 매핑이 정리돼 있다.
- 데이터플레인은 **순수 C 유지**가 설계 제약이다 — 이미지에 libstdc++ 가 보이면
  설계가 새는 것이다.
- 성능·정확도 검증은 **vfio(Tier 1)** 에서만 유효하다. `af_xdp`/`af_packet` 은
  기능 개발용(커널이 NIC 을 계속 소유해 handshake 시 RST 를 쏜다).

## 라이선스

[Apache-2.0](LICENSE).
