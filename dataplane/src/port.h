/*
 * port — NIC 포트 구성(mempool · 큐 · start)과 링크 상태 확인.
 *
 * **큐 하나는 반드시 하나의 lcore 만 만진다.** DPDK 의 rx/tx burst 함수는 큐
 * 단위로 lock-free 라, 같은 큐를 두 스레드가 만지면 락 없이 조용히 깨진다.
 * 증상이 "가끔 이상한 패킷"으로 나타나 추적이 매우 어렵다.
 *
 * 그래서 TX 큐를 worker lcore 수만큼 만들고 worker i 가 queue i 만 쓴다
 * (tx_engine.c). RX 는 아직 1개다 — 수신 경로는 Phase 3 이고, 검증하지 않은
 * 구조를 미리 굳히지 않는다.
 */
#ifndef MIR_PORT_H
#define MIR_PORT_H

#include <stddef.h>
#include <stdint.h>

#include <rte_ethdev.h>
#include <rte_mempool.h>

typedef struct {
    uint16_t            port_id;
    struct rte_mempool *pool;      /* 이 포트 전용 mbuf 풀 (포트의 NUMA 노드에 생성) */
    int                 socket_id;
    int                 started;   /* rte_eth_dev_start 성공 여부 */

    uint16_t n_tx_queues;
    uint16_t n_rx_queues;

    /* NIC 이 L3+L4 체크섬을 채워 주는가. 셋을 모두 광고할 때만 1 이다.
     * 0 이면 프레임을 구울 때 소프트웨어로 계산해 둔다 (pktbuild.c). */
    int tx_cksum_offload;
} mir_port;

/*
 * 포트 하나를 구성하고 start 한다.
 *   mempool 생성 → dev_configure → rx/tx queue setup → dev_start → promiscuous
 *
 * 0 = 성공. 실패 시 -1 을 반환하고 err 에 사유를 채운다(부분 생성물은 정리).
 */
int mir_port_setup(uint16_t port_id, uint16_t n_tx_queues,
                   mir_port *out, char *err, size_t errlen);

/* start 된 포트를 stop·close 하고 mempool 을 해제한다. 두 번 불러도 안전하다. */
void mir_port_close(mir_port *p);

/*
 * 링크가 올라올 때까지 최대 timeout_ms 만큼 기다린다.
 * 1 = link up, 0 = timeout(링크 down 상태로 반환), -1 = 조회 실패.
 *
 * 링크가 내려가 있어도 실패로 보지 않는다. 케이블이 빠졌거나 상대 장비가
 * 아직 안 떠 있는 상황은 정상적인 운영 상태이고, 그 판단은 호출자 몫이다.
 */
int mir_port_wait_link(uint16_t port_id, unsigned timeout_ms,
                       struct rte_eth_link *out);

#endif /* MIR_PORT_H */
