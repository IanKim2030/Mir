#define _GNU_SOURCE

#include "pktbuild.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>

#define seterr(err, len, ...) \
    do { if ((err) && (len)) snprintf((err), (len), __VA_ARGS__); } while (0)

/* 페이로드를 채우는 패턴. hello packet 과 같은 표식을 쓴다 —
 * 캡처만 보고도 우리가 쏜 것인지 바로 구분된다. */
static const char PAYLOAD_PATTERN[] = "MIR";

static uint32_t clamp_count(uint32_t c)
{
    return c == 0 ? 1 : c;
}

/* a*b 를 하되 상한에서 포화시킨다 (조합 수 계산의 오버플로 방지). */
static uint32_t sat_mul(uint32_t a, uint32_t b)
{
    uint64_t r = (uint64_t)a * (uint64_t)b;
    return r > (uint64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)r;
}

static int parse_mac(const char *s, uint8_t out[6], char *err, size_t errlen)
{
    struct rte_ether_addr a;
    if (rte_ether_unformat_addr(s, &a) != 0) {
        seterr(err, errlen, "MAC 주소를 해석하지 못했다: \"%s\"", s);
        return -1;
    }
    memcpy(out, a.addr_bytes, 6);
    return 0;
}

/* 호스트 바이트 순서 IPv4 를 돌려준다. */
static int parse_ipv4(const char *s, uint32_t *out, char *err, size_t errlen)
{
    struct in_addr a;
    if (!s || !*s || inet_pton(AF_INET, s, &a) != 1) {
        seterr(err, errlen, "IPv4 주소를 해석하지 못했다: \"%s\"", s ? s : "(빈 값)");
        return -1;
    }
    *out = rte_be_to_cpu_32(a.s_addr);
    return 0;
}

/*
 * 한 변형을 조립한다. idx 로부터 각 가변 필드의 오프셋을 뽑아낸다.
 *
 * 조합 순서는 자릿수 전개다 — src_ip 가 가장 빨리 돌고 dst_port 가 가장 느리게
 * 돈다. 순서 자체에 의미는 없지만, 캡처를 보고 예측할 수 있어야 디버깅이 된다.
 */
struct vary {
    uint32_t ip_src, ip_dst;      /* 호스트 순서 base */
    uint16_t sport, dport;
    uint32_t n_ip_src, n_ip_dst, n_sport, n_dport;
};

