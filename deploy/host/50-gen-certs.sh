#!/usr/bin/env bash
#
# 50-gen-certs.sh — 제어 채널(④ gRPC)용 자체 CA 와 인증서를 발급한다.
#
#   ./50-gen-certs.sh out/ 10.10.40.121 10.10.40.122
#
# 첫 인자는 출력 디렉터리, 그 뒤는 **사이드카가 붙는 장비의 주소**들이다.
# 제어부가 "10.10.40.121:9100" 으로 다이얼하면 TLS 는 그 host 부분을 서버
# 이름으로 검증하므로, 해당 IP(또는 호스트명)가 인증서 SAN 에 들어 있어야 한다.
#
# 왜 필요한가: 한 장비 안에서만 돌 때는 평문이어도 넘어갔지만, 장비 경계를
# 넘으면 :9100 에 닿는 누구나 라인레이트 패킷 제너레이터를 조종할 수 있다.
# 관리 전용 VLAN 을 쓰더라도 그것 하나에 기대지 않는다.
#
set -euo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }

OUT="${1:-}"
shift || true
HOSTS=("$@")

[[ -n "$OUT" ]]          || die "사용법: $0 <출력디렉터리> <장비주소>..."
[[ ${#HOSTS[@]} -gt 0 ]] || die "장비 주소를 하나 이상 지정할 것 (fleet.yaml 의 address 와 같아야 한다)"
command -v openssl >/dev/null || die "openssl 이 필요하다"

DAYS="${DAYS:-825}"   # 브라우저/라이브러리가 흔히 거부하지 않는 상한
mkdir -p "$OUT"
cd "$OUT"

# ── CA ───────────────────────────────────────────────────────
if [[ -f ca.key && -f ca.crt ]]; then
    echo "== 기존 CA 재사용 (ca.crt) =="
else
    echo "== CA 생성 =="
    openssl genrsa -out ca.key 4096 2>/dev/null
    openssl req -x509 -new -nodes -key ca.key -sha256 -days 3650 \
        -subj "/CN=mir-ca" -out ca.crt
fi

# ── 인증서 하나 발급 ─────────────────────────────────────────
# $1 = 파일 접두사, $2 = CN, $3.. = SAN 항목
issue() {
    local name="$1" cn="$2"; shift 2

    local san=""
    local i=1
    for entry in "$@"; do
        # IP 와 DNS 를 구분해 넣는다. IP 를 DNS SAN 으로 넣으면 검증에 실패한다.
        if [[ "$entry" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ || "$entry" == *:* ]]; then
            san+="IP.$i:$entry,"
        else
            san+="DNS.$i:$entry,"
        fi
        i=$((i + 1))
    done
    san="${san%,}"

    openssl genrsa -out "$name.key" 4096 2>/dev/null
    openssl req -new -key "$name.key" -subj "/CN=$cn" -out "$name.csr"

    # 프로세스 치환(<(...)) 대신 평범한 파일을 쓴다 — openssl 구현에 따라
    # /dev/fd 를 열지 못한다. 출력 디렉터리 안에 두어 /tmp 접근에도 기대지 않는다.
    local ext="$name.ext"
    printf 'subjectAltName=%s\nextendedKeyUsage=serverAuth,clientAuth\n' "$san" > "$ext"

    openssl x509 -req -in "$name.csr" -CA ca.crt -CAkey ca.key -CAcreateserial \
        -out "$name.crt" -days "$DAYS" -sha256 -extfile "$ext"

    rm -f "$name.csr" "$ext"
    echo "  $name.crt  (CN=$cn, SAN=$san)"
}

echo
echo "== 사이드카 인증서 =="
# 장비마다 하나씩. 그 장비의 사이드카 전부가 같은 인증서를 쓴다
# (같은 호스트 netns 에서 포트만 다르므로 이름이 같다).
for h in "${HOSTS[@]}"; do
    issue "agent-$h" "$h" "$h"
done

echo
echo "== 제어부 인증서 =="
# 제어부는 클라이언트로만 쓰이므로 SAN 이 검증에 쓰이지는 않지만,
# 비워 두면 일부 스택이 거부해서 CN 을 그대로 넣는다.
issue "control" "mir-control" "mir-control"

chmod 600 ./*.key
echo
echo "== 배포 =="
echo "  각 생성 장비:  ca.crt + agent-<주소>.{crt,key}"
echo "  제어 장비:     ca.crt + control.{crt,key}"
echo
echo "환경변수 (사이드카):"
echo "  MIR_TLS_CERT=/etc/mir/tls/agent.crt"
echo "  MIR_TLS_KEY=/etc/mir/tls/agent.key"
echo "  MIR_TLS_CA=/etc/mir/tls/ca.crt"
echo
echo "★ .key 는 저장소에 커밋하지 말 것."
