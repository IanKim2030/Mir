# 배포 가이드 — Docker Compose (단일/멀티 장비)

대상: 베어메탈 장비 1대 이상. 장비마다 NIC PF 를 N개 꽂고, PF 하나당
데이터플레인 인스턴스 하나를 띄운다. 제어부는 그중 한 대(또는 별도 장비)에서
하나만 돌면서 함대 전체를 관측한다.

> **오케스트레이터를 쓰지 않는 이유.** 배치를 정하는 주체가 스케줄러가 아니라
> **PCIe 슬롯**이다. `0000:43:00.0` 을 쓰는 컨테이너는 그 카드가 꽂힌 장비에서만
> 돌 수 있으므로 스케줄러가 결정할 것이 없다. 코어도 마찬가지로, 커널
> `isolcpus` 와 **같은 출처를 보는** 명시적 cpuset 이 "5개 달라"고 요청해서
> 받아쓰는 것보다 정확하다. 근거는 [REQUIREMENTS.md](REQUIREMENTS.md) 4-3 절.
>
> 컨테이너 자체는 유지한다 — **목적이 성능이 아니라 재현성**이기 때문이다.
> 장비마다 툴체인이 갈리면 `-march` 를 낮은 쪽에 맞춰야 하고, 그러면 성능
> 검증의 의미가 줄어든다.

---

## 0. 계층 분리 — 무엇을 누가 하는가

| 계층 | 담당 | 컨테이너가 대신 못 하는 것 |
|---|---|---|
| BIOS | VT-d, HT off, C-State off | — |
| 호스트 커널 | IOMMU, hugepage **생성**, `isolcpus`, vfio 바인딩 | 전부 |
| Compose | cpuset 고정, 장치 전달, hugepage **소비 상한**, 재시작 | IOMMU·hugepage 생성 |
| 애플리케이션 | EAL 인자 조립, 포트 구성, 판정 | — |

호스트 준비는 **장비마다 동일**하고 오케스트레이터 유무와 무관하다.

---

## 1. 호스트 준비 (생성 장비마다)

### 1-1. 커널 파라미터 — 재부팅 필요

[deploy/host/10-kernel-cmdline.md](../deploy/host/10-kernel-cmdline.md) 참조.

```
intel_iommu=on iommu=pt
default_hugepagesz=1G hugepagesz=1G hugepages=16
isolcpus=managed_irq,domain,2-23 nohz_full=2-23 rcu_nocbs=2-23
```

`isolcpus` 가 이제 **코어 격리의 유일한 출처**다. `.env` 의 `MIR_DP*_CPUSET` 은
반드시 이 집합 안에 들어가야 하고, `40-verify-node.sh` 가 그걸 검사한다.
(오케스트레이터를 쓸 때는 `isolcpus` 와 `reserved-cpus` 를 서로 다른 파일에서
손으로 맞춰야 했다. 그 이중 출처가 사라진 것이 이번 전환의 실질적 이득 하나다.)

hugepage 총량 ≥ `MIR_MEM_MB` × 인스턴스 수 여야 한다. 검증기 기준
4096MB × 4 = 16GB = `hugepages=16` 으로 딱 맞는다.

### 1-2. NIC 을 vfio-pci 로 바인딩

```bash
sudo deploy/host/20-bind-vfio.sh                      # 후보 목록
sudo deploy/host/20-bind-vfio.sh 0000:43:00.0 0000:43:00.1
```

출력의 `<bdf> → /dev/vfio/<group>` 이 그대로 `.env` 의 `MIR_DP*_PF` 와
`MIR_DP*_VFIO_GROUP` 이 된다.

> ⚠️ 관리 NIC 을 넘기면 SSH 가 끊긴다. 스크립트가 기본 라우트 netdev 를
> 보호하지만 `--force` 로 무력화할 수 있으니 주의할 것.

> PTP 를 쓸 계획이면 **바인딩 전에** `ethtool -T` 로 하드웨어 타임스탬핑을
> 확인해 둔다. vfio 로 넘긴 뒤에는 커널이 그 포트를 더 이상 보지 못한다.

### 1-3. Docker 설치

```bash
sudo deploy/host/30-install-docker.sh
```

### 1-4. 사전조건 점검

```bash
deploy/host/40-verify-node.sh
# 장비 2대 이상이면:
MIR_REQUIRE_PTP=1 deploy/host/40-verify-node.sh
```

FAIL 이 하나라도 있으면 진행하지 않는다. 여기서 걸러지는 문제는 나중에
"왜 안 뜨지"로 모습만 바뀔 뿐 사라지지 않는다.

