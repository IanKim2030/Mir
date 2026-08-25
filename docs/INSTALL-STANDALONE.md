# 설치 가이드 — 단독 실행 형태 (컨테이너 없음)

베어메탈 장비 한 대에 **컨테이너 없이** Mir 를 설치한다.
요구사항 정의서의 **S 형태**([REQUIREMENTS.md](REQUIREMENTS.md) 4-3-5)에 해당한다.

> 정본 형태(Docker Compose)로 배포하려면 이 문서가 아니라 [DEPLOYMENT.md](DEPLOYMENT.md) 를 본다.

## 0. 범위 — 무엇을 얻고 무엇을 못 얻는가

두 형태는 **호스트 준비까지 완전히 동일**하고, 컨테이너 런타임을 설치하는 지점에서 갈린다.

```
[공통] 10-kernel-cmdline.md → 20-bind-vfio.sh
           │
           ├─ D 형태 → 30-install-docker.sh → 이미지 빌드 → compose up   (DEPLOYMENT.md)
           └─ S 형태 → 네이티브 빌드 → systemd 유닛 → systemctl start   (이 문서)
```

| 이 문서로 얻는 것 | 아직 못 얻는 것 |
|---|---|
| `mir-dataplane` 기동, NIC 포트 인식·start, 링크 확인 | **GUI** (Phase 6 미착수) |
| `mir-agent` 를 통한 gRPC 접근 (Hello / 텔레메트리) | **mir-control** — 이제 기동한다 (7절) |
| Phase 1 hello packet 송신 검증 | 시나리오 송신 엔진 (Phase 2~4-1 미구현) |

즉 이 단계의 운용 창구는 `systemctl` + `journalctl` + `grpcurl` 이다.

> ⚠️ **전제**: 대상은 항상 본인 소유/테스트 승인된 장비.

---

## 1. 사전 조건 (K 형태와 동일)

### 1-1. 커널 파라미터

[deploy/host/10-kernel-cmdline.md](../deploy/host/10-kernel-cmdline.md) 를 **그대로** 적용한다.
IOMMU · 1GB hugepage · `isolcpus` 세 가지 모두 필요하고, 컨테이너가 아니라고 해서
면제되는 항목은 하나도 없다.

**단 한 가지 재해석**: 문서의 코어 배분 규칙에서 `isolcpus` 의 **여집합**이 K 형태에서는
kubelet `reserved-cpus` 였지만, S 형태에서는 *OS + 사이드카(+ 나중에 제어부)* 의 몫이 된다.
경계의 의미는 같다 — **격리 코어는 데이터플레인 전용이고 그 밖의 프로세스가 올라가면 안 된다.**

```
전체 코어 0-31
  ├─ 0,1        → OS / 시스템 데몬 / mir-agent / (후속) mir-control
  └─ 2-31       → isolcpus. mir-dataplane 만 taskset 으로 올라간다
```

### 1-2. NIC 을 vfio-pci 로 바인딩

```bash
sudo ./deploy/host/20-bind-vfio.sh                    # 후보 목록
sudo ./deploy/host/20-bind-vfio.sh 0000:3b:00.0       # 바인딩
```

스크립트 수정은 필요 없다. 마지막 줄의 `다음: ./30-install-docker.sh` 안내와
`sriovdp-config.yaml 에 반영할 것` 안내는 **S 형태에서 무시**한다. 대신 출력된 BDF 를
3절의 인스턴스 설정 파일에 적는다.

`driverctl` 이 설치돼 있어야 재부팅 후에도 바인딩이 유지된다(`sudo apt install driverctl`).

### 1-3. Docker 설치 — 건너뛴다

`30-install-docker.sh` 는 실행하지 않는다.

### 1-4. 사전조건 점검

```bash
./deploy/host/40-verify-node.sh
```

이 스크립트는 K 형태 기준이라 **1~3절(커널/IOMMU · Hugepage · vfio 바인딩)만 유효**하다.
4절(Docker)은 FAIL 로 나오는 것이 정상이며, 그 때문에 **종료 코드가
1 이 되므로 신뢰하지 말 것**. 1~3절에 FAIL 이 없으면 다음으로 넘어간다.

