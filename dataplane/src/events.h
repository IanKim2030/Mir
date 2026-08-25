/*
 * events — RX lcore(생산자) → 제어 스레드(소비자) 이벤트 큐.
 *
 * **왜 큐인가.** 판정은 RX lcore 에서 µs 단위로 일어나는데, 그 결과를 사이드카로
 * 보내는 것(protobuf 인코딩 + unix socket write)은 느리고 블로킹될 수 있다.
 * busy-poll 하는 RX lcore 가 거기서 멈추면 수신을 놓친다. 그래서 판정은 고정
 * 크기 구조체를 링에 넣기만 하고, 제어 스레드가 100ms 주기로 꺼내 보낸다.
 *
 * **왜 rte_ring 인가.** SPSC(생산자 하나, 소비자 하나)라 락이 필요 없지만,
 * lock-free 큐의 메모리 배리어를 직접 짜면 틀리기 쉽다. rte_ring 은 그걸
 * 검증된 형태로 제공한다. 요소를 값으로 담는(rte_ring_create_elem) 변형을 써서
 * mempool 없이 구조체를 그대로 넣는다 — 문자열 포인터를 링에 담으면 생산자가
 * 재사용하는 버퍼를 소비자가 읽는 race 가 난다.
 *
 * **가득 차면 버린다.** 제어 채널이 느려도 판정 정확도(카운터)는 영향받지
 * 않아야 한다는 게 이 프로젝트의 전제다. 이벤트는 이상동작의 표본일 뿐이고,
 * 버려진 수는 event_drop 으로 집계된다.
 */
#ifndef MIR_EVENTS_H
#define MIR_EVENTS_H

#include <stddef.h>
#include <stdint.h>

/* 이벤트 종류 — proto 의 Event.Kind 와 값을 맞춘다 (변환 시 그대로 캐스팅).
 * 새 값을 넣을 때는 proto 의 Kind enum 과 반드시 함께 고친다. */
typedef enum {
    MIR_EVENT_UNSPECIFIED      = 0,
    MIR_EVENT_SYNACK_TIMEOUT   = 1,
    MIR_EVENT_RST_RECEIVED     = 2,
    MIR_EVENT_UNEXPECTED_FLAG  = 3,
    MIR_EVENT_RETRANSMIT       = 4,
    MIR_EVENT_HANDSHAKE_DONE   = 5,
    MIR_EVENT_HANDSHAKE_REFUSED = 6,
} mir_event_kind;

#define MIR_EVENT_FLOWKEY_MAX 64
#define MIR_EVENT_DETAIL_MAX  64

/* 링에 값으로 담기는 이벤트. 문자열은 고정 배열이라 포인터 수명 문제가 없다. */
typedef struct {
    uint64_t       ts_ns;
    mir_event_kind kind;
    uint32_t       port_id;
    uint32_t       rtt_us;
    char           flow_key[MIR_EVENT_FLOWKEY_MAX];
    char           detail[MIR_EVENT_DETAIL_MAX];
} mir_event;

/* 링을 만든다. count 는 2의 거듭제곱이어야 한다(rte_ring 요구).
 * socket_id 는 링 메모리를 둘 NUMA 노드. 0 = 성공. */
int mir_events_init(unsigned count, int socket_id);

void mir_events_free(void);

/*
 * 이벤트 하나를 넣는다 (RX lcore 에서 호출).
 * 0 = 성공, -1 = 가득 참(호출자가 event_drop 을 올린다).
 * 초기화 전이면 -1 을 돌려주므로 RX 경로가 항상 안전하다.
 */
int mir_events_push(const mir_event *ev);

/*
 * 최대 max 개를 꺼낸다 (제어 스레드에서 호출). 꺼낸 수를 돌려준다.
 * 링이 없으면 0.
 */
unsigned mir_events_drain(mir_event *out, unsigned max);

#endif /* MIR_EVENTS_H */
