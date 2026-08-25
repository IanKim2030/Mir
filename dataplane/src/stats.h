/*
 * stats — worker lcore → 제어 스레드 카운터 전달 (채널 ①).
 *
 * lcore 마다 **전용** 구조체에 쓰기만 한다. 단일 writer 이므로 원자연산도
 * 락도 필요 없다. 제어 스레드는 100ms 마다 전체를 훑어 합산 스냅샷을 뜨는데,
 * 그 순간의 값이 조금 어긋나는 것은 텔레메트리에서 아무 문제가 아니다.
 *
 * ★ 캐시라인 정렬을 빠뜨리면 인접한 lcore 의 카운터가 같은 캐시라인에 얹혀
 *   false sharing 이 발생하고, 그것만으로 라인레이트가 무너진다. 이 파일에서
 *   가장 중요한 건 정렬 속성 한 줄이다.
 */
#ifndef MIR_STATS_H
#define MIR_STATS_H

#include <stdint.h>

#include <rte_config.h>
#include <rte_common.h>

/*
 * DPDK 의 __rte_cache_aligned 는 버전에 따라 매크로 위치가 바뀌었다
 * (속성 → alignas). 버전 간 이동에 흔들리지 않도록 컴파일러 속성을 직접 쓴다.
 */
#define MIR_CACHE_ALIGNED __attribute__((aligned(RTE_CACHE_LINE_SIZE)))

struct mir_lcore_stats {
    uint64_t tx_pkts;
    uint64_t tx_bytes;
    uint64_t tx_drop;
    uint64_t tx_err;

    uint64_t rx_pkts;
    uint64_t rx_bytes;
    uint64_t rx_drop;
    uint64_t rx_err;

    /* 수신 분류 (RX lcore 만 쓴다). TCP 플래그별 집계가 handshake 검증의
     * 1차 지표다 — 개별 응답 패킷은 경계를 넘지 않으므로 여기서 센다. */
    uint64_t rx_tcp_syn;
    uint64_t rx_tcp_syn_ack;
    uint64_t rx_tcp_rst;
    uint64_t rx_tcp_fin;
    uint64_t rx_tcp_ack;
    uint64_t rx_tcp_other;
    uint64_t rx_udp;
    uint64_t rx_non_ip;

    /* 이벤트 ring 이 가득 차 버린 개수.
     * 생산자는 ring 이 full 이어도 절대 블로킹하지 않고 여기만 올린다. */
    uint64_t event_drop;
} MIR_CACHE_ALIGNED;

/* main.c 에서 정의. lcore 는 자기 인덱스에만 쓴다. */
extern struct mir_lcore_stats mir_stats[RTE_MAX_LCORE];

/* 전 lcore 합산 결과 (포트별 분해는 Phase 2 에서 추가). */
struct mir_stats_total {
    uint64_t tx_pkts;
    uint64_t tx_bytes;
    uint64_t tx_drop;
    uint64_t tx_err;
    uint64_t rx_pkts;
    uint64_t rx_bytes;
    uint64_t rx_drop;
    uint64_t rx_err;

    uint64_t rx_tcp_syn;
    uint64_t rx_tcp_syn_ack;
    uint64_t rx_tcp_rst;
    uint64_t rx_tcp_fin;
    uint64_t rx_tcp_ack;
    uint64_t rx_tcp_other;
    uint64_t rx_udp;
    uint64_t rx_non_ip;

    uint64_t event_drop;
};

static inline void mir_stats_sum(struct mir_stats_total *out)
{
    struct mir_stats_total t = {0};

    for (unsigned i = 0; i < RTE_MAX_LCORE; i++) {
        const struct mir_lcore_stats *s = &mir_stats[i];
        t.tx_pkts        += s->tx_pkts;
        t.tx_bytes       += s->tx_bytes;
        t.tx_drop        += s->tx_drop;
        t.tx_err         += s->tx_err;
        t.rx_pkts        += s->rx_pkts;
        t.rx_bytes       += s->rx_bytes;
        t.rx_drop        += s->rx_drop;
        t.rx_err         += s->rx_err;
        t.rx_tcp_syn     += s->rx_tcp_syn;
        t.rx_tcp_syn_ack += s->rx_tcp_syn_ack;
        t.rx_tcp_rst     += s->rx_tcp_rst;
        t.rx_tcp_fin     += s->rx_tcp_fin;
        t.rx_tcp_ack     += s->rx_tcp_ack;
        t.rx_tcp_other   += s->rx_tcp_other;
        t.rx_udp         += s->rx_udp;
        t.rx_non_ip      += s->rx_non_ip;
        t.event_drop     += s->event_drop;
    }
    *out = t;
}

#endif /* MIR_STATS_H */
