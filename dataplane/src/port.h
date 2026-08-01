/*
 * port — NIC 포트 구성(mempool · 큐 · start)과 링크 상태 확인.
 *
 * Phase 1 은 포트당 rx/tx 큐를 **1개씩만** 만든다. 큐를 lcore 수만큼 벌려
 * RSS 로 분배하는 것은 Phase 2(고속 송신)의 일이고, 여기서 미리 만들어 두면
 * 검증하지 않은 구조가 굳는다. 지금 확정하려는 것은 "포트가 실제로 start 되고
 * 프레임이 선로에 나간다"는 사실 하나다.
 *
 * 큐 하나는 반드시 하나의 lcore 만 만진다. DPDK 의 rx/tx burst 함수는
 * 큐 단위로 lock-free 라 같은 큐를 두 스레드가 만지는 순간 조용히 깨진다.
 * Phase 1 에서는 main lcore 만 TX 하므로 이 규칙이 자동으로 지켜진다.
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
} mir_port;

/*
 * 포트 하나를 구성하고 start 한다.
 *   mempool 생성 → dev_configure → rx/tx queue setup → dev_start → promiscuous
 *
 * 0 = 성공. 실패 시 -1 을 반환하고 err 에 사유를 채운다(부분 생성물은 정리).
 */
int mir_port_setup(uint16_t port_id, mir_port *out, char *err, size_t errlen);

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
