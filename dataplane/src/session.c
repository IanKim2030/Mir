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

/* L7 요청 최대 길이. GET 한 줄이면 충분하고, 한 세그먼트(MSS 이하)·기본 mbuf
 * data room(2048) - 헤더(54) 안에 들어가야 한다. */
#define MAX_L7_REQ 1400

enum sess_state {
    S_IDLE = 0,     /* 아직 SYN 안 보냄 */
    S_SENT,         /* SYN 보냄, SYN-ACK 대기 */
    S_DONE,         /* 핸드셰이크 지정 행동까지 끝남 (Phase 4: l7 없음) */
    S_ESTABLISHED,  /* 3-way 완료 + 요청 송신, 응답 대기 (Phase 5a) */
    S_CLOSED,       /* 데이터 교환 후 FIN 종료 (Phase 5a) */
    S_REFUSED,      /* SYN 에 RST */
    S_TIMEDOUT,     /* SYN-ACK 무응답 */
};

struct session {
    uint8_t  state;
    uint8_t  fin_sent;   /* 우리 FIN 을 보냈나 */
    uint8_t  got_resp;   /* 응답 첫 세그먼트를 이미 집계했나 */
    uint16_t src_port;
    uint32_t syn_seq;
    uint32_t snd_nxt;    /* 우리가 보낼 다음 seq */
    uint32_t rcv_nxt;    /* 상대에게 기대하는 다음 seq */
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

    /* Phase 5a — 3-way 완료 후 보낼 L7 요청. spec 은 곧 free 되므로 복사해 둔다. */
    uint8_t  l7_req[MAX_L7_REQ];
    uint32_t l7_req_len;
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

    /* L7 요청 복사 (있으면). spec->l7_request 는 반환 후 free 된다. */
    if (spec->l7_request.len > 0) {
        if (spec->l7_request.len > MAX_L7_REQ) {
            seterr(err, errlen, "l7_request 가 %d 바이트를 넘는다 (%zu)",
                   MAX_L7_REQ, spec->l7_request.len);
            return -1;
        }
        memcpy(c.l7_req, spec->l7_request.data, spec->l7_request.len);
        c.l7_req_len = (uint32_t)spec->l7_request.len;
    }

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

/* TCP 세그먼트 하나를 mbuf 로 만들어 tx_burst 로 보낸다. payload 가 있으면
 * TCP 헤더 뒤에 싣는다 (Phase 5a 의 L7 요청). 소프트웨어 체크섬 — 저속이라
 * 오프로드 플래그 관리가 오히려 부담이다. */
static void send_seg_data(uint16_t src_port, uint32_t seq, uint32_t ack,
                          uint8_t flags, const uint8_t *payload, uint16_t paylen)
{
    struct rte_mbuf *m = rte_pktmbuf_alloc(g.port->pool);
    if (!m)
        return;

    uint16_t frame_len = sizeof(struct rte_ether_hdr) +
                         sizeof(struct rte_ipv4_hdr) +
                         sizeof(struct rte_tcp_hdr) + paylen;
    uint8_t *p = (uint8_t *)rte_pktmbuf_append(m, frame_len);
    if (!p) {
        rte_pktmbuf_free(m);
        return;
    }
    memset(p, 0, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                 sizeof(struct rte_tcp_hdr));

    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)p;
    memcpy(eth->dst_addr.addr_bytes, g.cfg.dst_mac, 6);
    memcpy(eth->src_addr.addr_bytes, g.cfg.src_mac, 6);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(p + sizeof(*eth));
    ip->version_ihl     = RTE_IPV4_VHL_DEF;
    ip->total_length    = rte_cpu_to_be_16(sizeof(*ip) + sizeof(struct rte_tcp_hdr) + paylen);
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

    if (payload && paylen)
        memcpy(p + sizeof(*eth) + sizeof(*ip) + sizeof(*tcp), payload, paylen);

    tcp->cksum = rte_ipv4_udptcp_cksum(ip, tcp);

    if (rte_eth_tx_burst(g.port->port_id, g.tx_queue, &m, 1) != 1)
        rte_pktmbuf_free(m);
}

