#define _GNU_SOURCE

#include "replay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_branch_prediction.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_memcpy.h>

#include "pcap_reader.h"
#include "rx_engine.h"
#include "stats.h"

#define LOG(level, fmt, ...) \
    rte_log(RTE_LOG_##level, RTE_LOGTYPE_USER1, "replay: " fmt "\n", ##__VA_ARGS__)

#define seterr(err, len, ...) \
    do { if ((err) && (len)) snprintf((err), (len), __VA_ARGS__); } while (0)

/* 메모리 상한. 리플레이는 파일을 통째로 올리므로 상한이 없으면 큰 캡처가
 * hugepage 밖 힙을 무제한으로 먹는다. 넘으면 그 지점까지만 싣고 경고한다. */
#define REPLAY_MAX_FRAMES     (4u * 1000u * 1000u)
#define REPLAY_MAX_BLOB_BYTES (1ull << 30)   /* 1 GiB */

/* 재생 큐. 모드 A/B 와 배타라 worker 큐 0 을 그대로 쓴다. */
#define REPLAY_TX_QUEUE 0

/* 한 프레임의 위치·길이·원본 타임스탬프. data 는 blob 안 오프셋. */
struct rep_frame {
    uint32_t off;
    uint16_t len;
    uint64_t ts_ns;
};

struct replay_ctx {
    uint16_t port_id;
    unsigned lcore_id;
    struct rte_mempool *pool;

    const uint8_t   *blob;    /* 프레임 바이트 저장소 */
    const struct rep_frame *frames;
    uint32_t         first;   /* 재생 시작 인덱스(포함) */
    uint32_t         last;    /* 재생 끝 인덱스(포함) */
    uint32_t         loops;   /* 재생 횟수. 0 = 1회(proto 규약) */
    int              preserve_timing;
    double           speed;
};

static struct {
    int          running;
    volatile int stop;

    uint8_t          *blob;
    size_t            blob_len;
    struct rep_frame *frames;
    uint32_t          n_frames;

    struct replay_ctx ctx;
    unsigned          lcore;

    mir_replay_status status;
} g;

/* ─────────────────────────────────────────────────────────── */

static void store_free(void)
{
    free(g.blob);
    free(g.frames);
    g.blob     = NULL;
    g.frames   = NULL;
    g.blob_len = 0;
    g.n_frames = 0;
}

/*
 * pcap 을 통째로 읽어 Ethernet 프레임만 메모리에 올린다.
 * 링크타입이 Ethernet 이 아니거나 mbuf 에 안 들어갈 만큼 큰 프레임은 건너뛴다.
 * 0 = 성공(n_skipped 로 건너뛴 수 보고), -1 = 실패.
 */
static int load_pcap(const char *path, uint16_t max_frame,
                     uint32_t *n_skipped, char *err, size_t errlen)
{
    char rerr[192] = {0};
    pcap_reader *r = pcap_reader_open(path, rerr, sizeof(rerr));
    if (!r) {
        seterr(err, errlen, "pcap 열기 실패: %s", rerr);
        return -1;
    }

    snprintf(g.status.format, sizeof(g.status.format), "%s",
             pcap_reader_format(r));

    size_t   blob_cap  = 1u << 20;   /* 1 MiB 부터 배로 늘린다 */
    uint32_t frame_cap = 4096;
    g.blob   = malloc(blob_cap);
    g.frames = malloc(frame_cap * sizeof(*g.frames));
    if (!g.blob || !g.frames) {
        seterr(err, errlen, "메모리 부족");
        store_free();
        pcap_reader_close(r);
        return -1;
    }

    uint32_t skipped = 0;
    int truncated = 0;
    pcap_packet pkt;
    int rc;
    while ((rc = pcap_reader_next(r, &pkt)) == 1) {
        if (pkt.linktype != LINKTYPE_ETHERNET || pkt.caplen == 0 ||
            pkt.caplen > max_frame) {
            skipped++;
            continue;
        }

        if (g.n_frames >= REPLAY_MAX_FRAMES ||
            g.blob_len + pkt.caplen > REPLAY_MAX_BLOB_BYTES) {
            truncated = 1;
            break;
        }

        if (g.blob_len + pkt.caplen > blob_cap) {
            size_t want = blob_cap;
            while (g.blob_len + pkt.caplen > want)
                want *= 2;
            uint8_t *nb = realloc(g.blob, want);
            if (!nb) { seterr(err, errlen, "메모리 부족(blob)"); goto fail; }
            g.blob = nb;
            blob_cap = want;
        }
        if (g.n_frames >= frame_cap) {
            uint32_t want = frame_cap * 2;
            struct rep_frame *nf = realloc(g.frames, want * sizeof(*nf));
            if (!nf) { seterr(err, errlen, "메모리 부족(index)"); goto fail; }
            g.frames = nf;
            frame_cap = want;
        }

        memcpy(g.blob + g.blob_len, pkt.data, pkt.caplen);
        g.frames[g.n_frames].off   = (uint32_t)g.blob_len;
        g.frames[g.n_frames].len   = (uint16_t)pkt.caplen;
        g.frames[g.n_frames].ts_ns = pkt.ts_ns;
        g.n_frames++;
        g.blob_len += pkt.caplen;
    }

    if (rc < 0) {
        seterr(err, errlen, "pcap 파싱 오류: %s", pcap_reader_error(r));
        goto fail;
    }
    pcap_reader_close(r);

    if (truncated)
        LOG(WARNING, "상한 초과로 %u 프레임(%zuB)에서 잘라 실었다 — 구간(first/last)으로 나눠 보낼 것",
            g.n_frames, g.blob_len);

    if (g.n_frames == 0) {
        seterr(err, errlen, "재생할 Ethernet 프레임이 없다 (건너뜀 %u)", skipped);
        store_free();
        return -1;
    }

    *n_skipped = skipped;
    return 0;

fail:
    pcap_reader_close(r);
    store_free();
    return -1;
}

/* ─────────────────────────────────────────────────────────── */

static int worker_main(void *arg)
{
    struct replay_ctx *c = arg;
    struct mir_lcore_stats *st = &mir_stats[c->lcore_id];

    const uint64_t hz    = rte_get_tsc_hz();
    const double   speed = (c->speed > 0.0) ? c->speed : 1.0;

    /* proto 규약: loop=0 은 1회. 반복 횟수는 항상 유한하다(무한 옵션 없음). */
    const uint32_t plays = c->loops ? c->loops : 1;

    uint32_t loops_done = 0;
    while (likely(!g.stop)) {
        uint64_t due = rte_get_tsc_cycles();
        uint64_t prev_ts = 0;
        int have_prev = 0;

        for (uint32_t i = c->first; i <= c->last && !g.stop; i++) {
            const struct rep_frame *f = &c->frames[i];

            if (c->preserve_timing) {
                if (have_prev && f->ts_ns >= prev_ts) {
                    uint64_t gap_ns = f->ts_ns - prev_ts;
                    /* 배속 적용. speed=2 → 간격 절반(두 배 빠르게). */
                    uint64_t adj = (uint64_t)((double)gap_ns / speed);
                    due += (uint64_t)(((__uint128_t)adj * hz) / 1000000000ull);
                    while (rte_get_tsc_cycles() < due) {
                        if (g.stop)
                            return 0;
                    }
                }
                prev_ts  = f->ts_ns;
                /* ts_ns==0 은 "타임스탬프 없음"(SPB)으로 본다 — reader 가 그렇게
                 * 정규화한다. 그런 프레임 다음 구간은 페이싱하지 않는다(안 그러면
                 * 0 과 큰 에폭의 차이만큼 거대한 대기가 생긴다). 실제 캡처의
                 * 에폭은 항상 큰 값이라 이 센티넬에 걸리지 않는다. */
                have_prev = (f->ts_ns != 0);
            }

            struct rte_mbuf *m = rte_pktmbuf_alloc(c->pool);
            if (unlikely(!m)) {
                /* 풀이 말랐다 — NIC 이 아직 안 돌려줬다. 잠깐 돌며 재시도. */
                st->tx_drop++;
                while (!(m = rte_pktmbuf_alloc(c->pool))) {
                    if (g.stop)
                        return 0;
                }
            }

            rte_memcpy(rte_pktmbuf_mtod(m, void *),
                       c->blob + f->off, f->len);
            m->data_len = f->len;
            m->pkt_len  = f->len;

            uint16_t sent = rte_eth_tx_burst(c->port_id, REPLAY_TX_QUEUE, &m, 1);
            if (likely(sent == 1)) {
                st->tx_pkts++;
                st->tx_bytes += f->len;
            } else {
                /* NIC 이 못 받았다. 순서 보존이 본질이라 재시도로 밀어 넣는다. */
                while (rte_eth_tx_burst(c->port_id, REPLAY_TX_QUEUE, &m, 1) == 0) {
                    if (g.stop) {
                        rte_pktmbuf_free(m);
                        return 0;
                    }
                }
                st->tx_pkts++;
                st->tx_bytes += f->len;
            }
        }

        loops_done++;
        if (loops_done >= plays)
            break;
    }
    return 0;
}

/* ─────────────────────────────────────────────────────────── */

int mir_replay_start(const Mir__V1__StartScenarioRequest *req,
                     const mir_port *port,
                     const unsigned *lcores, size_t n_lcores,
                     char *err, size_t errlen)
{
    if (g.running) {
        seterr(err, errlen, "이미 리플레이 \"%s\" 가 실행 중이다 — 먼저 정지할 것",
               g.status.scenario_id);
        return -1;
    }
    if (!port || !port->started) {
        seterr(err, errlen, "포트가 준비되지 않았다");
        return -1;
    }
    if (!req->pcap_path || !*req->pcap_path) {
        seterr(err, errlen, "pcap_path 가 비었다");
        return -1;
    }

    /* worker 로 쓸 lcore 하나를 고른다 (main·RX 제외). */
    unsigned main_lcore = rte_get_main_lcore();
    unsigned rx_lcore   = mir_rx_lcore();
    unsigned worker = RTE_MAX_LCORE;
    for (size_t i = 0; i < n_lcores; i++) {
        if (lcores[i] == main_lcore || lcores[i] == rx_lcore)
            continue;
        worker = lcores[i];
        break;
    }
    if (worker == RTE_MAX_LCORE) {
        seterr(err, errlen,
               "재생에 쓸 worker lcore 가 없다 (코어 %zu개 중 하나는 제어, 하나는 RX)",
               n_lcores);
        return -1;
    }
    if (rte_eal_get_lcore_state(worker) == RUNNING) {
        seterr(err, errlen, "worker lcore %u 가 이미 다른 작업 중이다", worker);
        return -1;
    }

    /* 프레임이 mbuf 에 들어가는지 판정할 상한. */
    uint16_t room = rte_pktmbuf_data_room_size(port->pool);
    uint16_t max_frame = (room > RTE_PKTMBUF_HEADROOM)
                             ? (uint16_t)(room - RTE_PKTMBUF_HEADROOM) : 0;

    memset(&g.status, 0, sizeof(g.status));
    uint32_t skipped = 0;
    if (load_pcap(req->pcap_path, max_frame, &skipped, err, errlen) != 0)
        return -1;

    /* 구간 계산 (0-based, 로드된 Ethernet 프레임 기준). */
    const Mir__V1__ReplayOpts *ro = req->replay;
    uint32_t first = ro ? ro->first_pkt : 0;
    uint32_t last  = (ro && ro->last_pkt) ? ro->last_pkt : g.n_frames - 1;
    if (first >= g.n_frames) {
        seterr(err, errlen, "first_pkt=%u 가 프레임 수 %u 를 넘는다", first, g.n_frames);
        store_free();
        return -1;
    }
    if (last >= g.n_frames)
        last = g.n_frames - 1;
    if (last < first) {
        seterr(err, errlen, "구간이 비었다 (first=%u > last=%u)", first, last);
        store_free();
        return -1;
    }

    /* ── ctx 구성 ─────────────────────────────────────── */
    struct replay_ctx *c = &g.ctx;
    memset(c, 0, sizeof(*c));
    c->port_id         = port->port_id;
    c->lcore_id        = worker;
    c->pool            = port->pool;
    c->blob            = g.blob;
    c->frames          = g.frames;
    c->first           = first;
    c->last            = last;
    c->loops           = ro ? ro->loop : 0;
    c->preserve_timing = ro ? ro->preserve_timing : 0;
    c->speed           = ro ? ro->speed : 0.0;

    g.stop  = 0;
    g.lcore = worker;

    int rc = rte_eal_remote_launch(worker_main, c, worker);
    if (rc != 0) {
        seterr(err, errlen, "lcore %u launch 실패 (%d)", worker, rc);
        store_free();
        return -1;
    }

    snprintf(g.status.scenario_id, sizeof(g.status.scenario_id), "%s",
             req->scenario_id ? req->scenario_id : "");
    snprintf(g.status.pcap_path, sizeof(g.status.pcap_path), "%s", req->pcap_path);
    g.status.lcore           = worker;
    g.status.n_frames        = last - first + 1;
    g.status.n_skipped       = skipped;
    g.status.loops_target    = c->loops ? c->loops : 1;
    g.status.preserve_timing = c->preserve_timing;
    g.running = 1;

    LOG(INFO,
        "시작 \"%s\": %s 프레임 %u개 구간[%u..%u] 재생=%u회 timing=%s speed=%.2f 건너뜀=%u lcore=%u",
        g.status.scenario_id, g.status.format, g.status.n_frames, first, last,
        g.status.loops_target, c->preserve_timing ? "보존" : "최대", c->speed,
        skipped, worker);
    return 0;
}

/* worker 를 거두고 자원을 해제한다. worker 는 이미 멈춰 있어야 한다.
 * ★ rte_eal_mp_wait_lcore() 금지 — RX lcore 는 영원히 RUNNING 이다. */
static void reap(void)
{
    rte_eal_wait_lcore(g.lcore);
    store_free();
    g.running = 0;
    memset(&g.status, 0, sizeof(g.status));
}

int mir_replay_stop(char *err, size_t errlen)
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

int mir_replay_status_get(mir_replay_status *out)
{
    if (!g.running)
        return 0;

    /* 유한 loop 가 스스로 끝났으면 거둔다 (status 조회는 텔레메트리 주기마다 온다). */
    if (rte_eal_get_lcore_state(g.lcore) != RUNNING) {
        LOG(INFO, "리플레이 \"%s\" 종료 (loop 완료)", g.status.scenario_id);
        reap();
        return 0;
    }

    *out = g.status;
    return 1;
}

void mir_replay_shutdown(void)
{
    char err[128];
    mir_replay_stop(err, sizeof(err));
}
