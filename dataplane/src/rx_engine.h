/*
 * rx_engine — 수신 프레임을 실시간으로 분류·판정한다 (Phase 3).
 *
 * **전용 lcore 에서 상시 폴링한다.** 판정은 블라스트 중이 아니어도 켜져 있어야
 * 한다 — 유휴 상태에서 상대가 RST 를 쏘는 것도 잡아야 하기 때문이다. 그래서
 * 시나리오 생명주기와 분리해, 기동 시 한 lcore 를 잡아 종료까지 돈다.
 *
 * **판정 위치가 여기인 이유.** SYN-ACK 즉시 판정·RST 감지는 µs 단위 반응이라
 * C 에서 한다(CLAUDE.md 의 판정 위치 분리). 세션 단위 룰은 제어부 몫이다.
 *
 * **개별 응답 패킷은 경계를 넘지 않는다.** SYN 홍수에 상대가 초당 수백만
 * 응답을 돌려줘도 하나씩 올리는 건 성립하지 않는다. 분류는 카운터로 집계하고
 * (stats.h 의 rx_tcp_*), 이벤트는 이상동작만 레이트 제한해 표본으로 올린다.
 */
#ifndef MIR_RX_ENGINE_H
#define MIR_RX_ENGINE_H

#include <stddef.h>

#include "port.h"

/*
 * RX 폴링을 lcore_id 에서 시작한다. port 는 수신할 포트.
 * 0 = 성공. 이미 실행 중이거나 launch 실패면 -1.
 */
int mir_rx_start(const mir_port *port, unsigned lcore_id, char *err, size_t errlen);

/* 폴링을 멈추고 lcore 가 빠져나올 때까지 기다린다. 실행 중이 아니면 0. */
int mir_rx_stop(void);

/* 실행 중이면 그 lcore_id 를, 아니면 RTE_MAX_LCORE 를 돌려준다. */
unsigned mir_rx_lcore(void);

#endif /* MIR_RX_ENGINE_H */
