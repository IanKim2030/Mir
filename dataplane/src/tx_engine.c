#define _GNU_SOURCE

#include "tx_engine.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <rte_branch_prediction.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_memcpy.h>

#include "pktbuild.h"
#include "rx_engine.h"
#include "stats.h"

#define LOG(level, fmt, ...) \
    rte_log(RTE_LOG_##level, RTE_LOGTYPE_USER1, "tx: " fmt "\n", ##__VA_ARGS__)

#define seterr(err, len, ...) \
    do { if ((err) && (len)) snprintf((err), (len), __VA_ARGS__); } while (0)

/* 기본 burst. 너무 작으면 tx_burst 호출 오버헤드가 지배하고, 너무 크면
 * 페이싱 해상도가 나빠진다. 32 는 DPDK 예제들의 관례값이다. */
#define DEFAULT_BURST 32

/*
 * 페이싱이 이만큼 밀리면 따라잡기를 포기하고 현재 시각으로 재동기한다.
 *
 * 없으면 한 번 밀린 뒤 next 가 계속 과거에 머물러 **무제한 폭주**로 만회하려
 * 든다. 요청한 pps 를 잠깐 초과하는 것보다 그쪽이 훨씬 나쁘다.
 */
#define RESYNC_SLACK_BURSTS 16

struct worker_ctx {
    uint16_t            port_id;
    uint16_t            queue_id;
    unsigned            lcore_id;
    const mir_pkt_set  *pkts;
    struct rte_mempool *pool;

    uint32_t burst;
    uint64_t cycles_per_burst;  /* 0 = 최대 속도 */
    uint64_t end_tsc;           /* 0 = 무한 */
    uint64_t start_at_ns;       /* 0 = 즉시 */

    uint32_t cursor;            /* 변형 순환 위치 (worker 마다 독립) */
};

static struct {
    int      running;
    volatile int stop;

    mir_pkt_set       pkts;
    struct worker_ctx ctx[RTE_MAX_LCORE];
    unsigned          worker_lcore[RTE_MAX_LCORE];
    uint32_t          n_workers;

    mir_tx_status status;
} g;

/* ─────────────────────────────────────────────────────────── */

static uint64_t now_realtime_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/*
 * start_at_ns 까지 기다린다.
 *
 * 장비 여러 대가 같은 시각에 송신을 시작하게 만드는 것이 목적이므로, 마지막
 * 구간은 자지 않고 TSC 로 돈다 — nanosleep 의 깨어남 지터(수십~수백 µs)가
 * 그대로 장비 간 편차가 되기 때문이다. 멀리 남았을 때만 잠깐씩 잔다.
 */
static void wait_until(uint64_t target_ns, volatile int *stop)
{
    if (target_ns == 0)
        return;

    for (;;) {
        if (*stop)
            return;

        uint64_t now = now_realtime_ns();
        if (now >= target_ns)
            return;

        uint64_t left = target_ns - now;
        if (left > 2000000ull) {          /* 2ms 넘게 남으면 1ms 씩 잔다 */
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
            nanosleep(&ts, NULL);
        }
        /* 2ms 이내면 그대로 스핀 */
    }
}

static int worker_main(void *arg)
{
    struct worker_ctx *c = arg;
    struct mir_lcore_stats *st = &mir_stats[c->lcore_id];

    struct rte_mbuf *bufs[MIR_TX_BURST_MAX];
    const uint16_t   len     = c->pkts->frame_len;
    const uint32_t   burst   = c->burst;
    const int        offload = c->pkts->use_offload;
    const uint64_t   pace    = c->cycles_per_burst;
    const uint64_t   slack   = pace * RESYNC_SLACK_BURSTS;

    uint64_t ol_flags = 0;
    if (offload) {
        ol_flags = RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM;
        ol_flags |= (c->pkts->l4_proto == IPPROTO_TCP)
                        ? RTE_MBUF_F_TX_TCP_CKSUM
                        : RTE_MBUF_F_TX_UDP_CKSUM;
    }

    wait_until(c->start_at_ns, &g.stop);

    uint64_t next = rte_get_tsc_cycles();

    while (likely(!g.stop)) {
        uint64_t now = rte_get_tsc_cycles();

        if (c->end_tsc && now >= c->end_tsc)
            break;

        if (pace) {
            if (now < next)
                continue;                 /* 아직 이르다 — 스핀 */
            next += pace;
            if (unlikely(now > next + slack))
                next = now + pace;        /* 너무 밀렸다 — 폭주 대신 재동기 */
        }

        if (unlikely(rte_pktmbuf_alloc_bulk(c->pool, bufs, burst) != 0)) {
            /* 풀이 말랐다. NIC 이 아직 안 돌려준 것이므로 다음 회차에 다시 시도한다. */
            st->tx_drop += burst;
            continue;
        }

        for (uint32_t i = 0; i < burst; i++) {
            struct rte_mbuf *m = bufs[i];
            rte_memcpy(rte_pktmbuf_mtod(m, void *),
                       mir_pkt_variant(c->pkts, c->cursor + i), len);
            m->data_len = len;
            m->pkt_len  = len;

            if (offload) {
                m->ol_flags = ol_flags;
                m->l2_len   = c->pkts->l2_len;
                m->l3_len   = c->pkts->l3_len;
            }
        }
        c->cursor += burst;

        uint16_t sent = rte_eth_tx_burst(c->port_id, c->queue_id, bufs,
                                         (uint16_t)burst);
        st->tx_pkts  += sent;
        st->tx_bytes += (uint64_t)sent * len;

        if (unlikely(sent < burst)) {
            /* NIC 이 요구 속도를 못 따라간다. 남은 것은 버린다 —
             * 여기서 재시도하면 페이싱이 무너지고 지연이 누적된다. */
            st->tx_drop += burst - sent;
            for (uint32_t i = sent; i < burst; i++)
                rte_pktmbuf_free(bufs[i]);
        }
    }
    return 0;
}

/* ─────────────────────────────────────────────────────────── */

int mir_tx_start(const Mir__V1__StartScenarioRequest *req,
                 const mir_port *port,
                 const unsigned *lcores, size_t n_lcores,
                 char *err, size_t errlen)
{
    if (g.running) {
        seterr(err, errlen, "이미 시나리오 \"%s\" 가 실행 중이다 — 먼저 정지할 것",
               g.status.scenario_id);
        return -1;
    }
    if (!port || !port->started) {
        seterr(err, errlen, "포트가 준비되지 않았다");
        return -1;
    }
    if (req->pcap_path && *req->pcap_path) {
        seterr(err, errlen, "PCAP 리플레이는 Phase 4-1 이다 (packet 명세를 쓸 것)");
        return -1;
    }

    /* worker 로 쓸 lcore 를 고른다. main lcore(제어 스레드)와 RX lcore(상시
     * 수신 폴링)는 제외한다 — 둘 다 이미 다른 일로 그 코어를 점유한다. */
    unsigned main_lcore = rte_get_main_lcore();
    unsigned rx_lcore   = mir_rx_lcore();
    uint32_t avail = 0;
    for (size_t i = 0; i < n_lcores && avail < RTE_MAX_LCORE; i++) {
        if (lcores[i] == main_lcore || lcores[i] == rx_lcore)
            continue;
        g.worker_lcore[avail++] = lcores[i];
    }
    if (avail == 0) {
        seterr(err, errlen,
               "worker 로 쓸 lcore 가 없다 (cpuset 코어 %zu개 중 하나는 제어 "
               "스레드, 하나는 RX 폴링이 쓴다)", n_lcores);
        return -1;
    }

    uint32_t want = req->tx_lcores ? req->tx_lcores : avail;
    if (want > avail) {
        seterr(err, errlen, "tx_lcores=%u 를 요청했으나 사용 가능한 worker 는 %u개다",
               want, avail);
        return -1;
    }

    /* 포트가 만들어 둔 TX 큐보다 많은 worker 를 붙일 수 없다 —
     * 큐 하나를 둘이 만지는 순간 조용히 깨진다. */
    struct rte_eth_dev_info info;
    memset(&info, 0, sizeof(info));
    if (rte_eth_dev_info_get(port->port_id, &info) != 0) {
        seterr(err, errlen, "dev_info_get 실패");
        return -1;
    }
    if (want > port->n_tx_queues) {
        seterr(err, errlen,
               "worker %u개를 요청했으나 TX 큐는 %u개다 "
               "(큐 하나당 worker 하나여야 한다)", want, port->n_tx_queues);
        return -1;
    }

    int offload_ok =
        (info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM) &&
        (info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_TCP_CKSUM) &&
        (info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_UDP_CKSUM) &&
        port->tx_cksum_offload;

    struct rte_ether_addr mac;
    memset(&mac, 0, sizeof(mac));
    rte_eth_macaddr_get(port->port_id, &mac);

    if (mir_pkt_set_build(&g.pkts, req->packet, mac.addr_bytes,
                          offload_ok, err, errlen) != 0)
        return -1;

    if (g.pkts.requested_variants > g.pkts.n_variants) {
        LOG(WARNING,
            "가변 조합 %u개를 요청했으나 상한 %d 개로 잘랐다 — 일부 조합은 나오지 않는다",
            g.pkts.requested_variants, MIR_PKT_MAX_VARIANTS);
    }

    /* ── 속도 분배 ─────────────────────────────────────── */
    uint32_t burst = DEFAULT_BURST;
    uint64_t hz    = rte_get_tsc_hz();
    uint64_t pace  = 0;

    if (req->rate_pps) {
        /* worker 마다 균등 분배. 나머지는 버린다 — 요청 속도를 넘기지 않는
         * 쪽으로 기운다. */
        uint64_t per_worker = req->rate_pps / want;
        if (per_worker == 0) {
            seterr(err, errlen,
                   "rate_pps=%" PRIu64 " 를 worker %u개로 나누면 0 이 된다 "
                   "(tx_lcores 를 줄일 것)", req->rate_pps, want);
            mir_pkt_set_free(&g.pkts);
            return -1;
        }
        if (per_worker < burst)
            burst = (uint32_t)per_worker;   /* 저속에서 해상도를 유지한다 */
        pace = (hz * burst) / per_worker;
    }

    uint64_t end_tsc = 0;
    if (req->duration_s)
        end_tsc = rte_get_tsc_cycles() + (uint64_t)req->duration_s * hz;

    /* ── launch ────────────────────────────────────────── */
    g.stop = 0;
    g.n_workers = want;

    for (uint32_t i = 0; i < want; i++) {
        struct worker_ctx *c = &g.ctx[i];
        memset(c, 0, sizeof(*c));
        c->port_id          = port->port_id;
        c->queue_id         = (uint16_t)i;
        c->lcore_id         = g.worker_lcore[i];
        c->pkts             = &g.pkts;
        c->pool             = port->pool;
        c->burst            = burst;
        c->cycles_per_burst = pace;
        c->end_tsc          = end_tsc;
        c->start_at_ns      = req->start_at_ns;
        /* worker 마다 다른 지점에서 시작해 같은 순간 같은 플로우로 몰리지 않게 한다. */
        c->cursor           = i * burst;

        int rc = rte_eal_remote_launch(worker_main, c, c->lcore_id);
        if (rc != 0) {
            seterr(err, errlen, "lcore %u launch 실패 (%d) — 이미 다른 작업이 도는가",
                   c->lcore_id, rc);
            g.stop = 1;
            /* 이미 launch 된 worker 만 기다린다 (RX lcore 는 전체 대기에 걸린다). */
            for (uint32_t j = 0; j < i; j++)
                rte_eal_wait_lcore(g.worker_lcore[j]);
            mir_pkt_set_free(&g.pkts);
            return -1;
        }
    }

    memset(&g.status, 0, sizeof(g.status));
    snprintf(g.status.scenario_id, sizeof(g.status.scenario_id), "%s",
             req->scenario_id ? req->scenario_id : "");
    g.status.tx_lcores  = want;
    g.status.n_variants = g.pkts.n_variants;
    g.status.frame_len  = g.pkts.frame_len;
    g.status.rate_pps   = req->rate_pps;
    g.status.offload    = offload_ok;
    g.running = 1;

    LOG(INFO,
        "시작 \"%s\": worker=%u burst=%u frame=%uB 변형=%u rate=%" PRIu64
        "pps 체크섬=%s",
        g.status.scenario_id, want, burst, g.pkts.frame_len, g.pkts.n_variants,
        req->rate_pps, offload_ok ? "NIC" : "SW");
    return 0;
}

/* worker 가 전부 빠져나왔는지 본다. duration_s 만료로 스스로 끝난 경우를
 * 감지하기 위한 것이다 — 이때는 g.stop 을 아무도 세우지 않았으므로
 * g.running 만 보고는 "끝났다"를 알 수 없다.
 *
 * 이 함수와 mir_tx_start/stop 은 모두 IPC 단일 스레드에서만 불린다.
 * 그래서 g 에 대한 접근에 락이 필요 없다. */
static int all_workers_done(void)
{
    for (uint32_t i = 0; i < g.n_workers; i++) {
        if (rte_eal_get_lcore_state(g.worker_lcore[i]) == RUNNING)
            return 0;
    }
    return 1;
}

/* 실행이 끝난 시나리오의 자원을 회수한다. worker 는 이미 멈춰 있어야 한다.
 *
 * ★ rte_eal_mp_wait_lcore()(전체 대기)를 쓰면 안 된다. RX lcore 는 상시 폴링이라
 *   영원히 RUNNING 이고, 전체 대기는 그걸 기다리다 제어 스레드를 영구 블록한다.
 *   TX worker 만 개별로 기다린다. */
static void reap(void)
{
    for (uint32_t i = 0; i < g.n_workers; i++)
        rte_eal_wait_lcore(g.worker_lcore[i]);
    mir_pkt_set_free(&g.pkts);
    g.running = 0;
    memset(&g.status, 0, sizeof(g.status));
}

int mir_tx_stop(char *err, size_t errlen)
{
    (void)err; (void)errlen;

    if (!g.running)
        return 0;

    char id[sizeof(g.status.scenario_id)];
    snprintf(id, sizeof(id), "%s", g.status.scenario_id);

    g.stop = 1;
    reap();

    LOG(INFO, "정지 \"%s\"", id);
    return 0;
}

int mir_tx_status_get(mir_tx_status *out)
{
    if (!g.running)
        return 0;

    /* duration 만료로 스스로 끝났으면 여기서 거둔다. status 조회는 텔레메트리
     * 주기(100ms)마다 오므로, 늦어도 그 안에 running 상태가 풀린다. */
    if (all_workers_done()) {
        LOG(INFO, "시나리오 \"%s\" 종료 (duration 만료)", g.status.scenario_id);
        reap();
        return 0;
    }

    *out = g.status;
    return 1;
}

uint64_t mir_tx_drop_total(void)
{
    struct mir_stats_total t;
    mir_stats_sum(&t);
    return t.tx_drop;
}

void mir_tx_shutdown(void)
{
    char err[128];
    mir_tx_stop(err, sizeof(err));
}