---

## 2. 빌드 — 대상 장비에서 소스 빌드

버전과 옵션은 [Dockerfile.dataplane](../deploy/docker/Dockerfile.dataplane) 과 **문자열
단위로 동일**하게 맞춘다. 여기서 갈리면 "컨테이너에서만 되는 문제"나 그 반대가 생기고,
그 순간 두 형태를 오가며 디버깅하는 것이 불가능해진다.

### 2-1. 빌드 도구

```bash
sudo apt update && sudo apt install -y \
    build-essential meson ninja-build pkg-config \
    python3-pyelftools libnuma-dev \
    libprotobuf-c-dev protobuf-c-compiler \
    wget xz-utils ca-certificates
```

### 2-2. DPDK 25.11

```bash
DPDK_VERSION=25.11
MARCH=x86-64-v3

wget -q "https://fast.dpdk.org/rel/dpdk-${DPDK_VERSION}.tar.xz" -O /tmp/dpdk.tar.xz
mkdir -p /tmp/dpdk && tar -xJf /tmp/dpdk.tar.xz -C /tmp/dpdk --strip-components=1
cd /tmp/dpdk

meson setup build \
    --prefix=/usr/local \
    --buildtype=release \
    -Dplatform=generic \
    -Dcpu_instruction_set=${MARCH} \
    -Dtests=false \
    -Denable_docs=false
ninja -C build
sudo ninja -C build install
sudo ldconfig
```

- **`-Dplatform=generic` 이 핵심.** 없으면 DPDK 가 빌드 머신에 맞춰 `-march=native` 로
  최적화한다. 지금은 빌드 머신 = 실행 머신이라 당장은 돌지만, 같은 바이너리를 다른
  장비에 복사하는 순간 `SIGILL` 로 죽는다. 이미지와 값을 맞춰 두는 편이 안전하다.
- 배포판 `dpdk-dev` 패키지를 쓰지 않는 이유도 같다 — 버전이 조용히 바뀌면 재현성이 깨진다.

`ldconfig` 를 빠뜨리면 실행 시 `libdpdk` 를 찾지 못한다(10절).

### 2-3. mir-dataplane

```bash
cd <저장소>/dataplane
meson setup build --prefix=/usr/local --buildtype=release -Dmarch=x86-64-v3
ninja -C build
sudo ninja -C build install       # → /usr/local/bin/mir-dataplane
```

`.proto` → C 코드 생성은 meson 의 custom_target 이 `protoc-c` 로 알아서 수행한다.
따로 실행할 것은 없다.

### 2-4. mir-agent

Go **1.26 이상**이 필요하다([control/go.mod](../control/go.mod)). 배포판 패키지 버전이
낮으면 go.dev 타르볼을 쓴다.

```bash
cd <저장소>/control
go build -o /tmp/mir-agent ./cmd/mir-agent
sudo install -m 0755 /tmp/mir-agent /usr/local/bin/mir-agent
```

`mir-control` 도 함께 빌드할 수 있다 — 오케스트레이터 없이 기동한다(7절).

---

## 3. 설치 레이아웃

| 경로 | 내용 | K 형태의 무엇에 대응 |
|---|---|---|
| `/usr/local/bin/mir-dataplane`, `/usr/local/bin/mir-agent` | 바이너리 | 컨테이너 이미지 |
| `/etc/mir/dp-<N>.env` | 인스턴스별 설정 | compose `environment:` |
| `/run/mir/dp-<N>.sock` | 채널 ③ unix socket | `ipc` emptyDir |
| `/var/lib/mir/pcap/` | 리플레이 PCAP (Phase 4-1) | `mir-pcap` PVC |

```bash
sudo mkdir -p /etc/mir /var/lib/mir/pcap
```

### 3-1. 인스턴스 설정 — `/etc/mir/dp-0.env`

