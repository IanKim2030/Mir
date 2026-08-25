#define _GNU_SOURCE

#include "events.h"

#include <rte_ring.h>
#include <rte_ring_elem.h>

/* SPSC 링. 생산자는 RX lcore 하나, 소비자는 제어 스레드 하나뿐이라
 * SP/SC 플래그로 만들어 경합 처리 오버헤드를 없앤다. */
static struct rte_ring *g_ring;

int mir_events_init(unsigned count, int socket_id)
{
    if (g_ring)
        return 0;

    g_ring = rte_ring_create_elem("mir_events", sizeof(mir_event), count,
                                  socket_id, RING_F_SP_ENQ | RING_F_SC_DEQ);
    return g_ring ? 0 : -1;
}

void mir_events_free(void)
{
    if (g_ring) {
        rte_ring_free(g_ring);
        g_ring = NULL;
    }
}

int mir_events_push(const mir_event *ev)
{
    if (!g_ring)
        return -1;
    /* 값 복사로 넣는다. 실패(-ENOBUFS)면 -1. */
    return rte_ring_enqueue_elem(g_ring, (void *)ev, sizeof(*ev)) == 0 ? 0 : -1;
}

unsigned mir_events_drain(mir_event *out, unsigned max)
{
    if (!g_ring)
        return 0;
    return rte_ring_dequeue_burst_elem(g_ring, out, sizeof(mir_event), max, NULL);
}
