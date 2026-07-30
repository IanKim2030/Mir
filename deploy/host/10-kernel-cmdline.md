# 10 — 커널 파라미터 (수동 적용 + 재부팅)

컨테이너가 대신해 줄 수 없는 유일한 계층. **여기서 실패하면 뒤 단계는 전부 실패한다.**

## 0. BIOS 선행 설정

| 항목 | 값 | 이유 |
|---|---|---|
| Intel VT-d (IOMMU) | **Enable** | vfio-pci 정상 모드. 없으면 no-IOMMU 폴백(3절) |
| Hyper-Threading | 권장 Disable | 형제 스레드가 물리 코어를 공유해 pps 지터 발생 |
| C-State / C1E | Disable | busy-poll 중 코어가 저전력 상태로 내려가면 지연 튐 |
| Turbo Boost | 권장 Disable | 측정 재현성 확보 (최대 성능보다 일관성 우선) |
| PCIe Max Payload | 최대값 | DMA 효율 |

## 1. GRUB 파라미터

`/etc/default/grub` 의 `GRUB_CMDLINE_LINUX_DEFAULT` 에 추가한다.

```
intel_iommu=on iommu=pt
default_hugepagesz=1G hugepagesz=1G hugepages=16
isolcpus=managed_irq,domain,2-31 nohz_full=2-31 rcu_nocbs=2-31
```

```bash
sudo vi /etc/default/grub
sudo update-grub
sudo reboot
```

### 각 파라미터의 의미

| 파라미터 | 역할 | 값 결정 기준 |
|---|---|---|
| `intel_iommu=on` | IOMMU 활성화 → vfio-pci 정상 모드 → **비특권 파드** 가능 | 고정 |
| `iommu=pt` | passthrough 모드. DMA 리매핑 오버헤드 제거 | 고정 |
| `default_hugepagesz=1G hugepagesz=1G` | 1GB 페이지를 기본으로. **1GB는 부팅 시에만 예약 가능** | 고정 |
| `hugepages=16` | 1GB 페이지 개수 = 총 16GB | `데이터플레인 파드 수 × 파드당 hugepage` 이상 |
| `isolcpus=...` | 커널 스케줄러가 해당 코어에 태스크를 올리지 않음 | 아래 "코어 배분" |
| `nohz_full=...` | 해당 코어의 주기적 타이머 틱 제거 | `isolcpus` 와 동일 집합 |
| `rcu_nocbs=...` | RCU 콜백을 다른 코어로 오프로드 | `nohz_full` 과 동일 집합 |

### 코어 배분 규칙

`isolcpus` / `nohz_full` / `rcu_nocbs` 는 **반드시 같은 집합**이어야 하고,
그 여집합이 다음 단계의 kubelet `reserved-cpus` 가 된다.

```
전체 코어 0-31
  ├─ 0,1        → OS / kubelet / 시스템 데몬 / 사이드카   → reserved-cpus=0,1
  └─ 2-31       → isolcpus, DPDK worker 전용             → CPU Manager가 파드에 배타 할당
```

> 코어 수는 서버 실물 확인 후 확정한다 (REQUIREMENTS Open Issue 2).
> NUMA 2소켓이면 **NIC이 붙은 소켓의 코어만** 데이터플레인에 할당되도록
> Topology Manager `single-numa-node` 정책이 처리한다 — `30-install-k3s.sh` 참조.

### 선택: 지연 최적화 (Phase 7에서 재검토)

```
intel_idle.max_cstate=0 processor.max_cstate=1 idle=poll
```

`idle=poll` 은 전력 소비가 크게 늘어난다. 100G 튜닝 단계에서 효과를 측정한 뒤
채택 여부를 결정한다.

## 2. 적용 확인

```bash
cat /proc/cmdline                              # 파라미터가 실제로 들어갔는지
dmesg | grep -i -e DMAR -e IOMMU | head        # "DMAR: IOMMU enabled"
grep Huge /proc/meminfo                        # HugePages_Total = 16
```

`40-verify-node.sh` 가 위 항목을 일괄 점검한다.

## 3. IOMMU를 켤 수 없는 경우 (폴백)

BIOS에 VT-d 옵션이 없거나 활성화가 불가능하면 vfio no-IOMMU 모드로 폴백한다.

```bash
echo 1 | sudo tee /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
```

**대가를 명확히 인지할 것:**

- 컨테이너가 **임의의 물리 메모리에 DMA** 할 수 있다 → 파드 격리 경계가 사실상 사라진다
- 데이터플레인 컨테이너를 `privileged: true` 로 돌려야 한다
- `deploy/k8s/overlays/baremetal/kustomization.yaml` 의 securityContext 패치를
  그에 맞게 바꿔야 한다

전용 테스트 장비가 아니라면 권장하지 않는다. 이 경로를 택했다면
[../../docs/DEPLOYMENT.md](../../docs/DEPLOYMENT.md) 에 그 사실을 기록해 두어야
이후 디버깅이 쉽다.
