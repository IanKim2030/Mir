#!/usr/bin/env bash
#
# 20-bind-vfio.sh — DPDK가 점유할 NIC PF를 vfio-pci 로 바인딩한다.
#
#   sudo ./20-bind-vfio.sh                       # 후보 NIC 목록만 출력
#   sudo ./20-bind-vfio.sh 0000:3b:00.0 0000:3b:00.1
#   sudo ./20-bind-vfio.sh --force 0000:3b:00.0  # 안전장치 무시
#
# driverctl 이 있으면 재부팅 후에도 유지되도록 override 를 등록한다.
#
set -euo pipefail

FORCE=0
BDFS=()
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        -*)      echo "알 수 없는 옵션: $arg" >&2; exit 2 ;;
        *)       BDFS+=("$arg") ;;
    esac
done

die()  { echo "ERROR: $*" >&2; exit 1; }
warn() { echo "WARN:  $*" >&2; }
info() { echo "  $*"; }

[[ $EUID -eq 0 ]] || die "root 권한이 필요하다 (sudo)"

# ─────────────────────────────────────────────────────────────
# 후보 NIC 목록 (인자 없이 실행한 경우)
# ─────────────────────────────────────────────────────────────
if [[ ${#BDFS[@]} -eq 0 ]]; then
    echo "PCI 이더넷 장치 목록:"
    echo
    printf "  %-14s %-10s %-12s %-8s %s\n" "BDF" "DRIVER" "NETDEV" "NUMA" "설명"
    for dev in /sys/bus/pci/devices/*; do
        class=$(cat "$dev/class")
        [[ "$class" == 0x0200* ]] || continue          # 0x0200xx = Ethernet controller

        bdf=$(basename "$dev")
        drv="-"; [[ -e "$dev/driver" ]] && drv=$(basename "$(readlink -f "$dev/driver")")
        netdev="-"; [[ -d "$dev/net" ]] && netdev=$(ls "$dev/net" 2>/dev/null | head -1)
        numa=$(cat "$dev/numa_node" 2>/dev/null || echo "?")
        desc=$(lspci -s "$bdf" 2>/dev/null | cut -d' ' -f2- | cut -c1-50)

        printf "  %-14s %-10s %-12s %-8s %s\n" "$bdf" "$drv" "${netdev:--}" "node$numa" "$desc"
    done
    echo
    echo "위 목록에서 DPDK로 넘길 BDF를 인자로 지정해 다시 실행한다."
    echo "  주의: NUMA 노드가 서로 다른 NIC을 한 파드에 묶지 말 것."
    exit 0
fi

# ─────────────────────────────────────────────────────────────
# 사전 점검
# ─────────────────────────────────────────────────────────────
echo "== 사전 점검 =="

NOIOMMU=0
if dmesg 2>/dev/null | grep -qi "DMAR: IOMMU enabled"; then
    info "IOMMU: 활성 (vfio-pci 정상 모드 — 비특권 파드 가능)"
elif [[ -n "$(ls -A /sys/kernel/iommu_groups 2>/dev/null)" ]]; then
    info "IOMMU: 그룹 존재 (활성으로 간주)"
else
    warn "IOMMU 비활성 — no-IOMMU 폴백이 필요하다."
    warn "  데이터플레인 컨테이너를 privileged 로 돌려야 하고 격리 경계가 사라진다."
    warn "  10-kernel-cmdline.md 3절을 먼저 읽을 것."
    NOIOMMU=1
    [[ $FORCE -eq 0 ]] && die "IOMMU 없이 진행하려면 --force 를 붙일 것"
fi

modprobe vfio-pci || die "vfio-pci 모듈을 로드하지 못했다"
info "vfio-pci 모듈 로드됨"

if [[ $NOIOMMU -eq 1 ]]; then
    echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode 2>/dev/null \
        || warn "no-IOMMU 모드 활성화 실패 (커널에 CONFIG_VFIO_NOIOMMU 가 없을 수 있음)"
fi

# 관리용 NIC(기본 경로를 가진 인터페이스)을 실수로 넘기면 장비 접속이 끊긴다.
MGMT_NETDEV=$(ip -o route show default 2>/dev/null | awk '{print $5}' | head -1 || true)
[[ -n "$MGMT_NETDEV" ]] && info "관리용 인터페이스: $MGMT_NETDEV (바인딩 대상에서 보호)"
echo

# ─────────────────────────────────────────────────────────────
# 바인딩
# ─────────────────────────────────────────────────────────────
for bdf in "${BDFS[@]}"; do
    dev="/sys/bus/pci/devices/$bdf"
    echo "== $bdf =="
    [[ -d "$dev" ]] || die "$bdf 장치가 존재하지 않는다"

    cur_drv="-"
    [[ -e "$dev/driver" ]] && cur_drv=$(basename "$(readlink -f "$dev/driver")")

    if [[ "$cur_drv" == "vfio-pci" ]]; then
        info "이미 vfio-pci 에 바인딩되어 있음 — 건너뜀"
        echo
        continue
    fi

    # 안전장치: 관리용 NIC 보호
    if [[ -d "$dev/net" ]]; then
        netdev=$(ls "$dev/net" | head -1)
        if [[ "$netdev" == "$MGMT_NETDEV" && $FORCE -eq 0 ]]; then
            die "$bdf ($netdev) 는 기본 경로를 가진 관리용 인터페이스다. 바인딩하면 접속이 끊긴다. 정말 넘기려면 --force"
        fi
        if ip -o addr show dev "$netdev" 2>/dev/null | grep -q "inet "; then
            warn "$netdev 에 IP가 설정되어 있다 — 바인딩하면 해제된다"
        fi
        info "현재 netdev: $netdev (드라이버 $cur_drv)"
    fi

    # IOMMU 그룹 동거 장치 확인.
    # 그룹 내 다른 장치가 커널 드라이버를 쓰고 있으면 EAL 초기화가
    # "group not viable" 로 실패한다. 미리 경고해 두면 디버깅 시간을 크게 줄인다.
    if [[ -e "$dev/iommu_group" ]]; then
        group=$(basename "$(readlink -f "$dev/iommu_group")")
        members=$(ls "$dev/iommu_group/devices")
        member_count=$(echo "$members" | wc -w)
        info "IOMMU 그룹 $group (구성원 ${member_count}개)"
        if [[ $member_count -gt 1 ]]; then
            for m in $members; do
                [[ "$m" == "$bdf" ]] && continue
                m_drv="-"
                [[ -e "/sys/bus/pci/devices/$m/driver" ]] && \
                    m_drv=$(basename "$(readlink -f "/sys/bus/pci/devices/$m/driver")")
                if [[ "$m_drv" != "-" && "$m_drv" != "vfio-pci" ]]; then
                    warn "  동거 장치 $m 이 드라이버 '$m_drv' 사용 중 → 'group not viable' 위험"
                    warn "  해당 장치도 vfio-pci 로 넘기거나 unbind 해야 한다"
                fi
            done
        fi
    fi

    # driver_override 방식.
    # new_id 는 같은 vendor:device 를 가진 다른 장치까지 끌어가므로 쓰지 않는다.
    if [[ "$cur_drv" != "-" ]]; then
        info "$cur_drv 에서 unbind"
        echo "$bdf" > "$dev/driver/unbind"
    fi
    echo "vfio-pci" > "$dev/driver_override"
    echo "$bdf" > /sys/bus/pci/drivers_probe

    new_drv="-"
    [[ -e "$dev/driver" ]] && new_drv=$(basename "$(readlink -f "$dev/driver")")
    [[ "$new_drv" == "vfio-pci" ]] || die "$bdf 바인딩 실패 (현재 드라이버: $new_drv)"
    info "vfio-pci 바인딩 완료"

    # 재부팅 후 유지
    if command -v driverctl >/dev/null 2>&1; then
        driverctl set-override "$bdf" vfio-pci
        info "driverctl override 등록 — 재부팅 후에도 유지됨"
    else
        warn "driverctl 미설치 — 이 바인딩은 재부팅 시 사라진다"
        warn "  sudo apt install driverctl  후 이 스크립트를 다시 실행할 것"
    fi
    echo
done

# ─────────────────────────────────────────────────────────────
# 결과
# ─────────────────────────────────────────────────────────────
echo "== 결과 =="
for bdf in "${BDFS[@]}"; do
    group="?"
    [[ -e "/sys/bus/pci/devices/$bdf/iommu_group" ]] && \
        group=$(basename "$(readlink -f "/sys/bus/pci/devices/$bdf/iommu_group")")
    numa=$(cat "/sys/bus/pci/devices/$bdf/numa_node" 2>/dev/null || echo "?")
    info "$bdf  →  /dev/vfio/$group  (NUMA node$numa)"
done
echo
[[ $NOIOMMU -eq 1 ]] && \
    warn "no-IOMMU 모드다. overlays/baremetal 의 securityContext 를 privileged 로 바꿔야 한다."
echo "이 BDF 목록을 deploy/k8s/overlays/baremetal/sriovdp-config.yaml 에 반영할 것."
echo "다음: ./30-install-k3s.sh"