---

## 2. 이미지 빌드

빌드 컨텍스트는 **저장소 루트**다 (`proto/` 를 함께 넣어야 한다).

```bash
docker build -f deploy/docker/Dockerfile.dataplane -t mir/dataplane:dev .
docker build -f deploy/docker/Dockerfile.agent     -t mir/agent:dev     .
docker build -f deploy/docker/Dockerfile.control   -t mir/control:dev   .
```

데이터플레인 이미지는 DPDK 를 소스에서 빌드하므로 수 분 걸린다.

### 장비가 여러 대일 때

레지스트리를 하나 두거나 이미지를 실어 나른다.

```bash
docker save mir/dataplane:dev mir/agent:dev | ssh gen-2 'docker load'
```

> `-march=x86-64-v3` 로 빌드되므로 **Haswell 이상**에서만 돈다. 장비 간 CPU
> 세대가 다르면 낮은 쪽에 맞춰야 한다 (`--build-arg MARCH=`).

---

## 3. 코드 생성 (proto 를 고쳤을 때만)

```bash
buf generate          # Go — control/internal/pb/ 에 떨어지고 커밋한다
```

C 쪽은 이미지 빌드 중 meson `custom_target` 이 자동 실행하므로 수동 실행이
필요 없다.

---

## 4. 인증서 발급

사이드카 포트(`:9100~`)는 장비 밖으로 열린다. **여기 닿는 누구나 라인레이트
패킷 제너레이터를 조종할 수 있으므로** mTLS 가 기본이다.

```bash
./deploy/host/50-gen-certs.sh certs/ 10.10.40.121 10.10.40.122
```

장비 주소는 `fleet.yaml` 의 `address` 와 **정확히 같아야 한다** — TLS 가 그
값을 서버 이름으로 검증하고, 스크립트가 그 값을 IP SAN 으로 넣는다.

배치:

| 장비 | 파일 |
|---|---|
| 생성 장비 | `ca.crt`, `agent-<주소>.crt` → `agent.crt`, `agent-<주소>.key` → `agent.key` |
| 제어 장비 | `ca.crt`, `control.crt`, `control.key` |

개발 중 끄려면 `.env` 에 `MIR_INSECURE=1`. 인증서 경로와 **함께 쓰면 기동을
거부한다** — "보안을 켰다고 믿는 채로 평문이 나가는" 상태가 가장 나쁜 결과라
의도적으로 막아 둔 것이다.

---

## 5. 배포

### 5-1. 생성 장비 (장비마다)

```bash
cd deploy/compose
cp dataplane.env.example .env
$EDITOR .env          # MIR_MACHINE, cpuset, BDF, vfio 그룹, 포트
```

**첫 기동은 인스턴스 하나로 한다.**

```bash
COMPOSE_PROFILES= docker compose -f docker-compose.dataplane.yml up -d
docker logs -f "$(grep ^MIR_MACHINE .env | cut -d= -f2)-dp0"
```

확인되면 늘린다 (`.env` 의 `COMPOSE_PROFILES` 가 적용된다).

```bash
docker compose -f docker-compose.dataplane.yml up -d
```

### 5-2. 제어 장비 (한 대)

```bash
cd deploy/compose
cp fleet.example.yaml fleet.yaml
$EDITOR fleet.yaml    # 장비·인스턴스·PF 목록
docker compose -f docker-compose.control.yml up -d
```

> `fleet.yaml` 의 `machines[].name` 과 각 생성 장비 `.env` 의 `MIR_MACHINE` 이
> 같아야 한다. 인스턴스 이름이 `<machine>-dp<id>` 로 만들어지고 제어부가 그
> 이름으로 기대와 실측을 잇는다. 어긋나면 전부 `unreachable` 로 보인다.

---

## 6. 검증

### 1단계 — 호스트

```bash
deploy/host/40-verify-node.sh
```

### 2단계 — 데이터플레인 기동 (Phase 0 완료 기준)

```bash
docker logs gen-1-dp0
```

나와야 하는 것:

```
  lcores      : 2,3,4,5,6  (5개, cpuset 에서 읽음)
  device      : pci:0000:43:00.0
  memory      : 4096 MB  (NUMA 노드 1개 중 node0 에 배정)
  EAL argv    : mir-dataplane -l 2,3,4,5,6 --file-prefix gen-1-dp0
                --proc-type=primary --socket-mem 4096 --socket-limit 4096
                -a 0000:43:00.0
...
rte_eal_init 성공 (main lcore=2, lcore 수=5)
포트 1개 인식
port 0: driver=net_i40e mac=... numa=0 ...
```

