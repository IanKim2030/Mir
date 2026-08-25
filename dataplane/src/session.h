/*
 * session — 모드 B, handshake 제어 (Phase 4).
 *
 * N개의 TCP 3-way handshake 를 시도하고, SYN-ACK 를 받으면 지정한 행동을 한다.
 * **비정상 handshake 를 만드는 것**이 목적이다 — ACK 를 생략해 서버를 half-open
 * 으로 남기거나, ACK 대신 RST 로 끊는다. RTT 와 SYN-ACK timeout 도 여기서 잰다.
 *
 * **모든 세션 상태 변경은 RX lcore 에서만** 일어난다. 그 lcore 가 이미 RX 를
 * 폴링하며 SYN-ACK 를 받으니, 매칭·RTT·응답 결정을 같은 lcore 가 하면 상태
 * 공유도 락도 필요 없다. 제어 스레드는 설정을 스테이징만 하고(request_start/stop),
 * RX 루프가 안전한 지점(service)에서 채택한다.
 *
 * 라인레이트가 아니라 **세션 수 제한**이다 (CLAUDE.md 모드 B). 그래서 소프트웨어
 * 체크섬으로 충분하고, 미리 굽기(pktbuild)도 쓰지 않는다.
 */
#ifndef MIR_SESSION_H
#define MIR_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include <rte_mbuf.h>

#include "dataplane.pb-c.h"
#include "port.h"

/* 텔레메트리로 올리는 집계. RX lcore 가 쓰고 제어 스레드가 읽는다 —
 * 카운터라 약간의 tearing 은 텔레메트리에서 문제되지 않는다. */
typedef struct {
    int      active;
    uint32_t sessions;
    uint32_t sent;
    uint32_t synack;
    uint32_t completed;
    uint32_t refused;
    uint32_t timed_out;
    uint64_t rtt_sum_us;
    uint32_t rtt_min_us;
    uint32_t rtt_max_us;
    uint32_t rtt_count;

    /* 데이터 경로 (Phase 5a) — l7_request 가 있을 때만 진행. */
    uint32_t established;   /* 3-way 완료(ACK 보냄) */
    uint32_t req_sent;      /* L7 요청 송신 */
    uint32_t responded;     /* 응답 첫 세그먼트 수신 */
    uint32_t closed;        /* FIN 교환으로 정상 종료 */
    uint64_t bytes_rx;      /* 받은 응답 바이트 누계 */
    uint32_t http_2xx;      /* 상태줄이 HTTP 2xx 인 수 */
} mir_session_stats;

/*
 * 시작을 요청한다 (제어 스레드에서). 설정을 검증·파싱해 스테이징하고, 실제
 * 테이블 구성은 RX lcore 의 service() 가 한다.
 *
 *   port         : 송신·수신할 포트
 *   tx_queue_id  : 이 엔진 전용 TX 큐 (RX lcore 가 독점)
 *
 * 0 = 성공(요청 접수). -1 = 검증 실패, err 에 사유.
 */
int mir_session_request_start(const Mir__V1__HandshakeSpec *spec,
                              const mir_port *port, uint16_t tx_queue_id,
                              char *err, size_t errlen);

/* 정지를 요청한다 (제어 스레드). 실제 정리는 RX lcore 가 한다. */
void mir_session_request_stop(void);

/* RX 루프가 매 회차 부른다 (RX lcore). 대기 요청 처리 + SYN 송신 + timeout 검사. */
void mir_session_service(uint64_t now_ns);

/*
 * RX 루프가 수신 프레임마다 부른다 (RX lcore).
 * 세션에 매칭돼 소비했으면 1, 아니면 0 (호출자가 일반 분류로 넘긴다).
 */
int mir_session_handle(struct rte_mbuf *m, uint64_t now_ns);

/* 집계를 복사한다 (제어 스레드). active 가 0 이면 유휴다. */
void mir_session_get_stats(mir_session_stats *out);

/* 종료 경로. 실행 중이면 정리한다. */
void mir_session_shutdown(void);

#endif /* MIR_SESSION_H */
