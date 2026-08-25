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

### 검증기 `10.10.40.121` (Xeon Gold 5418N, 24C/1소켓) — 실측 확정값

```
intel_iommu=on iommu=pt
default_hugepagesz=1G hugepagesz=1G hugepages=16
isolcpus=managed_irq,domain,2-23 nohz_full=2-23 rcu_nocbs=2-23
```

> **BIOS 에서 HT 를 먼저 끌 것.** 아래 "현재 설정이 왜 무효인가" 참조.
> HT 를 끄면 논리 CPU 가 0-23(=물리코어 24개)이 되어 위 값이 그대로 맞는다.

```bash
sudo vi /etc/default/grub
sudo update-grub
sudo reboot
```

### 각 파라미터의 의미

| 파라미터 | 역할 | 값 결정 기준 |
|---|---|---|
| `intel_iommu=on` | IOMMU 활성화 → vfio-pci 정상 모드 → **비특권 컨테이너** 가능 | 고정 |
| `iommu=pt` | passthrough 모드. DMA 리매핑 오버헤드 제거 | 고정 |
| `default_hugepagesz=1G hugepagesz=1G` | 1GB 페이지를 기본으로. **1GB는 부팅 시에만 예약 가능** | 고정 |
| `hugepages=16` | 1GB 페이지 개수 = 총 16GB | `인스턴스 수 × MIR_MEM_MB` 이상 |
| `isolcpus=...` | 커널 스케줄러가 해당 코어에 태스크를 올리지 않음 | 아래 "코어 배분" |
| `nohz_full=...` | 해당 코어의 주기적 타이머 틱 제거 | `isolcpus` 와 동일 집합 |
| `rcu_nocbs=...` | RCU 콜백을 다른 코어로 오프로드 | `nohz_full` 과 동일 집합 |

### 코어 배분 규칙

`isolcpus` / `nohz_full` / `rcu_nocbs` 는 **반드시 같은 집합**이어야 한다.

**`isolcpus` 가 코어 격리의 유일한 출처다.** 오케스트레이터에게 "코어 5개
달라"고 요청하고 받아쓰는 방식이 아니라, `deploy/compose/.env` 의
`MIR_DP*_CPUSET` 에 여기서 정한 코어를 **그대로 적어 넣는다**. 두 값이
어긋나면 `40-verify-node.sh` 가 FAIL 로 잡는다.

```
검증기 전체 코어 0-23 (HT off 기준)
  ├─ 0,1        → OS / 시스템 데몬 / 사이드카 / 제어부
  └─ 2-23       → isolcpus, DPDK worker 전용 (22코어)
       ├─ 2-6    → MIR_DP0_CPUSET
       ├─ 7-11   → MIR_DP1_CPUSET
       ├─ 12-16  → MIR_DP2_CPUSET
       └─ 17-21  → MIR_DP3_CPUSET   (22,23 은 여유)
```

인스턴스는 PF 하나당 하나이므로 **4개**(X710 4포트), 인스턴스당 5코어
(main 1 + worker 4) → 20코어. `isolcpus` 22코어 안에 들어간다.

> NUMA 2소켓이면 **NIC이 붙은 소켓의 코어만** 데이터플레인에 할당되도록
> 데이터플레인이 `MIR_MEM_MB` 와 `/sys` 의 NUMA 토폴로지를 읽어 자기 코어가 있는
> 소켓에만 hugepage 를 배정한다 — `dataplane/src/eal_args.c` 의 `resolve_memory`.
> 검증기는 1소켓이라 해당 없음. 목표기(2소켓)에서는 필수다.

### ⚠️ 왜 HT 를 끄고 `2-23` 으로 잡는가 (해소된 사례)

아래는 **2026-08-01 시점의 잘못된 설정**이다. 2026-08-25 에 HT off +
`isolcpus=2-23` 로 바로잡았고, 같은 함정을 반복하지 않기 위해 근거를 남겨 둔다.

```
당시 cmdline: isolcpus=0-23 nohz_full=0-23 rcu_nocbs=0-23
HT 매핑:      core0→(0,24)  core1→(1,25)  …  core23→(23,47)
```

`0-23` 은 물리코어 24개 **전부의 첫 번째 스레드**다. OS 가 쓰는 `24-47` 은 바로 그
물리코어들의 **HT 형제**이므로, 24개 물리코어를 DPDK 와 OS 가 통째로 공유한다.
형제 스레드는 실행 유닛과 L1/L2 를 공유하니 busy-poll 중인 코어 옆에서 OS 작업이
돌면 pps 지터가 그대로 발생한다 — **격리가 전혀 성립하지 않는다.**

확인 방법:

```bash
lscpu -p=CPU,CORE | grep -v '^#' | sort -t, -k2 -n | head -4
#   0,0 / 24,0 / 1,1 / 25,1  ← 같은 CORE 에 두 CPU 가 묶여 있으면 HT on
```

해결은 BIOS 에서 **Hyper-Threading Disable** (0절 표의 권장 사항과 동일). 끄면
논리 CPU 가 24개로 줄어 `2-23` 이 물리코어 22개와 1:1 대응한다. HT 를 유지해야 한다면
`isolcpus=2-23,26-47` 처럼 **물리코어 단위로 형제를 함께** 격리해야 하지만,
DPDK worker 를 형제 스레드에 올려도 성능은 늘지 않으므로 권장하지 않는다.

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

- 컨테이너가 **임의의 물리 메모리에 DMA** 할 수 있다 → 컨테이너 격리 경계가 사실상 사라진다
- `deploy/compose/docker-compose.dataplane.yml` 의 `x-dataplane` 앵커에
  `privileged: true` 를 추가해야 한다

전용 테스트 장비가 아니라면 권장하지 않는다. 이 경로를 택했다면
[../../docs/DEPLOYMENT.md](../../docs/DEPLOYMENT.md) 에 그 사실을 기록해 두어야
이후 디버깅이 쉽다.
