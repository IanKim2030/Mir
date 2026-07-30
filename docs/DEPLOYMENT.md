# 배포 가이드 — 베어메탈 단일 노드 k8s

Mir 데이터플레인(C/DPDK)과 제어부(Go)를 베어메탈 한 대의 단일 노드 k8s 위에
올리는 전체 절차.

> **현재 범위**: Phase 0. 컨테이너 안에서 `rte_eal_init` 이 성공하고 NIC 포트가
> 인식되는 지점까지가 완료 기준이다. TX/RX 송신 엔진은 Phase 1 이후다.

---

## 0. 계층 분리 — 무엇을 누가 하는가

DPDK 를 컨테이너화하는 이유는 성능이 아니라 **재현성**이다. 그런데 hugepage 크기,
IOMMU, vfio 바인딩, CPU 격리는 전부 호스트 커널에 묶여 있어 컨테이너가 숨겨주지
못한다. 이 경계를 헷갈리면 디버깅이 매우 괴로워진다.

| 계층 | 담당 | 컨테이너가 못 하는 일 |
|---|---|---|
| **호스트 (수동/스크립트)** | BIOS VT-d, 커널 cmdline(iommu·hugepages·isolcpus), vfio-pci 바인딩, hugetlbfs | — |
| **k8s 노드 설정** | kubelet CPU Manager `static`, Topology Manager `single-numa-node`, reserved CPUs | 호스트 커널 설정 변경 |
| **k8s 워크로드** | 파드 스케줄링, PF 할당, hugepage 할당량, 코어 배타 점유 | IOMMU·hugepage **생성** |

### 용어 정리 — "k8s docker"

Docker Engine 은 k8s 1.24 부터 CRI 런타임이 아니다. **이미지는 Docker 로 빌드하고
(OCI 표준), 런타임은 k3s 내장 containerd** 가 실행한다. 개발자 로컬에서
`docker run` 으로 같은 이미지를 띄우는 것도 그대로 가능하다 (§7 참조).

---

## 1. 호스트 준비

```bash
cd deploy/host
chmod +x *.sh      # Windows 에서 체크아웃했다면 실행 비트가 없을 수 있다
```

### 1-1. 커널 파라미터

[deploy/host/10-kernel-cmdline.md](../deploy/host/10-kernel-cmdline.md) 를 따라
BIOS 와 GRUB 을 설정하고 **재부팅**한다. 요약:

```
intel_iommu=on iommu=pt
default_hugepagesz=1G hugepagesz=1G hugepages=16
isolcpus=managed_irq,domain,2-31 nohz_full=2-31 rcu_nocbs=2-31
```

`isolcpus` / `nohz_full` / `rcu_nocbs` 는 **반드시 같은 집합**이고,
그 여집합이 다음 단계의 `reserved-cpus` 가 된다.

### 1-2. NIC 을 vfio-pci 로 바인딩

```bash
# 후보 목록 확인 (BDF, 드라이버, netdev, NUMA 노드)
sudo ./20-bind-vfio.sh

# 넘길 BDF 를 지정
sudo ./20-bind-vfio.sh 0000:3b:00.0 0000:3b:00.1
```

관리용 인터페이스(기본 경로를 가진 NIC)는 자동으로 보호된다 — 실수로 넘기면
장비 접속이 끊기기 때문이다. 정말 필요하면 `--force`.

`driverctl` 이 설치돼 있으면 재부팅 후에도 유지된다. 없으면 경고가 뜨고
바인딩이 휘발되므로 `sudo apt install driverctl` 후 다시 실행할 것.

### 1-3. k3s 설치

```bash
sudo ./30-install-k3s.sh                 # reserved-cpus 기본값 0,1
sudo RESERVED_CPUS=0-3 ./30-install-k3s.sh
```

`/etc/rancher/k3s/config.yaml` 에 다음이 들어간다:

| 설정 | 왜 |
|---|---|
| `cpu-manager-policy=static` | Guaranteed QoS + 정수 cpu 컨테이너에 **배타 코어** 할당. 없으면 busy-poll 이 의미를 잃는다 |
| `reserved-cpus=0,1` | static 정책의 필수 조건. `isolcpus` 의 여집합과 맞춘다 |
| `topology-manager-policy=single-numa-node` | CPU·hugepage·vfio 를 같은 NUMA 노드로 정렬. 100G 목표에서는 필수 |

### 1-4. 사전조건 점검

```bash
./40-verify-node.sh
```

FAIL 이 하나라도 있으면 다음 단계로 넘어가지 않는다. §6 트러블슈팅 참조.

---

