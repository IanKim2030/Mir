/*
 * tx_hello — Phase 1 "hello packet".
 *
 * 목적은 트래픽 생성이 아니라 **경로 증명**이다. mbuf 를 할당해 이더넷 프레임을
 * 조립하고 rte_eth_tx_burst 로 내보내는 데까지가 한 번도 실물에서 확인된 적이
 * 없으므로, 그 최소 경로만 떼어 검증 가능한 형태로 만든다.
 *
 * 프레임은 EtherType **0x88B5** 를 쓴다. IEEE 가 프로토콜 표준화 없이 쓰라고
 * 배정해 둔 로컬 실험용 값이라, 이 프레임이 실수로 스위치의 다른 처리 경로를
 * 타거나 상대 스택이 IPv4 로 파싱하는 일이 없다. tcpdump 에서도 한눈에 걸린다.
 *
 * L2~L4 정식 헤더 빌더는 Phase 2 다. 여기서 IP/TCP 를 흉내내면 검증되지 않은
 * 체크섬·오프로드 경로가 딸려 들어와 "무엇이 증명됐는지"가 흐려진다.
 */
#ifndef MIR_TX_HELLO_H
#define MIR_TX_HELLO_H

#include <stddef.h>
#include <stdint.h>

#include <rte_ether.h>
#include <rte_mempool.h>

#define MIR_HELLO_ETHERTYPE 0x88B5   /* IEEE Std 802 - Local Experimental 1 */
#define MIR_HELLO_MAGIC     "MIR1"

/* 페이로드 선두 16바이트. 수신측(Phase 3)이 이 프레임을 식별하는 근거가 된다.
 * 모든 정수는 네트워크 바이트 오더. */
struct mir_hello_payload {
    char     magic[4];   /* "MIR1" */
    uint32_t seq;        /* 0부터 증가 */
    uint64_t ts_ns;      /* 송신 시각 (CLOCK_REALTIME) */
} __attribute__((packed));

#define MIR_HELLO_MIN_SIZE  (sizeof(struct rte_ether_hdr) + \
                             sizeof(struct mir_hello_payload))

/* pkt_size 는 FCS 를 제외한 길이다. RTE_ETHER_MAX_LEN(1518)은 FCS 를 포함한
 * 값이라 그대로 쓰면 MTU 1500 설정과 4바이트 어긋나 TX 가 거부된다. */
#define MIR_HELLO_MAX_SIZE  (RTE_ETHER_MAX_LEN - RTE_ETHER_CRC_LEN)

typedef struct {
    uint16_t              port_id;
    struct rte_ether_addr dst;       /* 기본값 = 브로드캐스트 */
    uint16_t              pkt_size;  /* FCS 를 제외한 프레임 길이 */
    uint32_t              count;     /* 보낼 패킷 수 */
    uint16_t              burst;     /* tx_burst 한 번에 넣을 개수 */
} mir_hello_conf;

/*
 * 환경변수에서 설정을 읽는다.
 *   MIR_HELLO_TX_COUNT  보낼 패킷 수. 미설정/0 이면 hello 송신을 하지 않는다
 *   MIR_HELLO_TX_PORT   대상 포트 id (기본 0)
 *   MIR_HELLO_DST_MAC   목적지 MAC "aa:bb:cc:dd:ee:ff" (기본 브로드캐스트)
 *   MIR_HELLO_PKT_SIZE  프레임 길이 (기본 64, MIR_HELLO_MIN_SIZE~1518 로 클램프)
 *   MIR_HELLO_BURST     burst 크기 (기본 32, 1~512)
 *
 * 1 = 송신하도록 설정됨, 0 = 비활성, -1 = 설정 오류(err 에 사유).
 */
int mir_hello_conf_from_env(mir_hello_conf *out, char *err, size_t errlen);

/*
 * cfg->count 개의 hello 프레임을 동기적으로 송신한다. 호출한 스레드에서
 * 그대로 실행되므로 반드시 lcore 에 고정된 스레드에서 부를 것.
 *
 * 송신에 성공한 개수를 *sent 에, 큐가 받아주지 않아 버린 개수를 *dropped 에
 * 채운다. 반환값 0 = 정상 종료(부분 송신 포함), -1 = 시작조차 못 함.
 *
 * per-lcore 카운터(mir_stats)도 함께 갱신하므로 텔레메트리에 그대로 반영된다.
 */
int mir_tx_hello(const mir_hello_conf *cfg, struct rte_mempool *pool,
                 uint32_t *sent, uint32_t *dropped, char *err, size_t errlen);

#endif /* MIR_TX_HELLO_H */
