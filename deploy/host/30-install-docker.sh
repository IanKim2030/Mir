#!/usr/bin/env bash
#
# 30-install-docker.sh — 생성 장비에 Docker Engine + Compose 플러그인을 준비한다.
#
#   sudo ./30-install-docker.sh
#
# 오케스트레이터를 두지 않는 이유는 배치가 스케줄러가 아니라 **하드웨어**로
# 정해지기 때문이다. 데이터플레인 컨테이너는 PF 가 꽂힌 장비에서만 돌 수 있어
# 스케줄러가 정할 것이 없고, 코어는 isolcpus 와 같은 출처를 보는 명시적
# cpuset 으로 고정하는 편이 정확하다. → docs/REQUIREMENTS.md 4-3 절
#
set -euo pipefail

die()  { echo "ERROR: $*" >&2; exit 1; }
warn() { echo "WARN:  $*" >&2; }
info() { echo "  $*"; }

[[ $EUID -eq 0 ]] || die "root 권한이 필요하다 (sudo)"

echo "== 사전 점검 =="

# hugepage 가 없으면 EAL 초기화 자체가 실패한다.
hp_total=$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo)
if [[ "${hp_total:-0}" -eq 0 ]]; then
    warn "HugePages_Total = 0 — 10-kernel-cmdline.md 를 먼저 적용하고 재부팅할 것"
    warn "  (지금 진행해도 설치는 되지만 데이터플레인이 기동에 실패한다)"
else
    hp_size=$(awk '/^Hugepagesize:/ {print $2" "$3}' /proc/meminfo)
    info "hugepage: ${hp_total}개 × ${hp_size}"
fi

# 주의: `cmd | grep -q` 를 쓰지 않는다. grep -q 는 첫 매칭에서 즉시 끝나는데,
# 그때 앞 명령이 아직 쓰고 있으면 SIGPIPE 로 죽고 set -o pipefail 이 그걸
# 파이프라인 실패로 판정한다. **패턴이 맞을 때만 실패하는** 형태라 더 나쁘다.
# 출력을 변수에 받아 here-string 으로 넘기면 파이프가 없어져 문제가 사라진다.
vfio_entries=$(ls /dev/vfio/ 2>/dev/null || true)
if grep -qv '^vfio$' <<<"$vfio_entries" && [[ -n "$vfio_entries" ]]; then
    info "vfio 그룹: $(ls /dev/vfio/ | grep -v '^vfio$' | tr '\n' ' ')"
else
    warn "/dev/vfio 에 IOMMU 그룹이 없다 — 20-bind-vfio.sh 를 먼저 실행할 것"
fi

isolated=$(cat /sys/devices/system/cpu/isolated 2>/dev/null || echo "")
if [[ -z "$isolated" ]]; then
    warn "격리된 코어가 없다 (isolcpus 미설정) — .env 의 cpuset 이 OS 스레드와 겹친다"
else
    info "격리 코어: $isolated"
fi
info "코어 수: $(nproc)"
echo

# ─────────────────────────────────────────────────────────────
# Docker 설치
# ─────────────────────────────────────────────────────────────
if command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1; then
    echo "== Docker 이미 설치됨 =="
    info "$(docker --version)"
    info "$(docker compose version)"
else
    echo "== Docker 설치 =="
    command -v curl >/dev/null || die "curl 이 필요하다"

    # 배포판 저장소의 docker.io 는 compose 플러그인이 빠져 있거나 오래된
    # 경우가 많아 공식 스크립트를 쓴다.
    curl -fsSL https://get.docker.com | sh

    docker compose version >/dev/null 2>&1 \
        || die "compose 플러그인이 없다 — docker-compose-plugin 을 설치할 것"
fi

systemctl enable --now docker
echo

# ─────────────────────────────────────────────────────────────
# 확인
# ─────────────────────────────────────────────────────────────
echo "== 기동 확인 =="
docker info >/dev/null 2>&1 || die "docker 데몬에 접속할 수 없다 — systemctl status docker"
info "storage driver : $(docker info --format '{{.Driver}}')"
info "cgroup version : $(docker info --format '{{.CgroupVersion}}')"

# 컨테이너가 cpuset 을 실제로 받는지 확인한다. 이게 안 되면 배타 코어 배치가
# 통째로 무의미해지므로 여기서 잡고 넘어간다.
if docker run --rm --cpuset-cpus=0 busybox:latest true 2>/dev/null; then
    info "cpuset 지정 동작 확인"
else
    warn "cpuset 테스트를 건너뛴다 (busybox 이미지를 받지 못했다)"
fi

echo
echo "다음: ./40-verify-node.sh"
echo "그 뒤: deploy/compose/ 의 .env 를 이 장비에 맞게 작성"