## 2. 이미지 빌드

빌드 컨텍스트는 **저장소 루트**다 (세 이미지가 `proto/` 를 공유한다).

```bash
cd <저장소 루트>

docker build -f deploy/docker/Dockerfile.dataplane -t mir/dataplane:dev .
docker build -f deploy/docker/Dockerfile.agent     -t mir/agent:dev .
docker build -f deploy/docker/Dockerfile.control   -t mir/control:dev .
```

k3s 는 containerd 를 쓰므로 로컬 Docker 이미지가 자동으로 보이지 않는다.
가져다 넣어야 한다:

```bash
for img in dataplane agent control; do
  docker save mir/$img:dev | sudo k3s ctr images import -
done
```

### 빌드에서 주의할 점

- **DPDK 버전이 `--build-arg DPDK_VERSION` 으로 고정**된다. 배포판 패키지 버전은
  베이스 이미지 갱신에 따라 조용히 바뀌는데, 컨테이너화의 목적 자체가 재현성이라
  그 변동을 허용하지 않는다.
- **`-march=native` 는 금지다.** 빌드 머신과 실행 머신의 CPU 가 다르면 SIGILL 로
  죽는다. 기본값은 `x86-64-v3` 이며 Phase 7 에서 타깃이 확정되면 올린다.
- 데이터플레인 이미지에 **C++ 런타임이 들어가면 안 된다.** gRPC 는 Go 사이드카가
  담당하므로 libstdc++·abseil·BoringSSL 이 보인다면 설계가 새고 있다는 신호다.

---

## 3. 코드 생성 (proto 를 고쳤을 때만)

```bash
# Go — 생성물은 control/internal/pb/ 에 떨어지고 커밋한다
buf lint
buf generate

# C — 이미지 빌드 중 meson custom_target 이 자동 실행하므로 수동 실행은 불필요
```

`.proto` 하나가 두 hop 을 모두 정의한다. 사이드카가 대부분의 필드를 그대로
통과시키는 릴레이라, 스키마를 하나로 유지하면 두 hop 이 갈라질 여지가 없다.

---

## 4. 배포

**순서가 중요하다.** device plugin 이 먼저 떠서 `mir.io/dpdk_pf` 를 노출해야
데이터플레인 파드가 스케줄된다.

```bash
export KUBECONFIG=/etc/rancher/k3s/k3s.yaml

# ① device plugin 설정 — pciAddresses 를 실물에 맞게 먼저 수정할 것
vi deploy/k8s/overlays/baremetal/sriovdp-config.yaml
kubectl apply -f deploy/k8s/overlays/baremetal/sriovdp-config.yaml

# ② device plugin DaemonSet — 태그를 확인해서 적용
#    https://github.com/k8snetworkplumbingwg/sriov-network-device-plugin/releases
kubectl apply -f https://raw.githubusercontent.com/k8snetworkplumbingwg/sriov-network-device-plugin/<TAG>/deployments/sriovdp-daemonset.yaml

# ③ 리소스가 실제로 노출됐는지 — 여기서 0 이면 아래는 전부 Pending 된다
kubectl get node -o jsonpath='{.items[0].status.allocatable}' | tr ',' '\n' | grep dpdk_pf

# ④ Mir 배포
kubectl apply -k deploy/k8s/overlays/baremetal
```

자세한 내용은 [deploy/k8s/overlays/README.md](../deploy/k8s/overlays/README.md).

> `deploy/k8s/base` 를 단독으로 apply 하지 말 것. 리소스가 비어 있어 데이터플레인이
> BestEffort QoS 로 뜨고 배타 코어도 NIC 도 받지 못한다.

---

## 5. 검증

### 1단계 — 호스트 사전조건

```bash
./deploy/host/40-verify-node.sh
```

### 2단계 — k8s 노드

```bash
kubectl get node -o jsonpath='{.items[0].status.allocatable}' | tr ',' '\n'
# hugepages-1Gi 와 mir.io/dpdk_pf 가 보여야 한다
```

### 3단계 — 데이터플레인 컨테이너 (완료 기준)

```bash
kubectl -n mir logs deploy/mir-dataplane -c dataplane
#   rte_eal_init 성공, port N개 인식 로그가 나와야 한다

kubectl -n mir exec deploy/mir-dataplane -c dataplane -- \
    cat /sys/fs/cgroup/cpuset.cpus.effective     # 배타 코어 5개

kubectl -n mir exec deploy/mir-dataplane -c dataplane -- env | grep PCIDEVICE
kubectl -n mir describe pod -l app=mir-dataplane | grep -i qos    # Guaranteed
```