**`lcores` 가 `.env` 의 `MIR_DP0_CPUSET` 과 정확히 일치**해야 한다. 다르면
cpuset 이 안 먹은 것이고, 그 상태로는 배타 코어 배치가 통째로 무의미하다.

`memory` 줄이 "상한 없음"이면 `MIR_MEM_MB` 가 안 들어간 것이다. 인스턴스를
여러 개 띄울 계획이면 여기서 멈추고 고친다 — 먼저 뜬 쪽이 hugepage 를 전부
가져간다.

### 3단계 — 사이드카 연결

```bash
docker logs gen-1-agent0
# "데이터플레인 연결됨" → "데이터플레인 준비 확인 ports=1 ..."
```

`ports=0` 이면 **SERVING 으로 올라가지 않는다.** 이건 버그가 아니라 의도된
동작이다 — 데이터플레인은 NIC 을 못 잡아도 진단을 위해 계속 살아 있으므로,
"살아 있음"이 "정상"으로 오해되지 않도록 사이드카가 막는다.

### 4단계 — 코어 분리 (사이드카 설계의 핵심 검증)

```bash
# 데이터플레인은 격리 코어에만
docker inspect -f '{{.HostConfig.CpusetCpus}}' gen-1-dp0

# 사이드카는 제한 없음 = 공유 풀
docker inspect -f '{{.HostConfig.CpusetCpus}}' gen-1-agent0

# 실제 스레드 배치
docker top gen-1-dp0 -o pid,psr,comm
```

사이드카 스레드가 데이터플레인 코어에 올라가 있으면 격리가 깨진 것이다.

### 5단계 — 제어부에서 함대 보기

```bash
curl -s localhost:8080/healthz | jq          # liveness — 항상 200
curl -s localhost:8080/readyz  | jq          # 기대한 인스턴스가 다 붙었는가
curl -s localhost:8080/api/capacity | jq     # 장비 단위 롤업
curl -s localhost:8080/api/dataplanes | jq   # 인스턴스 단위 상세
```

`state` 가 넷으로 갈린다. **이 구분이 오케스트레이터를 걷어내며 오히려 좋아진
부분이다** — 기대(설정)와 실측(HelloResponse)을 따로 들고 대조하기 때문이다.
노드 allocatable 합산으로는 "파드 몇 개가 Ready 인가"밖에 알 수 없었다.

| state | 뜻 | 볼 곳 |
|---|---|---|
| `ok` | 설정한 PF 를 실제로 잡았다 | — |
| `unreachable` | 연결 자체가 안 된다 | 컨테이너 상태, 방화벽, 포트, 인증서 |
| `no-ports` | 떠 있는데 NIC 을 못 잡았다 | vfio 바인딩, `MIR_DEVICE_SPEC` |
| `pf-mismatch` | 다른 PF 를 잡았다 | `.env` 와 `fleet.yaml` 의 BDF 불일치 |

### 6단계 — hello packet 송신 (Phase 1 완료 기준)

> ⚠️ **실제로 선로에 프레임이 나간다.** 본인 소유/승인된 링크에서만 할 것.

송신을 켜는 변수는 **인스턴스별**이다(`MIR_DP<i>_HELLO_COUNT`). 전역 스위치를
두지 않은 이유는 4포트가 한꺼번에 쏘는 사고를 막기 위해서다.

```bash
MIR_DP0_HELLO_COUNT=1000 \
  docker compose -f docker-compose.dataplane.yml up -d --force-recreate dp0 agent0

docker logs gen-1-dp0 | grep hello
#   hello 송신: port=0 count=1000 size=64B burst=32 dst=ff:ff:ff:ff:ff:ff
#   hello 송신 완료: 1000/1000 전송, 0 폐기 (64000 bytes)
```

**자기 보고만 믿지 말고 NIC 카운터로 대조한다.** tx_burst 가 mbuf 를 받아
갔다는 것과 선로에 나갔다는 것은 다른 사실이다.

```bash
curl -s localhost:8080/api/dataplanes | jq '.[] | {name, tx: .ports[0].stats.txPkts}'
#   {"name":"gen-1-dp0","tx":1000}   ← 자기 보고와 일치해야 한다
#   {"name":"gen-1-dp1","tx":0}      ← 나머지는 0 (인스턴스별 게이트 확인)
```