```ini
# 이 파일 하나가 인스턴스 하나를 정의한다.

# ★ 인스턴스마다 반드시 달라야 한다. EAL --file-prefix 로 쓰이며,
#   같으면 hugetlbfs 파일이 충돌해 두 번째 프로세스가 뜨지 못한다 (6절).
HOSTNAME=mir-dp-0

# 20-bind-vfio.sh 가 출력한 BDF.
MIR_DEVICE_SPEC=pci:0000:3b:00.0

# 채널 ③ 소켓. 사이드카가 같은 값을 봐야 한다.
MIR_IPC_SOCKET=/run/mir/dp-0.sock

# 이 인스턴스가 쓸 코어. isolcpus 안에서 고르고 인스턴스끼리 겹치면 안 된다.
# 데이터플레인은 sched_getaffinity 로 이 집합을 읽어 EAL -l 목록을 만든다.
MIR_LCORES=2-6

MIR_LOG_LEVEL=info
```

`MIR_LCORES` 는 DPDK 가 직접 읽는 값이 아니라 **taskset 에 넘기는 값**이다(4절).

---

## 4. systemd 유닛

아래 세 파일은 **문법만 `systemd-analyze verify` 로 확인**했고(systemd 255 기준, 오류 없음),
**실장비에서 실제로 기동해 본 적은 없다**. 설치 후 아래로 다시 확인할 수 있다.

```bash
sudo systemd-analyze verify mir-dataplane@0.service mir-agent@0.service mir@0.target
```

### 4-1. `/etc/tmpfiles.d/mir.conf`

```
d /run/mir 0755 root root -
```

```bash
sudo systemd-tmpfiles --create /etc/tmpfiles.d/mir.conf
```

### 4-2. `/etc/systemd/system/mir-dataplane@.service`

```ini
[Unit]
Description=Mir dataplane (C/DPDK) instance %i
# hugetlbfs 가 없으면 EAL 초기화 자체가 실패한다.
RequiresMountsFor=/dev/hugepages
After=network-pre.target

[Service]
Type=exec
EnvironmentFile=/etc/mir/dp-%i.env

# taskset 으로 코어를 고정한다. 데이터플레인은 sched_getaffinity 로 이 집합을
# 그대로 읽어 EAL lcore 목록을 만들므로, 여기가 곧 lcore 지정이다.
ExecStart=/usr/bin/taskset -c ${MIR_LCORES} /usr/local/bin/mir-dataplane

# DPDK 는 hugepage 와 vfio DMA 버퍼를 잠근다. 컨테이너의 IPC_LOCK 에 해당하며,
# 이게 없으면 "Cannot set up DMA remapping" 류로 실패한다.
LimitMEMLOCK=infinity

# rte_eal_cleanup 과 hugepage 반납에 시간을 준다.
# (K 형태의 terminationGracePeriodSeconds: 30 과 같은 값)
TimeoutStopSec=30
KillSignal=SIGTERM

Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

`CPUAffinity=` 를 쓰지 않은 이유: 유닛 지시어라 템플릿에서 인스턴스별로 값을 줄 수 없고,
드롭인 파일을 따로 만들어야 해서 인스턴스 설정이 두 곳으로 쪼개진다. taskset 을 쓰면
`/etc/mir/dp-<N>.env` 하나만 보면 된다.

### 4-3. `/etc/systemd/system/mir-agent@.service`

```ini
[Unit]
Description=Mir agent (sidecar) instance %i
# 데이터플레인이 죽으면 같이 내린다. 반대로 사이드카만 재시작하는 것은 가능하다.
BindsTo=mir-dataplane@%i.service
After=mir-dataplane@%i.service

[Service]
Type=exec
EnvironmentFile=/etc/mir/dp-%i.env
Environment=MIR_GRPC_LISTEN=:910%i
ExecStart=/usr/local/bin/mir-agent

# 사이드카는 공유 풀에서 돈다 — isolcpus 밖에 남겨 두는 것이 이 설계의 핵심이다.
# taskset 을 걸지 않으면 커널이 알아서 비격리 코어에만 올린다.

Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

