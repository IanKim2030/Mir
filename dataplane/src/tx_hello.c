#define _GNU_SOURCE

#include "tx_hello.h"
#include "stats.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_mbuf.h>

#define LOG(level, fmt, ...) \
    rte_log(RTE_LOG_ ## level, RTE_LOGTYPE_USER1, "tx_hello: " fmt "\n", ##__VA_ARGS__)

#define HELLO_BURST_MAX      512
#define HELLO_DEFAULT_SIZE   64
#define HELLO_DEFAULT_BURST  32

/*
 * tx_burst 가 0 을 돌려줄 때 몇 번까지 다시 밀어볼 것인가.
 *
 * TX 디스크립터 회수가 늦어 일시적으로 큐가 찬 상황과, 링크가 내려가 영영
 * 나가지 않는 상황을 구분할 방법이 여기서는 없다. 무한 재시도는 후자에서
 * 프로세스를 멈추게 하므로 반드시 상한을 둔다.
 */
#define TX_RETRY_MAX         64

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

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* 환경변수를 부호 없는 정수로. 미설정이면 def, 파싱 실패면 -1 반환. */
static long env_ulong(const char *name, long def)
{
    const char *v = getenv(name);
    if (!v || !*v)
        return def;

    char *end = NULL;
    long  n   = strtol(v, &end, 10);
    if (*end != '\0' || n < 0)
        return -1;
    return n;
}

int mir_hello_conf_from_env(mir_hello_conf *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof(*out));

    long count = env_ulong("MIR_HELLO_TX_COUNT", 0);
    if (count < 0) {
        seterr(err, errlen, "MIR_HELLO_TX_COUNT 값이 잘못됐다");
        return -1;
    }
    if (count == 0)
        return 0;   /* 비활성 — 기본값 */

    long port = env_ulong("MIR_HELLO_TX_PORT", 0);
    if (port < 0 || port > UINT16_MAX) {
        seterr(err, errlen, "MIR_HELLO_TX_PORT 값이 잘못됐다");
        return -1;
    }

    long size = env_ulong("MIR_HELLO_PKT_SIZE", HELLO_DEFAULT_SIZE);
    if (size < 0) {
        seterr(err, errlen, "MIR_HELLO_PKT_SIZE 값이 잘못됐다");
        return -1;
    }
    if (size < (long)MIR_HELLO_MIN_SIZE)
        size = (long)MIR_HELLO_MIN_SIZE;
    if (size > MIR_HELLO_MAX_SIZE)
        size = MIR_HELLO_MAX_SIZE;

    long burst = env_ulong("MIR_HELLO_BURST", HELLO_DEFAULT_BURST);
    if (burst <= 0) {
        seterr(err, errlen, "MIR_HELLO_BURST 값이 잘못됐다");
        return -1;
    }
    if (burst > HELLO_BURST_MAX)
        burst = HELLO_BURST_MAX;

    struct rte_ether_addr dst;
    const char *mac = getenv("MIR_HELLO_DST_MAC");
    if (mac && *mac) {
        if (rte_ether_unformat_addr(mac, &dst) != 0) {
            seterr(err, errlen, "MIR_HELLO_DST_MAC 파싱 실패: %s", mac);
            return -1;
        }
    } else {
        memset(&dst, 0xff, sizeof(dst));   /* 브로드캐스트 */
    }

    out->port_id  = (uint16_t)port;
    out->dst      = dst;
    out->pkt_size = (uint16_t)size;
    out->count    = (uint32_t)count;
    out->burst    = (uint16_t)burst;
    return 1;
}

static void fill_packet(struct rte_mbuf *m, const mir_hello_conf *cfg,
                        const struct rte_ether_addr *src, uint32_t seq)
{
    char *data = rte_pktmbuf_append(m, cfg->pkt_size);
    /* 풀의 data room 이 pkt_size 보다 크다는 것은 호출 전에 보장된다. */
    memset(data, 0, cfg->pkt_size);

    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)data;
    rte_ether_addr_copy(&cfg->dst, &eth->dst_addr);
    rte_ether_addr_copy(src, &eth->src_addr);
    eth->ether_type = rte_cpu_to_be_16(MIR_HELLO_ETHERTYPE);

    struct mir_hello_payload *p =
        (struct mir_hello_payload *)(data + sizeof(struct rte_ether_hdr));
    memcpy(p->magic, MIR_HELLO_MAGIC, sizeof(p->magic));
    p->seq   = rte_cpu_to_be_32(seq);
    p->ts_ns = rte_cpu_to_be_64(now_ns());

    /* 남는 자리는 'M','I','R' 반복으로 채운다. hexdump 로 바로 읽히고,
     * 프레임이 잘리거나 패딩이 섞이면 눈에 띈다. */
    static const char pat[3] = { 'M', 'I', 'R' };
    const size_t head = sizeof(struct rte_ether_hdr) + sizeof(*p);
    for (size_t i = head; i < (size_t)cfg->pkt_size; i++)
        data[i] = pat[(i - head) % sizeof(pat)];
}