/* 페이로드 없는 세그먼트 (SYN/ACK/RST/FIN 등). */
static void send_seg(uint16_t src_port, uint32_t seq, uint32_t ack, uint8_t flags)
{
    send_seg_data(src_port, seq, ack, flags, NULL, 0);
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

    /* 세션마다 유일한 ISN. 상위 비트를 ISN_BASE 로 고정해 우리 세그먼트를
     * 캡처에서 알아보기 쉽게 남긴다(SYN-ACK 의 ack 매칭에도 쓴다).
     *
     * ★ 같은 4-tuple(같은 src 포트)로 연달아 재접속하면 상대가 이전 연결을
     *   TIME_WAIT 로 붙들고 있어 SYN-ACK 대신 challenge ACK 를 주거나 조용히
     *   버린다. 이는 상대의 정상 TCP 동작이다(우리 SYN 에 timestamp 옵션이
     *   없어 서버가 새 incarnation 으로 인식하지 못한다). 반복 실행은 src
     *   포트를 바꿔서 한다 — 다중 세션은 세션마다 포트가 다르므로 무관하다. */
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

/* ── 데이터 경로 처리 (RX lcore, Phase 5a) ─────────────────────
 *
 * S_ESTABLISHED 세션으로 오는 세그먼트를 처리한다. 순서 맞는 데이터만 받아
 * rcv_nxt 를 전진시키고 ACK 로 확인하며, 응답 첫 세그먼트에서 HTTP 상태줄을
 * 본다. 상대 FIN 이 오면 우리도 FIN|ACK 로 닫는다.
 *
 * 재전송·윈도우·혼잡제어는 없다 — 점대점 로컬 링크에서 손실이 무시할 수준인
 * 검증 도구라, 손실 시 세션은 그냥 응답 미완으로 남고 STOP 이 거둔다. */
static void handle_established(struct session *s, const struct rte_ipv4_hdr *ip,
                              const struct rte_tcp_hdr *tcp, uint64_t now_ns)
{
    uint8_t  f         = tcp->tcp_flags;
    uint32_t their_seq = rte_be_to_cpu_32(tcp->sent_seq);
    uint16_t iptot     = rte_be_to_cpu_16(ip->total_length);
    uint8_t  ihl       = (ip->version_ihl & 0x0f) * 4;
    uint8_t  thl       = ((tcp->data_off >> 4) & 0x0f) * 4;
    int      datalen   = (int)iptot - ihl - thl;
    if (datalen < 0)
        datalen = 0;
    const uint8_t *payload = (const uint8_t *)tcp + thl;

    if (f & RTE_TCP_RST_FLAG) {
        s->state = S_CLOSED;
        return;
    }

    /* 순서 맞는 데이터만 받아들인다 (순서 어긋나면 현재 rcv_nxt 로 재-ACK). */
    if (datalen > 0 && their_seq == s->rcv_nxt) {
        if (!s->got_resp) {
            s->got_resp = 1;
            g.stats.responded++;
            /* "HTTP/1.x 2NN" — 상태줄 첫 자리가 2 면 2xx. */
            if (datalen >= 12 && memcmp(payload, "HTTP/1.", 7) == 0 &&
                payload[9] == '2')
                g.stats.http_2xx++;
            emit(MIR_EVENT_HANDSHAKE_DONE, now_ns, s->src_port, 0, "L7 응답 수신");
        }
        s->rcv_nxt += (uint32_t)datalen;
        g.stats.bytes_rx += (uint32_t)datalen;
    }

    /* FIN — 순서 맞으면 소비하고 우리도 닫는다. */
    if ((f & RTE_TCP_FIN_FLAG) && their_seq + (uint32_t)datalen == s->rcv_nxt) {
        s->rcv_nxt += 1;   /* FIN 은 seq 하나를 소비한다 */
        send_seg(s->src_port, s->snd_nxt, s->rcv_nxt,
                 RTE_TCP_FIN_FLAG | RTE_TCP_ACK_FLAG);
        s->snd_nxt += 1;
        s->fin_sent = 1;
        s->state    = S_CLOSED;
        g.stats.closed++;
        return;
    }

    /* 데이터를 받았으면 ACK 로 확인해 준다 (FIN 이면 위에서 이미 닫았다). */
    if (datalen > 0)
        send_seg(s->src_port, s->snd_nxt, s->rcv_nxt, RTE_TCP_ACK_FLAG);
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
    if (s->state == S_ESTABLISHED) {
        handle_established(s, ip, tcp, now_ns);
        return 1;
    }
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
    uint32_t our_next   = s->syn_seq + 1;
    uint32_t their_next = their_seq + 1;
    s->snd_nxt = our_next;
    s->rcv_nxt = their_next;

    switch (g.cfg.action) {
    case MIR__V1__HANDSHAKE_SPEC__ACTION__ACTION_HALF_OPEN:
        /* ACK 를 보내지 않는다 — 서버를 half-open 으로 남긴다. */
        s->state = S_DONE;
        g.stats.completed++;
        emit(MIR_EVENT_HANDSHAKE_DONE, now_ns, our_port, rtt_us, "half-open 유지");
        return 1;
    case MIR__V1__HANDSHAKE_SPEC__ACTION__ACTION_RST:
        send_seg(our_port, our_next, 0, RTE_TCP_RST_FLAG);
        s->state = S_DONE;
        g.stats.completed++;
        emit(MIR_EVENT_HANDSHAKE_DONE, now_ns, our_port, rtt_us, "RST 종료");
        return 1;
    default:  /* UNSPECIFIED = COMPLETE */
        send_seg(our_port, our_next, their_next, RTE_TCP_ACK_FLAG);  /* 3-way 완료 */

        if (g.cfg.l7_req_len > 0) {
            /* Phase 5a — 요청을 실어 보내고 데이터 경로로 진입한다. */
            send_seg_data(our_port, s->snd_nxt, s->rcv_nxt,
                          RTE_TCP_PSH_FLAG | RTE_TCP_ACK_FLAG,
                          g.cfg.l7_req, (uint16_t)g.cfg.l7_req_len);
            s->snd_nxt  += g.cfg.l7_req_len;
            s->got_resp  = 0;
            s->fin_sent  = 0;
            s->state     = S_ESTABLISHED;
            g.stats.established++;
            g.stats.req_sent++;
            emit(MIR_EVENT_HANDSHAKE_DONE, now_ns, our_port, rtt_us,
                 "3-way 완료+요청 송신");
        } else {
            s->state = S_DONE;
            g.stats.completed++;
            emit(MIR_EVENT_HANDSHAKE_DONE, now_ns, our_port, rtt_us,
                 "3-way 응답 처리");
        }
        return 1;
    }
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