`MIR_GRPC_LISTEN=:910%i` 규약이라 인스턴스 0~9 까지만 자연스럽게 커진다. 그 이상은
포트를 명시적으로 정해야 한다.

`mir-dataplane` 이 아직 소켓을 만들지 않았어도 사이드카는 재시도하며 붙으므로
기동 순서를 엄격히 맞출 필요는 없다.

### 4-4. `/etc/systemd/system/mir@.target`

```ini
[Unit]
Description=Mir instance %i (dataplane + agent)
Wants=mir-dataplane@%i.service mir-agent@%i.service
After=mir-dataplane@%i.service mir-agent@%i.service

[Install]
WantedBy=multi-user.target
```

### 4-5. 컨테이너가 하던 일과의 대응

| K 형태 | S 형태 |
|---|---|
| CPU Manager `static` 배타 코어 | `isolcpus` + `taskset -c ${MIR_LCORES}` |
| `securityContext.capabilities: IPC_LOCK` | `LimitMEMLOCK=infinity` |
| SR-IOV Device Plugin (`PCIDEVICE_*`) | `MIR_DEVICE_SPEC=pci:<BDF>` |
| `terminationGracePeriodSeconds: 30` | `TimeoutStopSec=30` |
| `ipc` emptyDir | `/run/mir` (tmpfiles.d) |
| `mir-pcap` PVC | `/var/lib/mir/pcap` |
| 컨테이너 재시작 정책 | `Restart=on-failure` |

### 4-6. 실행 사용자

당분간 **root** 로 돌린다. K 형태의 데이터플레인 컨테이너도 root 이며(`/dev/vfio` 와
hugetlbfs 접근), 여기서 권한 모델을 새로 만들면 검증되지 않은 변수가 하나 늘어난다.

비-root 로 좁히려면 `/dev/vfio/<group>` 에 대한 그룹 권한과
`AmbientCapabilities=CAP_IPC_LOCK CAP_SYS_NICE` 가 필요하다. **미검증**이며,
제어부가 `systemctl` 로 인스턴스를 조종하게 될 때 권한 모델과 함께 결정한다
(REQUIREMENTS 7절 6-1).

---

## 5. 기동과 검증

```bash
sudo systemctl daemon-reload
sudo systemctl start mir@0.target
```

### 5-1. 데이터플레인 로그

```bash
journalctl -u mir-dataplane@0 -f
```

순서대로 나와야 한다.

```
mir-dataplane 0.1.0
  lcores      : 2,3,4,5,6  (5개, cpuset 에서 읽음)
  device      : pci:0000:3b:00.0
  file-prefix : mir-dp-0
...
rte_eal_init 성공 (main lcore=2, lcore 수=5)
port 0: driver=net_ice mac=... numa=0 rxq_max=... txq_max=...
port 0 준비 완료: rxq=1(1024 desc) txq=1(1024 desc) socket=0 pool=mir_mp_p0
port 0: Link up at 100 Gbps FDX Autoneg
listen /run/mir/dp-0.sock
```

`lcores` 가 `MIR_LCORES` 와 다르면 taskset 이 안 먹은 것이고, `file-prefix` 가 의도한
값이 아니면 env 파일의 `HOSTNAME` 이 전달되지 않은 것이다.

### 5-2. 채널 연결

```bash
ls -l /run/mir/dp-0.sock
journalctl -u mir-agent@0 -n 20

grpcurl -plaintext localhost:9100 list
grpcurl -plaintext localhost:9100 mir.v1.DataPlane/Hello      # 포트 목록이 나와야 한다
grpcurl -plaintext localhost:9100 mir.v1.DataPlane/StreamTelemetry | head
```

### 5-3. Phase 1 hello packet

`/etc/mir/dp-0.env` 에 **임시로** 추가하고 재시작한다.

```ini
MIR_HELLO_TX_COUNT=1000
```

```bash
sudo systemctl restart mir-dataplane@0
journalctl -u mir-dataplane@0 | grep hello
#   hello 송신 완료: 1000/1000 전송, 0 폐기 (64000 bytes)
```

