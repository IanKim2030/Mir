/*
 * pktbuild — PacketSpec 을 **완성된 프레임 배열**로 미리 굽는다.
 *
 * 왜 미리 굽는가. 송신 루프에서 헤더를 조립하고 체크섬을 계산하면 패킷마다
 * 수십 사이클이 붙는다. 64B 라인레이트는 10G 에서 14.88 Mpps, 100G 에서
 * 148.8 Mpps 라 코어 하나가 패킷당 쓸 수 있는 예산이 수십 사이클 수준이다.
 * 조립을 그 안에 넣으면 예산을 통째로 먹는다.
 *
 * 그래서 가변 조합을 **빌드 시점에 전부 펼쳐** 두고, worker 는 그중 하나를
 * mbuf 로 복사만 한다. 체크섬도 굽는 시점에 한 번씩만 계산된다.
 *
 * 대가는 메모리다. 조합 수 × 프레임 크기를 들고 있어야 하므로 상한을 둔다
 * (64B × 1024 = 64KB, 1518B × 1024 = 1.5MB — 어느 쪽이든 L2 에 안 들어가지만
 * 순차 접근이라 프리페처가 잘 먹는다).
 */
#ifndef MIR_PKTBUILD_H
#define MIR_PKTBUILD_H

#include <stddef.h>
#include <stdint.h>

#include "dataplane.pb-c.h"

/* CRC 를 제외한 최대 프레임. 점보는 Phase 7 에서 다룬다. */
#define MIR_PKT_MAX_FRAME 1518

/* 가변 조합 상한. 넘으면 잘라내고 그 사실을 호출자에게 알린다 —
 * 조용히 줄이면 "플로우를 N개 벌렸다"는 사용자의 기대가 어긋난다. */
#define MIR_PKT_MAX_VARIANTS 1024

typedef struct {
    /* variants[i] 는 length 바이트짜리 완성 프레임이다. */
    uint8_t *variants;      /* n_variants * frame_len 크기의 연속 버퍼 */
    uint32_t n_variants;
    uint16_t frame_len;

    /* 요청된 조합 수. n_variants 보다 크면 상한에 걸려 잘린 것이다. */
    uint32_t requested_variants;

    /* 체크섬 오프로드용. use_offload 가 0 이면 이미 소프트웨어로 계산해 뒀다. */
    int      use_offload;
    uint8_t  l2_len;
    uint8_t  l3_len;
    uint8_t  l4_proto;      /* IPPROTO_TCP / IPPROTO_UDP / 0 */
} mir_pkt_set;

/*
 * spec 을 프레임 배열로 굽는다.
 *
 *   port_mac    : EthSpec.src_mac 이 비었을 때 쓸 포트의 실제 MAC
 *   offload_ok  : NIC 이 L3/L4 체크섬 오프로드를 광고하는가
 *
 * 0 = 성공(호출자가 mir_pkt_set_free 로 해제). -1 = 실패, err 에 사유.
 */
int mir_pkt_set_build(mir_pkt_set *out,
                      const Mir__V1__PacketSpec *spec,
                      const uint8_t port_mac[6],
                      int offload_ok,
                      char *err, size_t errlen);

void mir_pkt_set_free(mir_pkt_set *s);

/* i 번째 변형의 시작 주소. worker 가 mbuf 로 복사할 원본이다. */
static inline const uint8_t *mir_pkt_variant(const mir_pkt_set *s, uint32_t i)
{
    return s->variants + (size_t)(i % s->n_variants) * s->frame_len;
}

#endif /* MIR_PKTBUILD_H */