컨테이너 안에서 장치 상태를 직접 볼 수도 있다:

```bash
kubectl -n mir exec deploy/mir-dataplane -c dataplane -- \
    python3 /usr/local/bin/dpdk-devbind.py --status
```

### 4단계 — 코어 분리 (사이드카 설계의 핵심 검증)

```bash
kubectl -n mir exec deploy/mir-dataplane -c dataplane -- cat /sys/fs/cgroup/cpuset.cpus.effective
kubectl -n mir exec deploy/mir-dataplane -c agent     -- cat /sys/fs/cgroup/cpuset.cpus.effective
```

**두 cpuset 이 겹치면 안 된다.** 겹쳤다면 사이드카의 gRPC 스레드가 DPDK 의
busy-poll 코어를 선점할 수 있다는 뜻이고, 대개 원인은 `agent` 의 cpu 요청을
실수로 정수(`"1"`)로 준 것이다 — 분수(`500m`)여야 공유 풀에 남는다.

### 5단계 — 채널 연결

```bash
# ③ unix socket
kubectl -n mir exec deploy/mir-dataplane -c agent -- ls -l /var/run/mir/dp.sock

# ④ gRPC
kubectl -n mir port-forward deploy/mir-dataplane 9100:9100 &
grpcurl -plaintext localhost:9100 list
grpcurl -plaintext localhost:9100 mir.v1.DataPlane/Hello   # 포트 목록이 나와야 한다

# 제어부
kubectl -n mir port-forward svc/mir-control 8080:80 &
curl -s localhost:8080/healthz | jq
# {"ok":true,"version":"0.1.0","connected":1,"total":1}
```

### 6단계 — 개수·리소스 조정

```bash
curl -s localhost:8080/api/capacity | jq
curl -s localhost:8080/api/dataplanes | jq

# 개수 조정 — 무중단 (제어부·GUI 영향 없음)
curl -XPUT localhost:8080/api/dataplanes/scale -d '{"replicas":2}'
kubectl -n mir get pod -l app=mir-dataplane -w

# 상한 초과는 400 으로 거절되어야 한다 (조용히 Pending 으로 새면 안 된다)
curl -XPUT localhost:8080/api/dataplanes/scale -d '{"replicas":99}'

# 리소스 조정 — 데이터플레인만 롤링 재생성
curl -XPUT localhost:8080/api/dataplanes/resources -d '{"cpu":"7"}'
kubectl -n mir rollout status deploy/mir-dataplane
curl -s localhost:8080/healthz | jq    # 자동 재연결까지 확인
```

---

## 6. 운영 메모 — 개수·리소스 조정의 성질

**"무중단 변경"은 달성 불가능하다.** 세 겹의 제약이 겹쳐 있다.

1. Pod 의 `spec.containers` 는 불변 — 컨테이너 개수 변경 = Pod 재생성
2. in-place resize(k8s 1.33 beta)는 **static CPU manager + Guaranteed 조합에서
   `Infeasible`** 로 마킹된다. 우리는 배타 코어를 위해 그 조합이 필수다.
   `hugepages-*` 와 device plugin 확장 리소스는 애초에 resize 대상도 아니다
3. **DPDK 가 런타임 lcore 변경을 지원하지 않는다** — `rte_eal_init` 이 lcore 집합을
   프로세스 생존 기간 동안 고정하고 mempool 도 그때 확보한다

그래서 목표는 "무중단"이 아니라 **재시작 범위를 데이터플레인으로만 한정**하는
것이고, 제어부가 별도 파드에 있어야 그게 성립한다.

| 요구 | 실현 | 제어부·GUI 영향 |
|---|---|---|
| 개수 설정 | `Deployment/scale` patch | 없음 |
| CPU/메모리 설정 | pod template patch → 데이터플레인만 롤링 재생성 | 없음 |
| 특정 데이터플레인 재시작 | 해당 Pod delete | 없음 |

재시작 동안 텔레메트리 스트림은 끊기지만, 제어부가 EndpointSlice 를 조회하고
있으므로 새 파드가 뜨면 **자동으로 재연결된다**. 별도 조작이 필요 없다.

---

## 7. 로컬 개발 — k8s 없이 컨테이너 경로만 검증

k8s 계층의 문제와 DPDK 계층의 문제를 분리해서 디버깅할 수 있다.