int mir_tx_hello(const mir_hello_conf *cfg, struct rte_mempool *pool,
                 uint32_t *sent, uint32_t *dropped, char *err, size_t errlen)
{
    *sent    = 0;
    *dropped = 0;

    if (!pool) {
        seterr(err, errlen, "port %u: mempool 이 없다 (포트 구성 실패)", cfg->port_id);
        return -1;
    }
    if (!rte_eth_dev_is_valid_port(cfg->port_id)) {
        seterr(err, errlen, "port %u 가 유효하지 않다", cfg->port_id);
        return -1;
    }

    /* mbuf 하나에 프레임 전체가 들어가야 한다. Phase 1 은 세그먼트 체인을
     * 쓰지 않으므로 여기서 걸러 두는 편이 tx 실패로 겪는 것보다 낫다. */
    uint16_t room = rte_pktmbuf_data_room_size(pool);
    if (cfg->pkt_size + RTE_PKTMBUF_HEADROOM > room) {
        seterr(err, errlen, "pkt_size %u 가 mbuf data room(%u) 을 넘는다",
               cfg->pkt_size, room);
        return -1;
    }

    struct rte_ether_addr src;
    memset(&src, 0, sizeof(src));
    int rc = rte_eth_macaddr_get(cfg->port_id, &src);
    if (rc != 0) {
        seterr(err, errlen, "macaddr_get: %s", rte_strerror(-rc));
        return -1;
    }

    /* 비-EAL 스레드에서 부르면 rte_lcore_id() 가 LCORE_ID_ANY(UINT_MAX)다.
     * 그대로 인덱스로 쓰면 배열 밖을 밟으므로 통계만 포기한다. */
    unsigned lcore = rte_lcore_id();
    struct mir_lcore_stats *st =
        lcore < RTE_MAX_LCORE ? &mir_stats[lcore] : NULL;

    /* bufs 는 스택 배열이다. 환경변수 경로는 이미 클램프하지만, 이 함수를
     * 다른 곳에서 부를 때를 대비해 여기서도 상한을 건다. */
    uint16_t burst = cfg->burst;
    if (burst == 0)
        burst = 1;
    if (burst > HELLO_BURST_MAX)
        burst = HELLO_BURST_MAX;

    struct rte_mbuf *bufs[HELLO_BURST_MAX];
    uint32_t         seq       = 0;
    uint32_t         remaining = cfg->count;

    while (remaining > 0) {
        uint16_t n = burst < remaining ? burst : (uint16_t)remaining;

        if (rte_pktmbuf_alloc_bulk(pool, bufs, n) != 0) {
            seterr(err, errlen, "mbuf %u개 할당 실패 (풀 고갈)", n);
            return -1;
        }

        for (uint16_t i = 0; i < n; i++)
            fill_packet(bufs[i], cfg, &src, seq++);

        uint16_t off     = 0;
        int      retries = 0;
        while (off < n) {
            uint16_t tx = rte_eth_tx_burst(cfg->port_id, 0, &bufs[off],
                                           (uint16_t)(n - off));
            if (tx == 0) {
                if (++retries > TX_RETRY_MAX)
                    break;
                rte_delay_us_block(100);
                continue;
            }
            retries = 0;
            off    += tx;

            if (st) {
                st->tx_pkts  += tx;
                st->tx_bytes += (uint64_t)tx * cfg->pkt_size;
            }
            *sent += tx;
        }

        /* 큐가 끝내 받아주지 않은 mbuf 는 **송신자가** 해제해야 한다.
         * tx_burst 가 반환한 개수까지만 소유권이 넘어간다. */
        if (off < n) {
            uint16_t left = (uint16_t)(n - off);
            rte_pktmbuf_free_bulk(&bufs[off], left);
            *dropped += left;
            if (st)
                st->tx_drop += left;

            LOG(WARNING, "port %u: %u개를 큐에 넣지 못했다 — 링크 down 이거나 "
                "TX 디스크립터 회수가 막혀 있다", cfg->port_id, left);
            break;   /* 계속 밀어봐야 같은 결과다 */
        }

        remaining -= n;
    }

    return 0;
}
