#!/usr/bin/env bash
#
# 30-install-k3s.sh — DPDK 워크로드용으로 튜닝된 단일 노드 k3s 를 설치한다.
#
#   sudo ./30-install-k3s.sh
#   sudo RESERVED_CPUS=0-3 ./30-install-k3s.sh
#
# 이미 k3s 가 설치되어 있으면 설정만 갱신하고 재시작한다.
#
set -euo pipefail

RESERVED_CPUS="${RESERVED_CPUS:-0,1}"
K3S_CONFIG=/etc/rancher/k3s/config.yaml
CPU_MANAGER_STATE=/var/lib/kubelet/cpu_manager_state

die()  { echo "ERROR: $*" >&2; exit 1; }
warn() { echo "WARN:  $*" >&2; }
info() { echo "  $*"; }

[[ $EUID -eq 0 ]] || die "root 권한이 필요하다 (sudo)"

echo "== 사전 점검 =="

# hugepage 가 없으면 데이터플레인 파드가 스케줄조차 되지 않는다.
hp_total=$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo)
if [[ "${hp_total:-0}" -eq 0 ]]; then
    warn "HugePages_Total = 0 — 10-kernel-cmdline.md 를 먼저 적용하고 재부팅할 것"
    warn "  (지금 진행해도 설치는 되지만 데이터플레인 파드가 Pending 에 머문다)"
else
    hp_size=$(awk '/^Hugepagesize:/ {print $2" "$3}' /proc/meminfo)
    info "hugepage: ${hp_total}개 × ${hp_size}"
fi

if ls /dev/vfio/ 2>/dev/null | grep -qv '^vfio$'; then
    info "vfio 그룹: $(ls /dev/vfio/ | grep -v '^vfio$' | tr '\n' ' ')"
else
    warn "/dev/vfio 에 IOMMU 그룹이 없다 — 20-bind-vfio.sh 를 먼저 실행할 것"
fi

info "코어 수: $(nproc), reserved-cpus: $RESERVED_CPUS"
echo

# ─────────────────────────────────────────────────────────────
# k3s 설정 파일
# ─────────────────────────────────────────────────────────────
echo "== k3s 설정 =="
mkdir -p "$(dirname "$K3S_CONFIG")"
cat > "$K3S_CONFIG" <<EOF
# Mir 데이터플레인용 단일 노드 k3s 설정
# 생성: deploy/host/30-install-k3s.sh

disable:
  - traefik            # GUI 는 NodePort 로 노출한다

kubelet-arg:
  # CPU Manager static: Guaranteed QoS + 정수 cpu 요청 컨테이너에 배타 코어 할당.
  # 이게 없으면 DPDK worker 가 다른 프로세스와 코어를 나눠 쓰게 되어
  # busy-poll 이 의미를 잃는다.
  - cpu-manager-policy=static
  - reserved-cpus=$RESERVED_CPUS

  # Topology Manager: CPU · hugepage · vfio 장치를 같은 NUMA 노드로 정렬.
  # 100G 목표에서는 선택이 아니라 필수 — 노드를 넘나드는 DMA 는
  # UPI 를 타면서 대역폭이 급감한다.
  - topology-manager-policy=single-numa-node
  - topology-manager-scope=pod
EOF
info "$K3S_CONFIG 작성 완료"
sed 's/^/    | /' "$K3S_CONFIG"
echo

# ─────────────────────────────────────────────────────────────
# 설치 / 재설정
# ─────────────────────────────────────────────────────────────
if systemctl list-unit-files 2>/dev/null | grep -q '^k3s\.service'; then
    echo "== 기존 k3s 재설정 =="
    info "k3s 중지"
    systemctl stop k3s || true

    # 함정: cpu-manager-policy 를 바꾸면 kubelet 이 이전 상태 파일과 충돌해
    # 기동에 실패한다. 반드시 지워야 한다.
    if [[ -f "$CPU_MANAGER_STATE" ]]; then
        info "$CPU_MANAGER_STATE 삭제 (정책 변경 시 필수)"
        rm -f "$CPU_MANAGER_STATE"
    fi

    info "k3s 시작"
    systemctl start k3s
else
    echo "== k3s 신규 설치 =="
    command -v curl >/dev/null || die "curl 이 필요하다"
    curl -sfL https://get.k3s.io | sh -
fi

echo
echo "== 기동 확인 =="
for _ in $(seq 1 30); do
    k3s kubectl get node >/dev/null 2>&1 && break
    sleep 2
done
k3s kubectl get node -o wide || die "k3s 기동 실패 — journalctl -u k3s -n 50 확인"

echo
echo "kubeconfig: /etc/rancher/k3s/k3s.yaml"
echo "  export KUBECONFIG=/etc/rancher/k3s/k3s.yaml"
echo
echo "다음: ./40-verify-node.sh"
