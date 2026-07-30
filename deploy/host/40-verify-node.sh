#!/usr/bin/env bash
#
# 40-verify-node.sh — 데이터플레인 파드를 띄우기 전 노드 사전조건을 일괄 점검한다.
#
#   ./40-verify-node.sh
#
# 종료 코드: 0 = FAIL 없음, 1 = FAIL 존재
#
set -uo pipefail

PASS=0; FAIL=0; WARN=0

ok()  { printf "  \033[32m[ OK ]\033[0m %-40s %s\n" "$1" "${2:-}"; PASS=$((PASS+1)); }
bad() { printf "  \033[31m[FAIL]\033[0m %-40s %s\n" "$1" "${2:-}"; FAIL=$((FAIL+1)); }
meh() { printf "  \033[33m[WARN]\033[0m %-40s %s\n" "$1" "${2:-}"; WARN=$((WARN+1)); }
sec() { printf "\n\033[1m%s\033[0m\n" "$1"; }

export KUBECONFIG="${KUBECONFIG:-/etc/rancher/k3s/k3s.yaml}"
KUBECTL=""
if command -v kubectl >/dev/null 2>&1; then KUBECTL="kubectl"
elif command -v k3s >/dev/null 2>&1;    then KUBECTL="k3s kubectl"; fi

# ─────────────────────────────────────────────────────────────
sec "1. 커널 / IOMMU"

if dmesg 2>/dev/null | grep -qi "DMAR: IOMMU enabled"; then
    ok "IOMMU 활성" "비특권 파드 가능"
elif [[ -n "$(ls -A /sys/kernel/iommu_groups 2>/dev/null)" ]]; then
    ok "IOMMU 그룹 존재" "$(ls /sys/kernel/iommu_groups | wc -l)개"
else
    noiommu=$(cat /sys/module/vfio/parameters/enable_unsafe_noiommu_mode 2>/dev/null || echo N)
    if [[ "$noiommu" == "Y" ]]; then
        meh "IOMMU 비활성 (no-IOMMU 폴백 중)" "privileged 필요"
    else
        bad "IOMMU 비활성" "10-kernel-cmdline.md 1절"
    fi
fi

grep -q "iommu=pt" /proc/cmdline \
    && ok "iommu=pt" "DMA 리매핑 오버헤드 제거됨" \
    || meh "iommu=pt 없음" "성능 손실 가능"

if grep -qE "isolcpus=" /proc/cmdline; then
    ok "isolcpus" "$(grep -oE 'isolcpus=[^ ]*' /proc/cmdline)"
else
    meh "isolcpus 없음" "커널이 격리 코어에 태스크를 올릴 수 있음"
fi

# ─────────────────────────────────────────────────────────────
sec "2. Hugepage"

hp_total=$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo)
hp_free=$(awk '/^HugePages_Free:/  {print $2}' /proc/meminfo)
hp_size=$(awk '/^Hugepagesize:/    {print $2}' /proc/meminfo)

[[ "${hp_total:-0}" -gt 0 ]] \
    && ok  "HugePages_Total" "${hp_total}개 × ${hp_size}kB" \
    || bad "HugePages_Total = 0" "10-kernel-cmdline.md 적용 후 재부팅"

if [[ "${hp_total:-0}" -gt 0 && "${hp_free:-0}" -eq 0 ]]; then
    meh "HugePages_Free = 0" "이미 전부 할당됨"
elif [[ "${hp_free:-0}" -gt 0 ]]; then
    ok "HugePages_Free" "${hp_free}개"
fi

[[ "${hp_size:-0}" -eq 1048576 ]] \
    && ok  "Hugepagesize = 1GB" "권장 설정" \
    || meh "Hugepagesize = ${hp_size}kB" "1GB 권장 (default_hugepagesz=1G)"

mountpoint -q /dev/hugepages 2>/dev/null \
    && ok  "hugetlbfs 마운트" "/dev/hugepages" \
    || bad "hugetlbfs 미마운트" "/dev/hugepages"

# ─────────────────────────────────────────────────────────────
sec "3. vfio-pci 바인딩"

lsmod 2>/dev/null | grep -q '^vfio_pci' \
    && ok  "vfio-pci 모듈 로드됨" \
    || bad "vfio-pci 모듈 없음" "modprobe vfio-pci"

