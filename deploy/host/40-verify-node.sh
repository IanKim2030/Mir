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
sec "4. Docker"

# 이 스크립트는 root 없이 도는 것이 전제다. 그런데 docker 데몬 소켓은 root
# 또는 docker 그룹 멤버만 열 수 있어서, "데몬이 죽었다"와 "내가 접근 권한이
# 없다"가 같은 실패로 보인다. 둘을 구분하지 않으면 멀쩡한 장비를 두고
# systemctl 을 뒤지게 된다.
DOCKER=""
if command -v docker >/dev/null 2>&1; then
    if docker info >/dev/null 2>&1; then
        DOCKER="docker"
    elif sudo -n docker info >/dev/null 2>&1; then
        DOCKER="sudo -n docker"
    fi
fi

if ! command -v docker >/dev/null 2>&1; then
    bad "docker 없음" "30-install-docker.sh 실행 필요"
elif [[ -z "$DOCKER" ]]; then
    bad "docker 데몬 응답 없음" "systemctl status docker"
else
    ok "docker" "$(docker --version | sed 's/^Docker version //')"

    if [[ "$DOCKER" == sudo* ]]; then
        # compose 명령마다 sudo 를 붙여야 한다는 뜻이라 그냥 넘길 정보가 아니다.
        meh "현재 사용자가 docker 그룹에 없음" \
            "compose 를 sudo 로 돌리거나 'sudo usermod -aG docker $USER' 후 재로그인"
    fi

    if $DOCKER compose version >/dev/null 2>&1; then
        ok "compose 플러그인" "$($DOCKER compose version --short 2>/dev/null)"
    else
        bad "compose 플러그인 없음" "docker-compose-plugin 설치 필요"
    fi

    # cgroup v2 여야 cpuset 이 컨테이너에 제대로 걸린다.
    cgv=$($DOCKER info --format '{{.CgroupVersion}}' 2>/dev/null || echo "?")
    [[ "$cgv" == "2" ]]         && ok  "cgroup v2"         || meh "cgroup v$cgv" "v2 권장 — cpuset 동작을 반드시 확인할 것"
fi

# ─────────────────────────────────────────────────────────────
sec "5. 코어 배치"

# 오케스트레이터가 없으므로 isolcpus 가 코어 격리의 **유일한 출처**다.
# compose 의 cpuset 이 이 집합 안에 들어가야 한다.
isolated=$(cat /sys/devices/system/cpu/isolated 2>/dev/null || echo "")
if [[ -n "$isolated" ]]; then
    ok "격리 코어" "$isolated"

    envf="$(dirname "$0")/../compose/.env"
    if [[ -f "$envf" ]]; then
        # .env 의 cpuset 이 격리 집합을 벗어나는지 본다. 벗어나면 그 인스턴스의
        # worker 가 OS 스레드와 코어를 나눠 쓰게 되어 busy-poll 이 의미를 잃는다.
        expand() { # "2-5,8" -> "2 3 4 5 8"
            local out=() part lo hi
            IFS=',' read -ra part <<< "$1"
            for p in "${part[@]}"; do
                if [[ "$p" == *-* ]]; then
                    lo=${p%-*}; hi=${p#*-}
                    for ((c=lo; c<=hi; c++)); do out+=("$c"); done
                else
                    out+=("$p")
                fi
            done
            echo "${out[@]}"
        }
        iso_list=" $(expand "$isolated") "
        bad_cpus=""
        while IFS='=' read -r key val; do
            [[ "$key" =~ ^MIR_DP[0-9]+_CPUSET$ ]] || continue
            for c in $(expand "$val"); do
                [[ "$iso_list" == *" $c "* ]] || bad_cpus="$bad_cpus $key:$c"
            done
        done < <(grep -E '^MIR_DP[0-9]+_CPUSET=' "$envf" 2>/dev/null || true)

        [[ -z "$bad_cpus" ]]             && ok  ".env cpuset 이 격리 코어 안에 있음"             || bad ".env cpuset 이 격리 코어를 벗어남" "$bad_cpus"
    else
        meh "compose/.env 없음" "dataplane.env.example 을 복사해 작성할 것"
    fi
else
    bad "격리된 코어 없음" "isolcpus 미설정 — 10-kernel-cmdline.md"
fi

# ─────────────────────────────────────────────────────────────
# 장비 두 대 이상을 한 제어부로 묶을 때만 검사한다 (MIR_REQUIRE_PTP=1).
# 한 대짜리 구성에서는 시계가 어긋날 상대가 없다.
if [[ "${MIR_REQUIRE_PTP:-0}" == "1" ]]; then
    sec "6. 시계 동기 (PTP)"

    if ! ls /dev/ptp* >/dev/null 2>&1; then
        bad "PTP 하드웨어 시계 없음" "NIC 이 PHC 를 노출하지 않는다 — 60-ptp.md"
    else
        ok "PTP 하드웨어 시계" "$(ls /dev/ptp* | tr '
' ' ')"
    fi

    if systemctl is-active --quiet 'ptp4l@*' 2>/dev/null || pgrep -x ptp4l >/dev/null 2>&1; then
        ok "ptp4l 동작 중"
    else
        bad "ptp4l 미동작" "장비 간 텔레메트리 상관과 지연 측정이 무의미해진다"
    fi

    # ptp4l 만으로는 NIC 시계만 맞는다. 데이터플레인이 읽는 것은 CLOCK_REALTIME
    # 이므로 phc2sys 가 PHC -> 시스템 시계로 흘려줘야 한다.
    if systemctl is-active --quiet 'phc2sys@*' 2>/dev/null || pgrep -x phc2sys >/dev/null 2>&1; then
        ok "phc2sys 동작 중"
    else
        bad "phc2sys 미동작" "NIC 시계만 맞고 시스템 시계는 그대로다 — 60-ptp.md"
    fi

    # NTP 데몬이 함께 돌면 두 소스가 시스템 시계를 서로 밀어내며 진동한다.
    for svc in chronyd systemd-timesyncd ntpd; do
        if systemctl is-active --quiet "$svc" 2>/dev/null; then
            bad "$svc 가 PTP 와 함께 동작 중" "시계 소스는 하나만 남길 것"
        fi
    done
fi

# ─────────────────────────────────────────────────────────────
printf "
[1m결과: %d PASS, %d WARN, %d FAIL[0m
" "$PASS" "$WARN" "$FAIL"
if [[ $FAIL -gt 0 ]]; then
    echo "FAIL 항목을 해결한 뒤 다시 실행할 것. docs/DEPLOYMENT.md 트러블슈팅 절 참조."
    exit 1
fi
echo "장비 사전조건 충족. 다음: deploy/compose/ 에서 docker compose up -d"
exit 0