환경변수 표 · tcpdump 확인 · NIC 카운터 대조 방법은 중복해 적지 않는다 →
[DEPLOYMENT.md](DEPLOYMENT.md) 5절 **3-1단계**.

**검증이 끝나면 반드시 이 줄을 지운다.** 서비스가 뜰 때마다 선로에 프레임이
나가는 상태로 두지 않는다.

---

## 6. PF 를 여러 장 쓸 때

인스턴스 `N` 마다 `/etc/mir/dp-<N>.env` 를 만들고 `systemctl start mir@<N>.target` 한다.
세 가지가 인스턴스마다 **반드시 달라야** 한다.

| 값 | 왜 |
|---|---|
| `HOSTNAME` | EAL `--file-prefix` 가 된다. 같으면 hugetlbfs 파일이 충돌해 두 번째가 못 뜬다 |
| `MIR_DEVICE_SPEC` | PF 하나당 프로세스 하나 |
| `MIR_LCORES` | 코어가 겹치면 두 데이터플레인이 같은 코어에서 busy-poll 한다 |
| `MIR_IPC_SOCKET` | 소켓 경로 충돌 |

`HOSTNAME` 이 특히 함정이다 — compose 는 인스턴스마다 다른 값을 넣어 주지만, 한 호스트에서
직접 띄울 때는 설정하지 않으면 데이터플레인이 `gethostname()` 으로 폴백해 **모든
인스턴스가 같은 prefix** 를 쓰게 된다.

### hugepage 상한 — `MIR_MEM_MB` 로 해결됐다

인스턴스별 hugepage 상한이 없어 프로세스 간 경쟁을 막을 수단이 없던 문제는
**애플리케이션 계층으로 옮겨 해결됐다.** `eal_args.c` 가 `MIR_MEM_MB` 를 읽어
EAL 의 `--socket-mem` / `--socket-limit` 을 만든다.

```ini
MIR_MEM_MB=4096
```

`--socket-mem` 은 기동 시 선확보이므로 hugepage 가 모자라면 **폴트 시점 SIGBUS 가
아니라 즉시 기동 실패**로 드러난다. 이 fail-fast 성질이 상한의 핵심이다.
NUMA 노드는 `/sys` 에서 읽어 그 프로세스의 cpuset 이 걸친 소켓에만 배정한다.

> 설정하지 않으면 기동 로그에 경고가 남는다. 인스턴스를 여러 개 띄운다면
> **반드시 설정할 것** — 먼저 뜬 쪽이 호스트 hugepage 를 전부 가져간다.

---

## 7. mir-control — 이제 오케스트레이터 없이 뜬다

제어부의 k8s 전제는 제거됐다. 붙을 대상은 **함대 설정**이 알려 준다.

```yaml
# /etc/mir/fleet.yaml
machines:
  - name: gen-1
    address: 127.0.0.1        # 같은 장비면 루프백
    instances:
      - { id: 0, port: 9100, pf: "0000:43:00.0" }
```

```bash
MIR_FLEET_CONFIG=/etc/mir/fleet.yaml MIR_INSECURE=1 ./mir-control
```

- `registry` 가 `Resolver` 인터페이스를 받고 함대 설정이 그걸 구현한다.
- 한 장비 안에서만 돌면 `MIR_INSECURE=1` 로 평문을 써도 된다. **장비 경계를
  넘으면 mTLS 가 필요하다** (DEPLOYMENT.md 4절).
- 개수·리소스 조정 REST 는 **제거됐다.** 라이프사이클은 systemd 가 소유하고
  제어부는 관측만 한다 — 근거는 `control/internal/api` 패키지 주석.

남는 제약: `HOSTNAME` 을 인스턴스마다 다르게 주고 함대 설정의
`<machine>-dp<id>` 와 일치시켜야 한다. 어긋나면 전부 `unreachable` 로 보인다.

S 형태의 운용 창구는 `systemctl` · `journalctl` · `grpcurl` 과
제어부의 `/api/dataplanes` 다.

---

## 8. 개수·리소스 조정

