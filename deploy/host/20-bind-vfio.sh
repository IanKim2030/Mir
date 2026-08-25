#!/usr/bin/env bash
#
# 20-bind-vfio.sh — DPDK가 점유할 NIC PF를 vfio-pci 로 바인딩한다.
#
#   sudo ./20-bind-vfio.sh                       # 후보 NIC 목록만 출력
#   sudo ./20-bind-vfio.sh 0000:3b:00.0 0000:3b:00.1
#   sudo ./20-bind-vfio.sh --force 0000:3b:00.0  # 안전장치 무시
#
# 바인딩한 BDF 목록과 oneshot systemd 유닛을 설치해 재부팅 후에도 유지한다.
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
# 주의: `cmd | grep -q` 를 쓰지 않는다. grep -q 는 첫 매칭에서 즉시 끝나는데,
# 그때 앞 명령이 아직 쓰고 있으면 SIGPIPE 로 죽고 set -o pipefail 이 그걸
# 파이프라인 실패로 판정한다. **패턴이 맞을 때만 실패하는** 형태라 더 나쁘다.
# 출력을 변수에 받아 here-string 으로 넘기면 파이프가 없어져 문제가 사라진다.
dmesg_out=$(dmesg 2>/dev/null || true)
if grep -qi "DMAR: IOMMU enabled" <<<"$dmesg_out"; then
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
        addr_out=$(ip -o addr show dev "$netdev" 2>/dev/null || true)
        if grep -q "inet " <<<"$addr_out"; then
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

    echo
done

# ─────────────────────────────────────────────────────────────
# 재부팅 후 유지
#
# driverctl 을 쓰지 않는다. Ubuntu 26.04 의 0.115-2build1 은 패키징이 깨져
# driverctl@.service 와 udev 규칙이 파일시스템 루트(/driverctl@.service,
# /rules.d/)에 설치된다 — systemd 도 udev 도 그걸 보지 못해 override 가
# 저장은 되지만 부팅 때 적용되지 않는다. 실장비에서 재부팅 후 4포트가 전부
# i40e 로 돌아가는 것으로 확인했다.
#
# 대신 BDF 목록과 oneshot 유닛을 직접 둔다. vendor:device ID 로 잡는
# (options vfio-pci ids=...) 방식보다 정확하다 — 같은 모델 카드가 더 꽂혀도
# 여기 적힌 것만 넘어간다. PF↔인스턴스를 BDF 로 고정하는 이 프로젝트의
# 설계와도 일치한다.
# ─────────────────────────────────────────────────────────────
BDF_LIST=/etc/mir/vfio-bdfs
HELPER=/usr/local/sbin/mir-vfio-bind
UNIT=/etc/systemd/system/mir-vfio-bind.service

echo "== 재부팅 후 유지 설정 =="

mkdir -p /etc/mir
printf '%s\n' "${BDFS[@]}" > "$BDF_LIST"
info "$BDF_LIST 기록 (${#BDFS[@]}개)"

cat > "$HELPER" <<'HELPER_EOF'
#!/usr/bin/env bash
# 부팅 시 지정된 BDF 를 vfio-pci 로 바인딩한다. 20-bind-vfio.sh 가 설치한다.
#
# 목록(/etc/mir/vfio-bdfs)은 20-bind-vfio.sh 가 관리용 인터페이스 보호를
# 통과시킨 것만 기록한다. 여기서는 그 목록을 그대로 신뢰한다 — 부팅 초기에는
# 기본 경로가 아직 없어 같은 검사를 다시 할 수 없다.
set -u
LIST=/etc/mir/vfio-bdfs
[[ -r "$LIST" ]] || exit 0

modprobe vfio-pci || exit 1

while read -r bdf; do
    [[ -n "$bdf" ]] || continue
    dev="/sys/bus/pci/devices/$bdf"
    [[ -d "$dev" ]] || { echo "mir-vfio-bind: $bdf 없음 — 건너뜀"; continue; }

    cur=""
    [[ -e "$dev/driver" ]] && cur=$(basename "$(readlink -f "$dev/driver")")
    [[ "$cur" == "vfio-pci" ]] && continue

    [[ -n "$cur" ]] && echo "$bdf" > "$dev/driver/unbind" 2>/dev/null
    echo "vfio-pci" > "$dev/driver_override"
    echo "$bdf" > /sys/bus/pci/drivers_probe 2>/dev/null

    new=""
    [[ -e "$dev/driver" ]] && new=$(basename "$(readlink -f "$dev/driver")")
    if [[ "$new" == "vfio-pci" ]]; then
        echo "mir-vfio-bind: $bdf → vfio-pci"
    else
        echo "mir-vfio-bind: $bdf 바인딩 실패 (현재 ${new:--})" >&2
    fi
done < "$LIST"
HELPER_EOF
chmod 755 "$HELPER"
info "$HELPER 설치"

cat > "$UNIT" <<'UNIT_EOF'
[Unit]
Description=Bind Mir dataplane NICs to vfio-pci
Documentation=file:///etc/mir/vfio-bdfs
# 네트워크 설정보다 먼저 끝나야 한다. 나중에 하면 커널 드라이버가 이미
# netdev 를 올려 두어 unbind 하는 모양이 되고, 그 사이 설정이 적용된다.
After=systemd-modules-load.service
Before=network-pre.target
Wants=network-pre.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/mir-vfio-bind

[Install]
WantedBy=multi-user.target
UNIT_EOF
info "$UNIT 설치"

systemctl daemon-reload
systemctl enable mir-vfio-bind.service >/dev/null 2>&1 \
    && info "mir-vfio-bind.service 활성화 — 재부팅 후에도 유지된다" \
    || warn "유닛 활성화 실패 — 재부팅 시 바인딩이 사라진다"
echo

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
echo "이 BDF 와 그룹 번호를 deploy/compose/.env 의 MIR_DP*_PF / MIR_DP*_VFIO_GROUP 에,"
echo "BDF 는 함대 설정(fleet.yaml)의 pf 에도 같은 값으로 넣을 것."
echo "다음: ./30-install-docker.sh"
