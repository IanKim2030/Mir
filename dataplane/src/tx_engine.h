/*
 * tx_engine — 모드 A(Stateless 고속 생성)의 송신 루프.
 *
 * 규약 셋이 이 파일의 전부다.
 *
 * 1. **큐 하나는 lcore 하나만 만진다.** DPDK 의 tx_burst 는 큐 단위로 lock-free
 *    라, 같은 큐를 두 스레드가 만지면 락 없이 조용히 깨진다. worker i 는
 *    queue i 만 쓴다 — 이 대응을 어기면 증상이 "가끔 이상한 패킷"으로 나타나
 *    추적이 매우 어렵다.
 *
 * 2. **조립은 루프 밖에서 끝낸다.** 64B 라인레이트는 코어 하나에 패킷당 수십
 *    사이클만 준다. 루프 안에서는 미리 구운 프레임을 mbuf 로 복사만 한다
 *    (pktbuild.h).
 *
 * 3. **속도 제어는 TSC 로 한다.** nanosleep 은 해상도도 오버헤드도 맞지 않는다.
 *    busy-poll 코어에서 자는 것 자체가 설계에 어긋난다.
 */
#ifndef MIR_TX_ENGINE_H
#define MIR_TX_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#include "dataplane.pb-c.h"
#include "port.h"

/* 한 번에 tx_burst 로 넣는 최대 패킷 수. */
#define MIR_TX_BURST_MAX 512

typedef struct {
    char     scenario_id[64];
    uint32_t tx_lcores;      /* 실제로 launch 한 worker 수 */
    uint32_t n_variants;     /* 굽힌 프레임 변형 수 */
    uint16_t frame_len;
    uint64_t rate_pps;       /* 0 = 최대 속도 */
    int      offload;        /* 체크섬을 NIC 이 채우는가 */
} mir_tx_status;

/*
 * 시나리오를 시작한다. 이미 실행 중이면 거부한다 — 겹쳐 돌면 어느 쪽 트래픽인지
 * 구분할 수 없고, 큐 대응도 깨진다.
 *
 *   port      : 송신할 포트 (인스턴스당 PF 하나)
 *   lcores    : EAL 이 준 lcore 목록 (main 포함)
 *   n_lcores  : 그 개수
 *
 * 0 = 성공. -1 = 실패, err 에 사유.
 */
int mir_tx_start(const Mir__V1__StartScenarioRequest *req,
                 const mir_port *port,
                 const unsigned *lcores, size_t n_lcores,
                 char *err, size_t errlen);

/*
 * 실행 중인 시나리오를 멈추고 worker 가 전부 빠져나올 때까지 기다린다.
 * 실행 중이 아니면 아무것도 하지 않는다(0 반환).
 */
int mir_tx_stop(char *err, size_t errlen);

/* 실행 중이면 1 을 돌려주고 out 을 채운다. 유휴면 0. */
int mir_tx_status_get(mir_tx_status *out);

/* worker 가 tx_burst 에 넣지 못해 버린 누계 (텔레메트리용). */
uint64_t mir_tx_drop_total(void);

/* 종료 경로에서 호출. 실행 중이면 멈추고 자원을 해제한다. */
void mir_tx_shutdown(void);

#endif /* MIR_TX_ENGINE_H */
