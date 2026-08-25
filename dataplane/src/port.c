#define _GNU_SOURCE

#include "port.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_mbuf.h>

#define LOG(level, fmt, ...) \
    rte_log(RTE_LOG_ ## level, RTE_LOGTYPE_USER1, "port: " fmt "\n", ##__VA_ARGS__)

/*
 * 포트당 mbuf 개수. Phase 1 은 큐가 rx/tx 1개씩이라 디스크립터 링(보통 각
 * 1024)과 burst 여유만 덮으면 충분하다. Phase 2 에서 큐를 lcore 수만큼 벌릴 때
 * 이 값은 큐 수에 비례해 다시 계산해야 한다.
 */
#define NUM_MBUFS        8192
#define MBUF_CACHE_SIZE  256

#define RX_DESC_DEFAULT  1024
#define TX_DESC_DEFAULT  1024

#define LINK_POLL_MS     100

static void seterr(char *err, size_t errlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void seterr(char *err, size_t errlen, const char *fmt, ...)
{
    if (!err || errlen == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

int mir_port_setup(uint16_t port_id, mir_port *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof(*out));
    out->port_id = port_id;

    struct rte_eth_dev_info info;
    memset(&info, 0, sizeof(info));
    int rc = rte_eth_dev_info_get(port_id, &info);
    if (rc != 0) {
        seterr(err, errlen, "dev_info_get: %s", rte_strerror(-rc));
        return -1;
    }

    /* socket_id 가 -1 로 오는 환경(가상 장치, NUMA 정보 미노출)이 흔하다.
     * 그대로 넘기면 mempool 생성이 실패하므로 현재 lcore 의 노드로 떨어뜨린다. */
    int socket_id = rte_eth_dev_socket_id(port_id);
    if (socket_id < 0)
        socket_id = (int)rte_socket_id();
    out->socket_id = socket_id;

    /* ── mempool ──────────────────────────────────────────────── */
    char pool_name[RTE_MEMPOOL_NAMESIZE];
    snprintf(pool_name, sizeof(pool_name), "mir_mp_p%u", port_id);

    out->pool = rte_pktmbuf_pool_create(pool_name, NUM_MBUFS, MBUF_CACHE_SIZE,
                                        0, RTE_MBUF_DEFAULT_BUF_SIZE, socket_id);
    if (!out->pool) {
        seterr(err, errlen, "mempool 생성 실패(%s): %s",
               pool_name, rte_strerror(rte_errno));
        return -1;
    }

    /* ── 포트 구성 ────────────────────────────────────────────── */
    struct rte_eth_conf conf;
    memset(&conf, 0, sizeof(conf));
    conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
    conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;
    conf.rxmode.mtu     = RTE_ETHER_MTU;

    /* FAST_FREE 는 "이 큐에서 나간 mbuf 는 전부 같은 풀 소속이고 세그먼트가
     * 하나"라는 가정 위에서 TX 완료 처리를 크게 줄인다. Phase 1 의 hello 는 그
     * 조건을 만족하지만, Phase 4-1 리플레이가 다중 세그먼트 mbuf 를 쓰기
     * 시작하면 이 플래그를 반드시 다시 검토해야 한다. */
    if (info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
        conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

    rc = rte_eth_dev_configure(port_id, 1, 1, &conf);
    if (rc != 0) {
        seterr(err, errlen, "dev_configure: %s", rte_strerror(-rc));
        goto fail;
    }

    uint16_t nb_rxd = RX_DESC_DEFAULT;
    uint16_t nb_txd = TX_DESC_DEFAULT;
    rc = rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &nb_rxd, &nb_txd);
    if (rc != 0) {
        seterr(err, errlen, "adjust_nb_rx_tx_desc: %s", rte_strerror(-rc));
        goto fail;
    }

    /* 큐 설정은 드라이버 기본값에서 출발해 포트 offload 만 얹는다.
     * 기본 threshold 는 PMD 마다 다르고, 임의로 덮으면 특정 NIC 에서만
     * 성능이 무너지는 종류의 문제가 된다. */
    struct rte_eth_rxconf rxconf = info.default_rxconf;
    rxconf.offloads = conf.rxmode.offloads;
    rc = rte_eth_rx_queue_setup(port_id, 0, nb_rxd, (unsigned)socket_id,
                                &rxconf, out->pool);
    if (rc != 0) {
        seterr(err, errlen, "rx_queue_setup: %s", rte_strerror(-rc));
        goto fail;
    }

    struct rte_eth_txconf txconf = info.default_txconf;
    txconf.offloads = conf.txmode.offloads;
    rc = rte_eth_tx_queue_setup(port_id, 0, nb_txd, (unsigned)socket_id, &txconf);
    if (rc != 0) {
        seterr(err, errlen, "tx_queue_setup: %s", rte_strerror(-rc));
        goto fail;
    }

    rc = rte_eth_dev_start(port_id);
    if (rc != 0) {
        seterr(err, errlen, "dev_start: %s", rte_strerror(-rc));
        goto fail;
    }
    out->started = 1;

    /* 이 도구는 자기 MAC 이 아닌 프레임까지 받아야 판정할 수 있다(Phase 3).
     * 지원하지 않는 PMD 도 있으므로 실패는 경고로만 남긴다. */
    rc = rte_eth_promiscuous_enable(port_id);
    if (rc != 0)
        LOG(WARNING, "port %u: promiscuous 활성 실패(%s) — 수신 범위가 제한된다",
            port_id, rte_strerror(-rc));

    LOG(INFO, "port %u 준비 완료: rxq=1(%u desc) txq=1(%u desc) socket=%d pool=%s",
        port_id, nb_rxd, nb_txd, socket_id, pool_name);
    return 0;

fail:
    if (out->pool) {
        rte_mempool_free(out->pool);
        out->pool = NULL;
    }
    out->started = 0;
    return -1;
}

void mir_port_close(mir_port *p)
{
    if (p->started) {
        int rc = rte_eth_dev_stop(p->port_id);
        if (rc != 0)
            LOG(WARNING, "port %u: dev_stop 실패: %s", p->port_id, rte_strerror(-rc));

        rc = rte_eth_dev_close(p->port_id);
        if (rc != 0)
            LOG(WARNING, "port %u: dev_close 실패: %s", p->port_id, rte_strerror(-rc));

        p->started = 0;
        LOG(INFO, "port %u 정리 완료", p->port_id);
    }

    if (p->pool) {
        rte_mempool_free(p->pool);
        p->pool = NULL;
    }
}

/*
 * 링크 상태를 사람이 읽을 문자열로 만든다.
 *
 * DPDK 의 rte_eth_link_to_str() 을 쓰지 않는 이유: 아직 실험적 API 라
 * ABI 가 고정되지 않았다(-Wdeprecated-declarations 경고가 나온다).
 * 로그 한 줄을 위해 버전 간에 깨질 수 있는 심볼에 묶일 이유가 없다.
 */
static void format_link(char *buf, size_t buflen, const struct rte_eth_link *link)
{
    if (link->link_status != RTE_ETH_LINK_UP) {
        snprintf(buf, buflen, "Link down");
        return;
    }

    char speed[32];
    if (link->link_speed == RTE_ETH_SPEED_NUM_UNKNOWN)
        snprintf(speed, sizeof(speed), "Unknown speed");
    else if (link->link_speed % 1000 == 0)
        snprintf(speed, sizeof(speed), "%u Gbps", link->link_speed / 1000);
    else
        snprintf(speed, sizeof(speed), "%u Mbps", link->link_speed);

    snprintf(buf, buflen, "Link up at %s %s %s", speed,
             link->link_duplex == RTE_ETH_LINK_FULL_DUPLEX ? "FDX" : "HDX",
             link->link_autoneg == RTE_ETH_LINK_AUTONEG ? "Autoneg" : "Fixed");
}

int mir_port_wait_link(uint16_t port_id, unsigned timeout_ms,
                       struct rte_eth_link *out)
{
    struct rte_eth_link link;
    unsigned waited = 0;

    for (;;) {
        memset(&link, 0, sizeof(link));
        int rc = rte_eth_link_get_nowait(port_id, &link);
        if (rc != 0) {
            LOG(WARNING, "port %u: link_get 실패: %s", port_id, rte_strerror(-rc));
            return -1;
        }

        if (link.link_status == RTE_ETH_LINK_UP)
            break;
        if (waited >= timeout_ms)
            break;

        struct timespec ts = {
            .tv_sec  = 0,
            .tv_nsec = (long)LINK_POLL_MS * 1000 * 1000,
        };
        nanosleep(&ts, NULL);
        waited += LINK_POLL_MS;
    }

    if (out)
        *out = link;

    char desc[128];
    format_link(desc, sizeof(desc), &link);
    LOG(INFO, "port %u: %s", port_id, desc);

    return link.link_status == RTE_ETH_LINK_UP ? 1 : 0;
}
