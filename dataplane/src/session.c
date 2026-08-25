#define _GNU_SOURCE

#include "session.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include <rte_branch_prediction.h>
#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_malloc.h>
#include <rte_tcp.h>

#include "events.h"
#include "stats.h"

#define seterr(err, len, ...) \
    do { if ((err) && (len)) snprintf((err), (len), __VA_ARGS__); } while (0)

/* 한 회차에 보낼 SYN 최대 수. 세션이 많아도 RX 폴링을 굶기지 않게 나눠 보낸다. */
#define SEND_BURST 64

/* 이벤트 레이트 제한 (종류당). rx_engine 과 같은 원칙 — 표본이면 충분하다. */
#define EVENT_MIN_INTERVAL_NS 200000000ull

/* SYN 의 초기 시퀀스 번호 베이스. 세션 index 를 더해 유일하게 만든다.
 * SYN-ACK 의 ack 필드가 이 값 +1 이어야 우리 세션의 응답으로 인정한다. */
#define ISN_BASE 0xC0DE0000u

enum sess_state {
    S_IDLE = 0,   /* 아직 SYN 안 보냄 */
    S_SENT,       /* SYN 보냄, SYN-ACK 대기 */
    S_DONE,       /* 지정 행동까지 끝남 */
    S_REFUSED,    /* SYN 에 RST */
    S_TIMEDOUT,   /* SYN-ACK 무응답 */
};

struct session {
    uint8_t  state;
    uint16_t src_port;
    uint32_t syn_seq;
    uint64_t sent_ns;
};

/* 제어 스레드가 스테이징하고 RX lcore 가 채택하는 설정. */
struct config {
    uint8_t  src_mac[6];
    uint8_t  dst_mac[6];
    uint32_t src_ip;      /* host order */
    uint32_t dst_ip;
    uint16_t dst_port;
    uint16_t src_port_base;
    uint32_t sessions;
    int      action;      /* Mir__V1__HandshakeSpec__Action */
    uint64_t timeout_ns;
};

static struct {
    /* pending: 제어 스레드가 쓰고 RX lcore 가 채택. */
    volatile int    pending_start;
    volatile int    pending_stop;
    struct config   staged;

    /* 아래는 RX lcore 만 만진다 (채택 후). */
    int             active;
    struct config   cfg;
    struct session *tab;
    uint32_t        next_to_send;   /* 아직 SYN 안 보낸 첫 세션 */
    const mir_port *port;
    uint16_t        tx_queue;

    mir_session_stats stats;
    uint64_t          last_event_ns[8];
} g;

/* ── 파싱 (제어 스레드) ────────────────────────────────────── */

static int parse_mac(const char *s, uint8_t out[6])
{
    struct rte_ether_addr a;
    if (rte_ether_unformat_addr(s, &a) != 0)
        return -1;
    memcpy(out, a.addr_bytes, 6);
    return 0;
}

static int parse_ip(const char *s, uint32_t *out)
{
    struct in_addr a;
    if (!s || !*s || inet_pton(AF_INET, s, &a) != 1)
        return -1;
    *out = rte_be_to_cpu_32(a.s_addr);
    return 0;
}