```bash
sudo systemctl start  mir@1.target      # 추가
sudo systemctl stop   mir@1.target      # 제거
sudo systemctl restart mir-dataplane@0  # 코어/장치 변경 후 (env 파일 수정 뒤)
```

**무중단 변경은 S 형태에서도 불가능하다.** 컨테이너 spec 불변성이나 in-place resize 제약과
무관하게, `rte_eal_init` 이 lcore 집합을 프로세스 생존 기간 동안 고정하기 때문이다
(REQUIREMENTS 4-3-3 의 제약 3). 코어를 바꾸려면 프로세스를 다시 띄워야 하고,
진행 중인 시나리오는 중단된다.

---

## 9. 제거 / 롤백

```bash
sudo systemctl disable --now mir@0.target mir-dataplane@0 mir-agent@0
sudo rm /etc/systemd/system/mir-{dataplane,agent}@.service /etc/systemd/system/mir@.target
sudo systemctl daemon-reload

sudo rm -rf /etc/mir /run/mir
sudo rm /usr/local/bin/mir-dataplane /usr/local/bin/mir-agent

# NIC 을 커널 드라이버로 되돌린다
sudo driverctl unset-override 0000:3b:00.0

# 커널 파라미터 원복이 필요하면 /etc/default/grub 에서 hugepages/isolcpus 를 지우고
# sudo update-grub && sudo reboot
```

DPDK 자체를 지우려면 빌드 디렉터리에서 `sudo ninja -C build uninstall` 한다.

---

## 10. 트러블슈팅 (S 형태 전용)

컨테이너 관련 증상은 [DEPLOYMENT.md](DEPLOYMENT.md) 7절에 있고, 여기에는 단독 실행에서만
나타나는 것만 적는다.

| 증상 | 원인 | 조치 |
|---|---|---|
| `error while loading shared libraries: libdpdk...` | `ldconfig` 누락 | `sudo ldconfig`. `/usr/local/lib/x86_64-linux-gnu` 가 `ld.so.conf.d` 에 있는지 확인 |
| `Cannot set up DMA remapping` / mlock 실패 | memlock 한도 | 유닛에 `LimitMEMLOCK=infinity` 가 있는지. `systemctl show -p LimitMEMLOCK mir-dataplane@0` |
| `Permission denied` on `/dev/vfio/N` | root 가 아니거나 그룹 권한 없음 | 4-6 절. 당분간 root 로 실행 |
| 두 번째 인스턴스가 EAL 초기화 실패 | `--file-prefix` 충돌 | `/etc/mir/dp-<N>.env` 의 `HOSTNAME` 이 인스턴스마다 다른지 (6절) |
| `lcores` 가 의도와 다름 | taskset 이 안 먹었거나 `MIR_LCORES` 오타 | `systemctl cat mir-dataplane@0` 로 ExecStart 확인. `taskset -cp <pid>` 로 대조 |
| 지터가 크고 pps 가 흔들림 | 데이터플레인 코어가 `isolcpus` 밖 | `cat /proc/cmdline` 의 `isolcpus` 와 `MIR_LCORES` 를 대조 |
| 사이드카가 소켓을 못 연다 | 경로 불일치 또는 UID 차이 | 두 유닛이 같은 `EnvironmentFile` 을 쓰는지. C 쪽이 소켓을 0666 으로 만든다 |
| `go build` 가 버전 오류 | Go 1.26 미만 | `go version` 확인 후 go.dev 타르볼로 교체 |
| 인식된 포트가 0개 | BDF 오타 또는 vfio 미바인딩 | `MIR_DEVICE_SPEC` 값과 `20-bind-vfio.sh` 출력 대조 |

---

## 관련 문서

- [REQUIREMENTS.md](REQUIREMENTS.md) — 4-3-5 절이 이 형태의 설계 근거
- [DEPLOYMENT.md](DEPLOYMENT.md) — Compose(D) 형태 배포. hello packet 검증 절차(6절 6단계) 포함
- [deploy/host/10-kernel-cmdline.md](../deploy/host/10-kernel-cmdline.md) — 호스트 커널 설정 (공통)