groups=$(ls /dev/vfio/ 2>/dev/null | grep -v '^vfio$' || true)
[[ -n "$groups" ]] \
    && ok  "vfio 그룹" "$(echo "$groups" | tr '\n' ' ')" \
    || bad "vfio 그룹 없음" "20-bind-vfio.sh 실행 필요"

bound=0
for dev in /sys/bus/pci/devices/*; do
    [[ -e "$dev/driver" ]] || continue
    [[ "$(basename "$(readlink -f "$dev/driver")")" == "vfio-pci" ]] || continue
    numa=$(cat "$dev/numa_node" 2>/dev/null || echo "?")
    ok "  $(basename "$dev")" "vfio-pci, NUMA node$numa"
    bound=$((bound+1))
done
[[ $bound -eq 0 ]] && bad "vfio-pci 바인딩된 장치 없음" "20-bind-vfio.sh 실행 필요"

# ─────────────────────────────────────────────────────────────
sec "4. Kubernetes 노드"

if [[ -z "$KUBECTL" ]]; then
    bad "kubectl 없음" "30-install-k3s.sh 실행 필요"
elif ! $KUBECTL get node >/dev/null 2>&1; then
    bad "k8s API 응답 없음" "journalctl -u k3s -n 50"
else
    node=$($KUBECTL get node -o jsonpath='{.items[0].metadata.name}')
    status=$($KUBECTL get node -o jsonpath='{.items[0].status.conditions[?(@.type=="Ready")].status}')
    [[ "$status" == "True" ]] \
        && ok  "노드 Ready" "$node" \
        || bad "노드 NotReady" "$node"

    alloc=$($KUBECTL get node -o jsonpath='{.items[0].status.allocatable}' 2>/dev/null)

    if echo "$alloc" | grep -q "hugepages-1Gi"; then
        ok "allocatable hugepages-1Gi" "$(echo "$alloc" | grep -oE '"hugepages-1Gi":"[^"]*"' | cut -d'"' -f4)"
    elif echo "$alloc" | grep -q "hugepages-2Mi"; then
        meh "allocatable hugepages-2Mi" "$(echo "$alloc" | grep -oE '"hugepages-2Mi":"[^"]*"' | cut -d'"' -f4) (1Gi 권장)"
    else
        bad "allocatable 에 hugepages 없음" "kubelet 이 hugepage 를 인식하지 못함"
    fi

    if echo "$alloc" | grep -q "mir.io/dpdk_pf"; then
        ok "allocatable mir.io/dpdk_pf" "$(echo "$alloc" | grep -oE '"mir.io/dpdk_pf":"[^"]*"' | cut -d'"' -f4)"
    else
        meh "mir.io/dpdk_pf 없음" "SR-IOV device plugin 미배포 (deploy/k8s 단계)"
    fi
fi

# ─────────────────────────────────────────────────────────────
sec "5. kubelet 정책"

cfg=/etc/rancher/k3s/config.yaml
if [[ -f "$cfg" ]]; then
    grep -q "cpu-manager-policy=static" "$cfg" \
        && ok  "cpu-manager-policy=static" \
        || bad "cpu-manager-policy 미설정" "배타 코어 할당 불가"
    grep -q "topology-manager-policy=single-numa-node" "$cfg" \
        && ok  "topology-manager-policy=single-numa-node" \
        || meh "topology-manager-policy 미설정" "NUMA 정렬 보장 안 됨"
    grep -q "reserved-cpus=" "$cfg" \
        && ok  "reserved-cpus" "$(grep -oE 'reserved-cpus=[^ ]*' "$cfg")" \
        || bad "reserved-cpus 미설정" "static 정책에 필수"
else
    meh "$cfg 없음" "30-install-k3s.sh 로 생성됨"
fi

# ─────────────────────────────────────────────────────────────
printf "\n\033[1m결과: %d PASS, %d WARN, %d FAIL\033[0m\n" "$PASS" "$WARN" "$FAIL"
if [[ $FAIL -gt 0 ]]; then
    echo "FAIL 항목을 해결한 뒤 다시 실행할 것. docs/DEPLOYMENT.md 트러블슈팅 절 참조."
    exit 1
fi
echo "노드 사전조건 충족. 다음: kubectl apply -k deploy/k8s/overlays/baremetal"
exit 0
