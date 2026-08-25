#define _GNU_SOURCE

#include "rx_engine.h"

#include <stdio.h>
#include <string.h>

#include <rte_branch_prediction.h>
#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_tcp.h>
#include <rte_udp.h>

#include "events.h"
#include "stats.h"

#define LOG(level, fmt, ...) \
    rte_log(RTE_LOG_##level, RTE_LOGTYPE_USER1, "rx: " fmt "\n", ##__VA_ARGS__)

#define RX_BURST 256

/* 이벤트 레이트 제한. 종류마다 이 간격 안에서는 한 번만 올린다.
 *
 * 없으면 닫힌 포트로 SYN 홍수를 쏠 때 상대의 RST 하나하나가 이벤트가 되어
 * 링을 즉시 넘치게 만든다. 이벤트는 "이런 일이 일어나고 있다"는 표본이면
 * 충분하고, 정확한 수는 카운터에 있다. */
#define EVENT_MIN_INTERVAL_NS 200000000ull  /* 200ms → 종류당 최대 5개/초 */

static struct {
    volatile int stop;
    unsigned     lcore_id;
    int          running;
    uint16_t     port_id;
} g = { .lcore_id = RTE_MAX_LCORE };

/* 종류별 마지막 이벤트 시각 (레이트 제한용). RX lcore 만 만진다. */
static uint64_t g_last_event_ns[8];

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* flow_key = "src:sport>dst:dport/proto". 응답 패킷이므로 src 가 상대다. */
static void make_flow_key(char *buf, size_t len,
                          const struct rte_ipv4_hdr *ip,
                          uint16_t sport, uint16_t dport, const char *proto)
{
    uint32_t s = rte_be_to_cpu_32(ip->src_addr);
    uint32_t d = rte_be_to_cpu_32(ip->dst_addr);
    snprintf(buf, len, "%u.%u.%u.%u:%u>%u.%u.%u.%u:%u/%s",
             (s >> 24) & 0xff, (s >> 16) & 0xff, (s >> 8) & 0xff, s & 0xff, sport,
             (d >> 24) & 0xff, (d >> 16) & 0xff, (d >> 8) & 0xff, d & 0xff, dport,
             proto);
}

/* 레이트 제한을 통과하면 이벤트를 넣는다. 넘치거나 제한에 걸리면 조용히 넘긴다
 * (넘침은 event_drop 으로 집계). */
static void maybe_emit(struct mir_lcore_stats *st, mir_event_kind kind,
                       uint64_t ts, uint16_t port_id,
                       const struct rte_ipv4_hdr *ip,
                       uint16_t sport, uint16_t dport, const char *proto,
                       const char *detail)
{
    if (ts - g_last_event_ns[kind] < EVENT_MIN_INTERVAL_NS)
        return;
    g_last_event_ns[kind] = ts;

    mir_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.ts_ns   = ts;
    ev.kind    = kind;
    ev.port_id = port_id;
    make_flow_key(ev.flow_key, sizeof(ev.flow_key), ip, sport, dport, proto);
    snprintf(ev.detail, sizeof(ev.detail), "%s", detail);

    if (mir_events_push(&ev) != 0)
        st->event_drop++;
}

static void classify(struct rte_mbuf *m, struct mir_lcore_stats *st,
                     uint16_t port_id, uint64_t ts)
{
    st->rx_pkts++;
    st->rx_bytes += rte_pktmbuf_pkt_len(m);

    const struct rte_ether_hdr *eth =
        rte_pktmbuf_mtod(m, const struct rte_ether_hdr *);
    uint16_t etype = rte_be_to_cpu_16(eth->ether_type);
    size_t   off   = sizeof(*eth);

    /* VLAN 태그는 건너뛴다. QinQ 는 Phase 3 범위 밖 — 이중 태그는 other 로 샌다. */
    if (etype == RTE_ETHER_TYPE_VLAN) {
        if (rte_pktmbuf_pkt_len(m) < off + sizeof(struct rte_vlan_hdr))
            return;
        const struct rte_vlan_hdr *vh =
            rte_pktmbuf_mtod_offset(m, const struct rte_vlan_hdr *, off);
        etype = rte_be_to_cpu_16(vh->eth_proto);
        off  += sizeof(*vh);
    }

    if (etype != RTE_ETHER_TYPE_IPV4) {
        st->rx_non_ip++;
        return;
    }

    if (rte_pktmbuf_pkt_len(m) < off + sizeof(struct rte_ipv4_hdr))
        return;
    const struct rte_ipv4_hdr *ip =
        rte_pktmbuf_mtod_offset(m, const struct rte_ipv4_hdr *, off);

    /* IHL 이 5보다 크면 옵션이 있다. 옵션 길이만큼 L4 오프셋을 민다. */
    uint8_t ihl = (ip->version_ihl & 0x0f) * 4;
    if (ihl < sizeof(struct rte_ipv4_hdr))
        return;
    size_t l4_off = off + ihl;

    if (ip->next_proto_id == IPPROTO_UDP) {
        st->rx_udp++;
        return;
    }
    if (ip->next_proto_id != IPPROTO_TCP)
        return;   /* ICMP 등은 지금 세지 않는다 */

    if (rte_pktmbuf_pkt_len(m) < l4_off + sizeof(struct rte_tcp_hdr))
        return;
    const struct rte_tcp_hdr *tcp =
        rte_pktmbuf_mtod_offset(m, const struct rte_tcp_hdr *, l4_off);

    uint16_t sport = rte_be_to_cpu_16(tcp->src_port);
    uint16_t dport = rte_be_to_cpu_16(tcp->dst_port);
    uint8_t  f     = tcp->tcp_flags;

    /* 우선순위가 있다 — RST 는 다른 무엇과 함께 와도 RST 로 본다. */
    if (f & RTE_TCP_RST_FLAG) {
        st->rx_tcp_rst++;
        maybe_emit(st, MIR_EVENT_RST_RECEIVED, ts, port_id, ip, sport, dport,
                   "tcp", "RST 수신 — 포트 닫힘/거부 추정");
        return;
    }

    int syn = (f & RTE_TCP_SYN_FLAG) != 0;
    int ack = (f & RTE_TCP_ACK_FLAG) != 0;
    int fin = (f & RTE_TCP_FIN_FLAG) != 0;

    if (syn && ack) {
        st->rx_tcp_syn_ack++;     /* 우리 SYN 에 대한 응답 — 포트 열림 */
    } else if (syn && !ack) {
        st->rx_tcp_syn++;         /* 상대가 우리에게 연결을 걸어온다 */
    } else if (fin) {
        st->rx_tcp_fin++;
    } else if (ack && !syn && !fin) {
        st->rx_tcp_ack++;
    } else {
        /* SYN 도 ACK 도 RST 도 FIN 도 아닌 조합 — 이상동작 후보 */
        st->rx_tcp_other++;
        char d[MIR_EVENT_DETAIL_MAX];
        snprintf(d, sizeof(d), "예상 밖 플래그 0x%02x", f);
        maybe_emit(st, MIR_EVENT_UNEXPECTED_FLAG, ts, port_id, ip, sport, dport,
                   "tcp", d);
    }
}

static int rx_main(void *arg)
{
    (void)arg;
    struct mir_lcore_stats *st = &mir_stats[rte_lcore_id()];
    struct rte_mbuf        *bufs[RX_BURST];

    LOG(INFO, "RX 폴링 시작 (port=%u, lcore=%u)", g.port_id, rte_lcore_id());

    while (likely(!g.stop)) {
        uint16_t n = rte_eth_rx_burst(g.port_id, 0, bufs, RX_BURST);
        if (n == 0)
            continue;

        uint64_t ts = now_ns();
        for (uint16_t i = 0; i < n; i++) {
            classify(bufs[i], st, g.port_id, ts);
            rte_pktmbuf_free(bufs[i]);
        }
    }

    LOG(INFO, "RX 폴링 종료");
    return 0;
}

int mir_rx_start(const mir_port *port, unsigned lcore_id, char *err, size_t errlen)
{
    if (g.running) {
        if (err) snprintf(err, errlen, "RX 엔진이 이미 실행 중이다");
        return -1;
    }
    if (!port || !port->started) {
        if (err) snprintf(err, errlen, "포트가 준비되지 않았다");
        return -1;
    }

    /* 이벤트 링은 RX lcore 가 있는 소켓에 둔다. 2의 거듭제곱. */
    if (mir_events_init(4096, (int)rte_lcore_to_socket_id(lcore_id)) != 0) {
        if (err) snprintf(err, errlen, "이벤트 링 생성 실패");
        return -1;
    }

    g.stop     = 0;
    g.port_id  = port->port_id;
    g.lcore_id = lcore_id;

    int rc = rte_eal_remote_launch(rx_main, NULL, lcore_id);
    if (rc != 0) {
        if (err) snprintf(err, errlen, "lcore %u launch 실패 (%d)", lcore_id, rc);
        g.lcore_id = RTE_MAX_LCORE;
        return -1;
    }
    g.running = 1;
    return 0;
}

int mir_rx_stop(void)
{
    if (!g.running)
        return 0;

    g.stop = 1;
    rte_eal_wait_lcore(g.lcore_id);
    g.running  = 0;
    g.lcore_id = RTE_MAX_LCORE;
    mir_events_free();
    return 0;
}

unsigned mir_rx_lcore(void)
{
    return g.running ? g.lcore_id : RTE_MAX_LCORE;
}
