# 환경별 오버레이

`base` 는 환경 무관 구조만 담는다. 환경마다 달라지는 것은 **네 가지뿐**이며,
이 경계만 유지하면 `base` 를 손대지 않고 환경을 늘릴 수 있다.

| 오버레이가 덮어쓰는 것 | 왜 환경마다 다른가 |
|---|---|
| **hugepage 크기** | 1Gi 는 커널 cmdline 으로만 예약 가능. managed 노드풀은 cmdline 접근이 막혀 2Mi 가 현실적 |
| **securityContext** | IOMMU 유무. 없으면 no-IOMMU 폴백 → `privileged: true` 강제 |
| **device plugin 리소스명** | NIC 장치 모델이 환경마다 다름 |
| **replicas** | 사용 가능한 NIC PF 개수 |

---

## baremetal (구현됨)

**순서가 중요하다.** device plugin 이 먼저 떠서 `mir.io/dpdk_pf` 를 노출해야
데이터플레인 파드가 스케줄된다.

```bash
# ① SR-IOV Device Plugin — kustomize 밖에서 적용한다.
#    ConfigMap 이 kube-system 에 있어야 하는데, 오버레이의 `namespace: mir`
#    변환이 그것까지 끌고 오기 때문에 kustomize 에 넣지 않았다.
#
#    ⚠️ pciAddresses 를 실물에 맞게 먼저 수정할 것.
#       (deploy/host/20-bind-vfio.sh 를 인자 없이 실행하면 후보가 나온다)
kubectl apply -f deploy/k8s/overlays/baremetal/sriovdp-config.yaml

#    DaemonSet 은 upstream 을 쓰되 **태그를 확인해서** 적용한다. 검증하지 않은
#    버전을 여기 고정해 두면 나중에 조용히 어긋난다.
#    https://github.com/k8snetworkplumbingwg/sriov-network-device-plugin/releases
kubectl apply -f https://raw.githubusercontent.com/k8snetworkplumbingwg/sriov-network-device-plugin/<TAG>/deployments/sriovdp-daemonset.yaml

# ② 리소스가 실제로 노출됐는지 확인 — 여기서 0 이면 아래는 전부 Pending 된다
kubectl get node -o jsonpath='{.items[0].status.allocatable}' | tr ',' '\n' | grep dpdk_pf

# ③ Mir 배포
kubectl apply -k deploy/k8s/overlays/baremetal
```

ConfigMap 을 고친 뒤에는 device plugin 을 재시작해야 반영된다:

```bash
kubectl -n kube-system rollout restart ds/kube-sriov-device-plugin-amd64
```

---

## eks / aks / gke (Phase 8 — 미구현)

이번 작업 범위 밖이지만, 무엇을 덮어써야 하는지는 조사가 끝나 있다.

### 공통

- **hugepage 를 2Mi 로**: `emptyDir.medium: HugePages-2Mi`, `hugepages-2Mi: 4Gi`
- **`*.metal` 인스턴스 권장**: IOMMU 정상 동작 + 비특권 파드 + 1G hugepage +
  실질적 코어 격리가 모두 성립해 베어메탈 구성이 거의 그대로 적용된다.
  일반 인스턴스는 기능 테스트용으로 격하한다.

### AWS (ENA) / GCP (gVNIC)

동일한 `vfio-pci` 모델이라 device plugin 설정을 **vendor ID 만 바꿔 재사용**할 수 있다.

- AWS vendor `1d0f`, GCP vendor `1ae0`
- ⚠️ IOMMU 를 지원하는 인스턴스는 `*.metal` 계열뿐이다. 그 외에는
  `enable_unsafe_noiommu_mode=1` + **`privileged: true`** 가 강제된다
- ENA 는 write-combining 활성화 패치가 별도로 필요하다
- **EKS VPC CNI 가 ENI 를 파드에 할당**하므로, DPDK 가 점유할 ENI 는 CNI 관리
  대상에서 제외해야 한다 — Phase 8 착수 시 첫 확인 항목

### Azure (MANA / mlx5)

**모델이 아예 다르다.** 오버레이 패치만으로는 안 되고 코드도 손봐야 한다.

- netvsc 합성 인터페이스 + SR-IOV VF 쌍 구조. DPDK 는 `uio_hv_generic` 을 쓰거나
  PMD 가 bifurcated 드라이버로 커널 netdev 를 그대로 두고 접근한다
- `/dev/vfio/N` 을 파드에 넘기는 모델이 아니라 **netdev 를 파드 netns 에 넣어야**
  하므로, 베어메탈에서 불필요했던 **Multus(또는 `hostNetwork: true`)가 필요**해진다
- MANA PMD 는 PCI BDF 가 아니라 **MAC 주소**로 바인딩 대상을 정한다
  → `dataplane/src/eal_args.h` 의 `DEVICE_SPEC_MAC_ADDR` 경로를 채우면 된다.
    그 자리를 미리 잡아 둔 이유가 이것이다