static void build_one(uint8_t *frame, uint16_t frame_len, uint32_t idx,
                      const struct vary *v,
                      const uint8_t src_mac[6], const uint8_t dst_mac[6],
                      uint32_t vlan_id, uint32_t vlan_pcp,
                      uint8_t ttl, uint8_t dscp,
                      int is_tcp, uint32_t tcp_flags, uint16_t tcp_window,
                      uint32_t tcp_seq,
                      const uint8_t *payload, size_t payload_len,
                      int use_offload)
{
    memset(frame, 0, frame_len);

    uint32_t i = idx;
    uint32_t ip_src = v->ip_src + (i % v->n_ip_src); i /= v->n_ip_src;
    uint32_t ip_dst = v->ip_dst + (i % v->n_ip_dst); i /= v->n_ip_dst;
    uint16_t sport  = (uint16_t)(v->sport + (i % v->n_sport)); i /= v->n_sport;
    uint16_t dport  = (uint16_t)(v->dport + (i % v->n_dport));

    /* ── L2 ────────────────────────────────────────────── */
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)frame;
    memcpy(eth->dst_addr.addr_bytes, dst_mac, 6);
    memcpy(eth->src_addr.addr_bytes, src_mac, 6);

    uint16_t l2_len = sizeof(*eth);
    if (vlan_id != 0) {
        eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN);
        struct rte_vlan_hdr *vh = (struct rte_vlan_hdr *)(frame + l2_len);
        vh->vlan_tci = rte_cpu_to_be_16((uint16_t)((vlan_pcp & 0x7) << 13 |
                                                   (vlan_id & 0xfff)));
        vh->eth_proto = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
        l2_len += sizeof(*vh);
    } else {
        eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    }

    /* ── L3 ────────────────────────────────────────────── */
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(frame + l2_len);
    ip->version_ihl     = RTE_IPV4_VHL_DEF;          /* v4, IHL=5 */
    ip->type_of_service = (uint8_t)(dscp << 2);
    ip->total_length    = rte_cpu_to_be_16((uint16_t)(frame_len - l2_len));
    ip->packet_id       = 0;
    ip->fragment_offset = rte_cpu_to_be_16(RTE_IPV4_HDR_DF_FLAG);
    ip->time_to_live    = ttl;
    ip->next_proto_id   = is_tcp ? IPPROTO_TCP : IPPROTO_UDP;
    ip->src_addr        = rte_cpu_to_be_32(ip_src);
    ip->dst_addr        = rte_cpu_to_be_32(ip_dst);
    ip->hdr_checksum    = 0;

    uint16_t l3_len = sizeof(*ip);

    /* ── L4 ────────────────────────────────────────────── */
    uint16_t l4_len;
    if (is_tcp) {
        struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)(frame + l2_len + l3_len);
        tcp->src_port  = rte_cpu_to_be_16(sport);
        tcp->dst_port  = rte_cpu_to_be_16(dport);
        tcp->sent_seq  = rte_cpu_to_be_32(tcp_seq);
        tcp->recv_ack  = 0;
        tcp->data_off  = (uint8_t)((sizeof(*tcp) / 4) << 4);
        tcp->tcp_flags = (uint8_t)tcp_flags;
        tcp->rx_win    = rte_cpu_to_be_16(tcp_window);
        tcp->cksum     = 0;
        tcp->tcp_urp   = 0;
        l4_len = sizeof(*tcp);
    } else {
        struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(frame + l2_len + l3_len);
        udp->src_port    = rte_cpu_to_be_16(sport);
        udp->dst_port    = rte_cpu_to_be_16(dport);
        udp->dgram_len   = rte_cpu_to_be_16((uint16_t)(frame_len - l2_len - l3_len));
        udp->dgram_cksum = 0;
        l4_len = sizeof(*udp);
    }

    /* ── 페이로드 ──────────────────────────────────────── */
    size_t hdr = (size_t)l2_len + l3_len + l4_len;
    if (frame_len > hdr) {
        size_t room = (size_t)frame_len - hdr;
        uint8_t *p = frame + hdr;

        if (payload_len > 0) {
            size_t n = payload_len < room ? payload_len : room;
            memcpy(p, payload, n);
            p += n;
            room -= n;
        }
        for (size_t k = 0; k < room; k++)
            p[k] = (uint8_t)PAYLOAD_PATTERN[k % (sizeof(PAYLOAD_PATTERN) - 1)];
    }

    /* ── 체크섬 ────────────────────────────────────────── */
    void *l4 = frame + l2_len + l3_len;

    if (!use_offload) {
        /* 전부 소프트웨어로 계산한다. rte_ipv4_udptcp_cksum 은 L4 cksum 필드가
         * 0 인 상태를 전제하므로 위에서 0 으로 둔 그대로 부른다. */
        ip->hdr_checksum = rte_ipv4_cksum(ip);
        if (is_tcp)
            ((struct rte_tcp_hdr *)l4)->cksum = rte_ipv4_udptcp_cksum(ip, l4);
        else
            ((struct rte_udp_hdr *)l4)->dgram_cksum = rte_ipv4_udptcp_cksum(ip, l4);
    } else {
        /* TX 오프로드는 NIC 이 **최종** 체크섬을 채우지 않는다. L4 헤더에
         * IP 유사헤더 부분합을 미리 넣어 두면 NIC 이 페이로드 부분만 더해
         * 완성한다. 이 부분합을 빠뜨리면 유사헤더가 통째로 누락된 값이 나가고,
         * 수신 측에서 "incorrect" 로 잡힌다 — 실장비에서 실제로 겪은 문제다.
         *
         * IP 헤더 체크섬은 별도 오프로드라 여기서 건드리지 않는다(NIC 이 채운다). */
        ip->hdr_checksum = 0;
        if (is_tcp)
            ((struct rte_tcp_hdr *)l4)->cksum = rte_ipv4_phdr_cksum(ip, 0);
        else
            ((struct rte_udp_hdr *)l4)->dgram_cksum = rte_ipv4_phdr_cksum(ip, 0);
    }
}