int mir_session_request_start(const Mir__V1__HandshakeSpec *spec,
                              const mir_port *port, uint16_t tx_queue_id,
                              char *err, size_t errlen)
{
    if (g.active || g.pending_start) {
        seterr(err, errlen, "handshake 세션이 이미 실행 중이다 — 먼저 정지할 것");
        return -1;
    }
    if (!spec) {
        seterr(err, errlen, "handshake 명세가 없다");
        return -1;
    }

    struct config c;
    memset(&c, 0, sizeof(c));

    if (!spec->eth || !spec->eth->dst_mac || parse_mac(spec->eth->dst_mac, c.dst_mac) != 0) {
        seterr(err, errlen, "eth.dst_mac 이 필요하다");
        return -1;
    }
    if (spec->eth->src_mac && *spec->eth->src_mac) {
        if (parse_mac(spec->eth->src_mac, c.src_mac) != 0) {
            seterr(err, errlen, "eth.src_mac 해석 실패");
            return -1;
        }
    } else {
        struct rte_ether_addr mac;
        rte_eth_macaddr_get(port->port_id, &mac);
        memcpy(c.src_mac, mac.addr_bytes, 6);
    }

    if (parse_ip(spec->src_ip, &c.src_ip) != 0) {
        seterr(err, errlen, "src_ip 가 필요하다");
        return -1;
    }
    if (parse_ip(spec->dst_ip, &c.dst_ip) != 0) {
        seterr(err, errlen, "dst_ip 가 필요하다");
        return -1;
    }
    if (spec->dst_port == 0 || spec->dst_port > 65535) {
        seterr(err, errlen, "dst_port 가 필요하다 (1~65535)");
        return -1;
    }
    c.dst_port = (uint16_t)spec->dst_port;

    c.sessions      = spec->sessions ? spec->sessions : 1;
    c.src_port_base = spec->src_port_base ? (uint16_t)spec->src_port_base : 40000;

    /* src port 범위가 65535 를 넘지 않아야 각 세션이 유일한 포트를 갖는다. */
    if ((uint32_t)c.src_port_base + c.sessions > 65536) {
        seterr(err, errlen, "src_port_base(%u) + sessions(%u) 가 65536 을 넘는다",
               c.src_port_base, c.sessions);
        return -1;
    }

    c.action     = spec->on_synack;   /* UNSPECIFIED(0) = COMPLETE 처럼 취급 */
    c.timeout_ns = (uint64_t)(spec->synack_timeout_ms ? spec->synack_timeout_ms : 1000)
                   * 1000000ull;

    g.staged        = c;
    g.port          = port;
    g.tx_queue      = tx_queue_id;
    g.pending_start = 1;
    return 0;
}

void mir_session_request_stop(void)
{
    if (g.active || g.pending_start)
        g.pending_stop = 1;
}

/* ── 프레임 조립 (RX lcore) ────────────────────────────────── */

/* TCP 세그먼트 하나(페이로드 없음)를 mbuf 로 만들어 tx_burst 로 보낸다.
 * 소프트웨어 체크섬 — 저속이라 오프로드 플래그 관리가 오히려 부담이다. */
static void send_seg(uint16_t src_port, uint32_t seq, uint32_t ack, uint8_t flags)
{
    struct rte_mbuf *m = rte_pktmbuf_alloc(g.port->pool);
    if (!m)
        return;

    uint16_t frame_len = sizeof(struct rte_ether_hdr) +
                         sizeof(struct rte_ipv4_hdr) +
                         sizeof(struct rte_tcp_hdr);
    uint8_t *p = (uint8_t *)rte_pktmbuf_append(m, frame_len);
    if (!p) {
        rte_pktmbuf_free(m);
        return;
    }
    memset(p, 0, frame_len);

    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)p;
    memcpy(eth->dst_addr.addr_bytes, g.cfg.dst_mac, 6);
    memcpy(eth->src_addr.addr_bytes, g.cfg.src_mac, 6);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(p + sizeof(*eth));
    ip->version_ihl     = RTE_IPV4_VHL_DEF;
    ip->total_length    = rte_cpu_to_be_16(sizeof(*ip) + sizeof(struct rte_tcp_hdr));
    ip->fragment_offset = rte_cpu_to_be_16(RTE_IPV4_HDR_DF_FLAG);
    ip->time_to_live    = 64;
    ip->next_proto_id   = IPPROTO_TCP;
    ip->src_addr        = rte_cpu_to_be_32(g.cfg.src_ip);
    ip->dst_addr        = rte_cpu_to_be_32(g.cfg.dst_ip);
    ip->hdr_checksum    = rte_ipv4_cksum(ip);

    struct rte_tcp_hdr *tcp =
        (struct rte_tcp_hdr *)(p + sizeof(*eth) + sizeof(*ip));
    tcp->src_port  = rte_cpu_to_be_16(src_port);
    tcp->dst_port  = rte_cpu_to_be_16(g.cfg.dst_port);
    tcp->sent_seq  = rte_cpu_to_be_32(seq);
    tcp->recv_ack  = rte_cpu_to_be_32(ack);
    tcp->data_off  = (sizeof(*tcp) / 4) << 4;
    tcp->tcp_flags = flags;
    tcp->rx_win    = rte_cpu_to_be_16(65535);
    tcp->cksum     = rte_ipv4_udptcp_cksum(ip, tcp);

    if (rte_eth_tx_burst(g.port->port_id, g.tx_queue, &m, 1) != 1)
        rte_pktmbuf_free(m);
}