프레임은 **순수 L2** 다 — `ETH + EtherType 0x88B5 + "MIR1" 매직 + seq +
타임스탬프`. IP 헤더가 없으므로 대상 IP 를 지정할 수 없다. 특정 장비로만
보내려면 MAC 을 준다.

```bash
MIR_HELLO_DST_MAC=aa:bb:cc:dd:ee:ff MIR_DP0_HELLO_COUNT=1000 docker compose ... 
```

상대 쪽에서 도착을 확인하려면 그 장비에서:

```bash
sudo tcpdump -i <iface> -e -XX ether proto 0x88B5
```

미설정이 기본이라 평소에는 기동만으로 프레임이 나가지 않는다. 검증 후 되돌린다.

### 7단계 — 장비 2대 (멀티 장비 구성일 때만)

```bash
# 두 장비의 인스턴스가 모두 붙는가
curl -s localhost:8080/api/capacity | jq '.machines'

# 한 대의 컨테이너를 죽여 unreachable 로 잡히는지
ssh gen-2 'docker stop gen-2-dp0'
curl -s localhost:8080/api/dataplanes | jq '.[] | select(.state != "ok")'

# 평문 접속이 거부되는지 (mTLS 회귀 테스트) — 실패해야 정상이다
grpcurl -plaintext 10.10.40.121:9100 list

# 시계 동기
journalctl -u 'ptp4l@*' | grep 'master offset' | tail -5
```

---

## 7. 트러블슈팅

| 증상 | 원인 | 조치 |
|---|---|---|
| `rte_eal_init 실패` + hugepage 언급 | 상한 × 인스턴스 수 > 호스트 총량 | `MIR_MEM_MB` 를 낮추거나 `hugepages=` 를 올린다 |
| `EAL: ... group not viable` | IOMMU 그룹에 커널 드라이버를 쓰는 다른 장치가 있다 | 같은 그룹의 장치를 전부 vfio 로 넘기거나 슬롯을 옮긴다 |
| 포트 0개 인식 | vfio 미바인딩 / BDF 오타 | `ls /dev/vfio/`, `.env` 의 `MIR_DP*_PF` 확인 |
| `lcores` 가 `.env` 와 다름 | cpuset 미적용 (cgroup v1 등) | `docker info --format '{{.CgroupVersion}}'` 확인 |
| 인스턴스 2개째부터 기동 실패 | hugepage 경쟁 | `MIR_MEM_MB` 총합 재계산 |
| `unreachable` (전부) | `MIR_MACHINE` ≠ `fleet.yaml` 의 name | 두 값을 일치시킨다 |
| `unreachable` (일부) | 포트 불일치, 방화벽 | `.env` 의 `MIR_DP*_PORT` 와 `fleet.yaml` 의 `port` |
| TLS 핸드셰이크 실패 | SAN 에 그 주소가 없다 | `fleet.yaml` 의 address 로 인증서를 다시 발급 |
| 제어부 기동 실패 | 함대 설정 없음/오타 | `MIR_FLEET_CONFIG` 경로 확인. 오타 난 키는 파싱 단계에서 거부된다 |

### 개수·리소스를 바꾸려면

제어부에 그런 API 는 **없다**. Compose 파일과 `.env` 가 소유한다.

```bash
$EDITOR .env                                            # cpuset, MIR_MEM_MB
docker compose -f docker-compose.dataplane.yml up -d    # 바뀐 것만 재생성
```

DPDK 는 런타임 lcore 변경을 지원하지 않으므로(`rte_eal_init` 이 고정한다)
어떤 방식이든 재시작을 수반한다. 그래서 선언적 파일이 맞는 자리다. 제어부에
라이프사이클 권한을 주지 않는 이유는 `control/internal/api` 패키지 주석 참조 —
요약하면 그러려면 장비마다 `docker.sock`(root 등가)을 받아야 하는데, GUI 가
웹으로 노출되는 구조에서 명백한 후퇴이고 멀티 장비에서는 애초에 성립하지 않는다.

---

## 관련 문서

- [REQUIREMENTS.md](REQUIREMENTS.md) — 요구사항·설계·미확정 이슈 (SoT)
- [ARCHITECTURE.md](ARCHITECTURE.md) — 채널 구조와 모듈 분해
- [INSTALL-STANDALONE.md](INSTALL-STANDALONE.md) — 컨테이너 없이 systemd 로 (S 형태)
- [deploy/host/10-kernel-cmdline.md](../deploy/host/10-kernel-cmdline.md) — 커널 파라미터 상세
- [deploy/host/60-ptp.md](../deploy/host/60-ptp.md) — 장비 간 시계 동기