int mir_pkt_set_build(mir_pkt_set *out,
                      const Mir__V1__PacketSpec *spec,
                      const uint8_t port_mac[6],
                      int offload_ok,
                      char *err, size_t errlen)
{
    memset(out, 0, sizeof(*out));

    if (!spec) {
        seterr(err, errlen, "packet 명세가 없다");
        return -1;
    }

    /* ── L3 검증 ───────────────────────────────────────── */
    if (spec->l3_case == MIR__V1__PACKET_SPEC__L3_IPV6) {
        seterr(err, errlen, "IPv6 는 Phase 2 에서 아직 지원하지 않는다 "
                            "(스키마만 확정돼 있다)");
        return -1;
    }
    if (spec->l3_case != MIR__V1__PACKET_SPEC__L3_IPV4 || !spec->ipv4) {
        seterr(err, errlen, "ipv4 명세가 필요하다");
        return -1;
    }

    int is_tcp;
    if (spec->l4_case == MIR__V1__PACKET_SPEC__L4_TCP && spec->tcp) {
        is_tcp = 1;
    } else if (spec->l4_case == MIR__V1__PACKET_SPEC__L4_UDP && spec->udp) {
        is_tcp = 0;
    } else {
        seterr(err, errlen, "tcp 또는 udp 명세가 필요하다");
        return -1;
    }

    /* ── L2 ────────────────────────────────────────────── */
    uint8_t src_mac[6], dst_mac[6];
    const Mir__V1__EthSpec *eth = spec->eth;

    if (eth && eth->src_mac && *eth->src_mac) {
        if (parse_mac(eth->src_mac, src_mac, err, errlen) != 0)
            return -1;
    } else {
        memcpy(src_mac, port_mac, 6);
    }

    if (!eth || !eth->dst_mac || !*eth->dst_mac) {
        seterr(err, errlen, "eth.dst_mac 이 필요하다 "
                            "(브로드캐스트를 원하면 ff:ff:ff:ff:ff:ff 로 명시할 것)");
        return -1;
    }
    if (parse_mac(eth->dst_mac, dst_mac, err, errlen) != 0)
        return -1;

    uint32_t vlan_id  = eth->vlan_id;
    uint32_t vlan_pcp = eth->vlan_pcp;
    if (vlan_id > 0xfff) {
        seterr(err, errlen, "vlan_id 범위 초과: %u (0~4095)", vlan_id);
        return -1;
    }

    /* ── 가변 필드 ─────────────────────────────────────── */
    struct vary v;
    memset(&v, 0, sizeof(v));

    if (parse_ipv4(spec->ipv4->src, &v.ip_src, err, errlen) != 0) return -1;
    if (parse_ipv4(spec->ipv4->dst, &v.ip_dst, err, errlen) != 0) return -1;
    v.n_ip_src = clamp_count(spec->ipv4->src_count);
    v.n_ip_dst = clamp_count(spec->ipv4->dst_count);

    uint32_t tcp_flags = 0, tcp_seq = 0;
    uint16_t tcp_window = 0;

    if (is_tcp) {
        v.sport    = (uint16_t)spec->tcp->src_port;
        v.dport    = (uint16_t)spec->tcp->dst_port;
        v.n_sport  = clamp_count(spec->tcp->src_port_count);
        v.n_dport  = clamp_count(spec->tcp->dst_port_count);
        tcp_flags  = spec->tcp->flags ? spec->tcp->flags : RTE_TCP_SYN_FLAG;
        tcp_window = spec->tcp->window ? (uint16_t)spec->tcp->window : 65535;
        tcp_seq    = spec->tcp->seq;
    } else {
        v.sport   = (uint16_t)spec->udp->src_port;
        v.dport   = (uint16_t)spec->udp->dst_port;
        v.n_sport = clamp_count(spec->udp->src_port_count);
        v.n_dport = clamp_count(spec->udp->dst_port_count);
    }

    /* ── 프레임 크기 ───────────────────────────────────── */
    uint16_t l2_len = (uint16_t)(sizeof(struct rte_ether_hdr) +
                                 (vlan_id ? sizeof(struct rte_vlan_hdr) : 0));
    uint16_t l3_len = (uint16_t)sizeof(struct rte_ipv4_hdr);
    uint16_t l4_len = (uint16_t)(is_tcp ? sizeof(struct rte_tcp_hdr)
                                        : sizeof(struct rte_udp_hdr));
    uint16_t hdr_len = (uint16_t)(l2_len + l3_len + l4_len);

    uint32_t frame_len = spec->frame_size ? spec->frame_size : hdr_len;

    /* 이더넷 최소 프레임(64B, CRC 포함 규정) 미만은 선로에 존재할 수 없다.
     * 헤더 합이 이미 그보다 크면 그대로 쓴다. */
    if (frame_len < RTE_ETHER_MIN_LEN - RTE_ETHER_CRC_LEN)
        frame_len = RTE_ETHER_MIN_LEN - RTE_ETHER_CRC_LEN;
    if (frame_len < hdr_len)
        frame_len = hdr_len;
    if (frame_len > MIR_PKT_MAX_FRAME) {
        seterr(err, errlen, "frame_size 가 상한을 넘는다: %u > %d",
               frame_len, MIR_PKT_MAX_FRAME);
        return -1;
    }

    /* ── 조합 수 ───────────────────────────────────────── */
    uint32_t want = sat_mul(sat_mul(v.n_ip_src, v.n_ip_dst),
                            sat_mul(v.n_sport, v.n_dport));
    uint32_t n = want > MIR_PKT_MAX_VARIANTS ? MIR_PKT_MAX_VARIANTS : want;

    uint8_t *buf = calloc((size_t)n, frame_len);
    if (!buf) {
        seterr(err, errlen, "변형 버퍼 할당 실패 (%u × %uB)", n, frame_len);
        return -1;
    }

    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    if (spec->payload.len > 0) {
        payload = spec->payload.data;
        payload_len = spec->payload.len;
    }

    uint8_t ttl  = spec->ipv4->ttl ? (uint8_t)spec->ipv4->ttl : 64;
    uint8_t dscp = (uint8_t)(spec->ipv4->dscp & 0x3f);

    for (uint32_t i = 0; i < n; i++) {
        build_one(buf + (size_t)i * frame_len, (uint16_t)frame_len, i, &v,
                  src_mac, dst_mac, vlan_id, vlan_pcp, ttl, dscp,
                  is_tcp, tcp_flags, tcp_window, tcp_seq,
                  payload, payload_len, offload_ok);
    }

    out->variants           = buf;
    out->n_variants         = n;
    out->requested_variants = want;
    out->frame_len          = (uint16_t)frame_len;
    out->use_offload        = offload_ok;
    out->l2_len             = (uint8_t)l2_len;
    out->l3_len             = (uint8_t)l3_len;
    out->l4_proto           = is_tcp ? IPPROTO_TCP : IPPROTO_UDP;
    return 0;
}

void mir_pkt_set_free(mir_pkt_set *s)
{
    if (!s)
        return;
    free(s->variants);
    s->variants = NULL;
    s->n_variants = 0;
}