/* ── 채택 / 정리 (RX lcore) ────────────────────────────────── */

static void adopt(void)
{
    g.cfg = g.staged;
    g.tab = rte_zmalloc_socket("mir_sessions",
                               (size_t)g.cfg.sessions * sizeof(struct session),
                               0, rte_socket_id());
    if (!g.tab) {
        /* 메모리 부족 — 조용히 유휴로 남는다. 스테이징만 지운다. */
        g.pending_start = 0;
        return;
    }

    for (uint32_t i = 0; i < g.cfg.sessions; i++) {
        g.tab[i].state    = S_IDLE;
        g.tab[i].src_port = (uint16_t)(g.cfg.src_port_base + i);
        g.tab[i].syn_seq  = ISN_BASE + i;
    }

    memset(&g.stats, 0, sizeof(g.stats));
    g.stats.active     = 1;
    g.stats.sessions   = g.cfg.sessions;
    g.stats.rtt_min_us = UINT32_MAX;
    g.next_to_send     = 0;
    g.active           = 1;
    g.pending_start    = 0;
}

static void teardown(void)
{
    if (g.tab) {
        rte_free(g.tab);
        g.tab = NULL;
    }
    g.active = 0;
    g.stats.active = 0;
    g.pending_stop = 0;
}

/* ── 이벤트 ────────────────────────────────────────────────── */

static void emit(mir_event_kind kind, uint64_t ts, uint16_t src_port,
                 uint32_t rtt_us, const char *detail)
{
    if (ts - g.last_event_ns[kind] < EVENT_MIN_INTERVAL_NS)
        return;
    g.last_event_ns[kind] = ts;

    mir_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.ts_ns   = ts;
    ev.kind    = kind;
    ev.port_id = g.port->port_id;
    ev.rtt_us  = rtt_us;

    uint32_t s = g.cfg.src_ip, d = g.cfg.dst_ip;
    snprintf(ev.flow_key, sizeof(ev.flow_key),
             "%u.%u.%u.%u:%u>%u.%u.%u.%u:%u/tcp",
             (s >> 24) & 0xff, (s >> 16) & 0xff, (s >> 8) & 0xff, s & 0xff, src_port,
             (d >> 24) & 0xff, (d >> 16) & 0xff, (d >> 8) & 0xff, d & 0xff,
             g.cfg.dst_port);
    snprintf(ev.detail, sizeof(ev.detail), "%s", detail);

    if (mir_events_push(&ev) != 0)
        mir_stats[rte_lcore_id()].event_drop++;
}

/* ── 서비스 루프 (RX lcore) ────────────────────────────────── */

void mir_session_service(uint64_t now_ns)
{
    if (g.pending_stop) {
        teardown();
        return;
    }
    if (g.pending_start && !g.active)
        adopt();
    if (!g.active)
        return;

    /* 아직 SYN 안 보낸 세션에 SYN 을 보낸다 (회차당 SEND_BURST 개까지). */
    uint32_t sent_now = 0;
    while (g.next_to_send < g.cfg.sessions && sent_now < SEND_BURST) {
        struct session *s = &g.tab[g.next_to_send];
        send_seg(s->src_port, s->syn_seq, 0, RTE_TCP_SYN_FLAG);
        s->state   = S_SENT;
        s->sent_ns = now_ns;
        g.stats.sent++;
        g.next_to_send++;
        sent_now++;
    }

    /* SYN-ACK 무응답 timeout 검사. 저속이라 전수 훑어도 부담이 없다. */
    for (uint32_t i = 0; i < g.cfg.sessions; i++) {
        struct session *s = &g.tab[i];
        if (s->state != S_SENT)
            continue;
        if (now_ns - s->sent_ns >= g.cfg.timeout_ns) {
            s->state = S_TIMEDOUT;
            g.stats.timed_out++;
            emit(MIR_EVENT_SYNACK_TIMEOUT, now_ns, s->src_port, 0,
                 "SYN-ACK 무응답");
        }
    }
}

/* ── 수신 처리 (RX lcore) ──────────────────────────────────── */