```bash
# 데이터플레인 — device plugin 없이 BDF 를 직접 지정
docker run --rm -it \
  --cpuset-cpus 2-6 \
  -v /dev/hugepages:/dev/hugepages \
  -v /dev/vfio:/dev/vfio \
  -v mir-ipc:/var/run/mir \
  --cap-add IPC_LOCK --cap-add SYS_NICE \
  -e MIR_DEVICE_SPEC=pci:0000:3b:00.0 \
  mir/dataplane:dev

# 사이드카 — 같은 볼륨에 붙인다
docker run --rm -it \
  -v mir-ipc:/var/run/mir \
  -p 9100:9100 \
  mir/agent:dev

grpcurl -plaintext localhost:9100 mir.v1.DataPlane/Hello
```

`MIR_DEVICE_SPEC` 는 device plugin 이 주입하는 `PCIDEVICE_*` 를 대신하는
수동 지정 통로다.

---

## 8. 트러블슈팅

| 증상 | 원인 | 조치 |
|---|---|---|
| `VFIO group not viable` | 같은 IOMMU 그룹의 다른 장치가 커널 드라이버 사용 중 | `20-bind-vfio.sh` 가 경고를 출력한다. 동거 장치도 vfio-pci 로 넘기거나 unbind |
| `Cannot get hugepage information` | hugepage 미예약 또는 hugetlbfs 미마운트 | `10-kernel-cmdline.md` 적용 후 재부팅. `grep Huge /proc/meminfo` 확인 |
| `EAL: No available 1048576 kB hugepages` | 다른 파드가 이미 소진 | `kubectl -n mir get pod` 로 중복 실행 확인. `hugepages=` 값을 늘린다 |
| 데이터플레인 파드가 계속 `Pending` | `mir.io/dpdk_pf` 부족 | `kubectl describe pod` 의 이벤트 확인. device plugin 이 떴는지, `replicas` 가 PF 개수를 넘지 않는지 |
| 리소스 변경 후 새 파드가 `Pending` 에서 멈춤 | `maxSurge` 가 0 이 아님 | PF 가 배타 자원이라 먼저 죽여야 반납된다. `strategy.rollingUpdate.maxSurge: 0` 확인 |
| lcore 개수가 예상과 다름 | cpuset 이 반영되지 않음 | 파드가 Guaranteed QoS 인지, `cpu` 가 **정수**인지 확인. `cpu_manager_state` 도 점검 |
| kubelet 이 기동 실패 | `cpu-manager-policy` 를 나중에 변경 | `sudo rm /var/lib/kubelet/cpu_manager_state && sudo systemctl restart k3s` |
| 포트는 probe 되는데 링크 다운 | SFP 모듈 비호환 | Intel 계열은 펌웨어가 비(非)Intel 광모듈을 거부할 수 있다. 케이블·모듈 교체로 확인 |
| `dp.sock` 이 없다 | C 가 아직 EAL 초기화 중이거나 `ipc` 볼륨 마운트 누락 | 사이드카는 이 상태를 정상으로 보고 재시도한다. 수 초 뒤에도 없으면 dataplane 컨테이너 로그 확인 |
| 사이드카가 `NOT_SERVING` 에서 안 벗어남 | C 데이터플레인이 뜨지 못함 | `kubectl logs -c dataplane` 확인. readiness 가 의도적으로 이 상태를 반영한다 |
| 인식된 포트가 0개 | device plugin 이 PCI 주소를 주입하지 않음 | `env | grep PCIDEVICE` 확인. `sriovdp-config.yaml` 의 `pciAddresses` 점검 |
| 제어부가 403/Forbidden | RBAC 누락 | `control-rbac.yaml` 이 적용됐는지, ServiceAccount 가 파드에 붙었는지 확인 |

### IOMMU 를 못 켠 경우

BIOS 에 VT-d 가 없으면 no-IOMMU 폴백을 쓴다. **대가를 인지할 것**: 컨테이너가
임의 물리 메모리에 DMA 할 수 있게 되어 파드 격리 경계가 사실상 사라지고,
데이터플레인 컨테이너를 `privileged: true` 로 돌려야 한다.

`deploy/k8s/overlays/baremetal/dataplane-resources.yaml` 의 `securityContext` 를
그에 맞게 수정하고, **이 사실을 여기 기록해 두어야** 이후 디버깅이 쉽다.

---

## 관련 문서

- [REQUIREMENTS.md](REQUIREMENTS.md) — 요구사항·설계의 단일 출처
- [deploy/k8s/overlays/README.md](../deploy/k8s/overlays/README.md) — 환경별 오버레이,
  클라우드 이식 시 무엇을 바꿔야 하는지
- [deploy/host/10-kernel-cmdline.md](../deploy/host/10-kernel-cmdline.md) — 커널 파라미터 상세