int mir_session_handle(struct rte_mbuf *m, uint64_t now_ns)
{
    if (!g.active)
        return 0;

    const struct rte_ether_hdr *eth =
        rte_pktmbuf_mtod(m, const struct rte_ether_hdr *);
    if (rte_be_to_cpu_16(eth->ether_type) != RTE_ETHER_TYPE_IPV4)
        return 0;

    size_t off = sizeof(*eth);
    if (rte_pktmbuf_pkt_len(m) < off + sizeof(struct rte_ipv4_hdr))
        return 0;
    const struct rte_ipv4_hdr *ip =
        rte_pktmbuf_mtod_offset(m, const struct rte_ipv4_hdr *, off);

    if (ip->next_proto_id != IPPROTO_TCP)
        return 0;
    /* 우리 세션의 상대(dst_ip)에서 우리(src_ip)로 오는 것만 본다. */
    if (rte_be_to_cpu_32(ip->src_addr) != g.cfg.dst_ip ||
        rte_be_to_cpu_32(ip->dst_addr) != g.cfg.src_ip)
        return 0;

    uint8_t ihl = (ip->version_ihl & 0x0f) * 4;
    size_t  l4  = off + ihl;
    if (rte_pktmbuf_pkt_len(m) < l4 + sizeof(struct rte_tcp_hdr))
        return 0;
    const struct rte_tcp_hdr *tcp =
        rte_pktmbuf_mtod_offset(m, const struct rte_tcp_hdr *, l4);

    uint16_t our_port = rte_be_to_cpu_16(tcp->dst_port);
    if (our_port < g.cfg.src_port_base ||
        our_port >= g.cfg.src_port_base + g.cfg.sessions)
        return 0;

    struct session *s = &g.tab[our_port - g.cfg.src_port_base];
    if (s->state != S_SENT)
        return 1;   /* 우리 세션 포트지만 이미 처리됨 — 소비하고 버린다 */

    uint8_t  f       = tcp->tcp_flags;
    uint32_t their_seq = rte_be_to_cpu_32(tcp->sent_seq);
    uint32_t ack       = rte_be_to_cpu_32(tcp->recv_ack);

    /* RST → 거부. */
    if (f & RTE_TCP_RST_FLAG) {
        s->state = S_REFUSED;
        g.stats.refused++;
        emit(MIR_EVENT_HANDSHAKE_REFUSED, now_ns, our_port, 0, "SYN 에 RST");
        return 1;
    }

    /* SYN-ACK 이고 ack 가 우리 SYN 을 인정하는가. */
    int synack = (f & RTE_TCP_SYN_FLAG) && (f & RTE_TCP_ACK_FLAG);
    if (!synack || ack != s->syn_seq + 1)
        return 1;   /* 우리 포트로 온 다른 무엇 — 소비만 한다 */

    uint32_t rtt_us = (uint32_t)((now_ns - s->sent_ns) / 1000);
    g.stats.synack++;
    g.stats.rtt_sum_us += rtt_us;
    g.stats.rtt_count++;
    if (rtt_us < g.stats.rtt_min_us) g.stats.rtt_min_us = rtt_us;
    if (rtt_us > g.stats.rtt_max_us) g.stats.rtt_max_us = rtt_us;

    /* 지정 행동. */
    uint32_t our_next = s->syn_seq + 1;
    uint32_t their_next = their_seq + 1;

    switch (g.cfg.action) {
    case MIR__V1__HANDSHAKE_SPEC__ACTION__ACTION_HALF_OPEN:
        /* ACK 를 보내지 않는다 — 서버를 half-open 으로 남긴다. */
        break;
    case MIR__V1__HANDSHAKE_SPEC__ACTION__ACTION_RST:
        send_seg(our_port, our_next, 0, RTE_TCP_RST_FLAG);
        break;
    default:  /* UNSPECIFIED = COMPLETE */
        send_seg(our_port, our_next, their_next, RTE_TCP_ACK_FLAG);
        break;
    }

    s->state = S_DONE;
    g.stats.completed++;
    emit(MIR_EVENT_HANDSHAKE_DONE, now_ns, our_port, rtt_us, "3-way 응답 처리");
    return 1;
}

void mir_session_get_stats(mir_session_stats *out)
{
    *out = g.stats;
}

void mir_session_shutdown(void)
{
    if (g.active)
        teardown();
}
