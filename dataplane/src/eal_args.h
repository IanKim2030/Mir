/*
 * eal_args — 컨테이너 환경에서 DPDK EAL 인자를 런타임에 조립한다.
 *
 * 인스턴스마다 코어도 장치도 다르므로 `-l 1-4` 같은 하드코딩은 반드시 깨진다.
 * 셋 다 실행 시점에 읽는다.
 *
 *   lcore  ← sched_getaffinity(2)  (컨테이너 cpuset 이 그대로 보인다)
 *   장치   ← MIR_DEVICE_SPEC, 또는 PCIDEVICE_* 환경변수
 *   메모리 ← MIR_MEM_MB + /sys 의 NUMA 토폴로지
 *
 * 메모리 상한이 여기 있는 이유: 오케스트레이터가 걸어 주던 인스턴스별
 * hugepage 한도가 없으면, 한 인스턴스가 호스트의 hugepage 를 전부 잡아
 * 나머지가 기동에 실패하거나 폴트 시점에 SIGBUS 로 죽는다. EAL 인자로
 * 직접 상한을 거는 것이 유일한 대체 수단이다.
 */
#ifndef EAL_ARGS_H
#define EAL_ARGS_H

#include <stddef.h>

/*
 * 장치 지정 방식.
 *
 * 베어메탈 / AWS(ENA) / GCP(gVNIC) 는 vfio-pci 모델이라 PCI BDF 로 지정하지만,
 * Azure MANA PMD 는 BDF 가 아니라 **MAC 주소**로 바인딩 대상을 결정한다.
 * 지금은 PCI_BDF 만 구현하고 MAC_ADDR 은 Phase 8 을 위해 자리만 잡아둔다 —
 * 이 한 겹이 나중에 재작성을 막는다.
 */
typedef enum {
    DEVICE_SPEC_NONE = 0,
    DEVICE_SPEC_PCI_BDF,
    DEVICE_SPEC_MAC_ADDR,
} device_spec_kind;

#define DEVICE_SPEC_VALUE_MAX 64
#define EAL_ARGS_MAX_LCORES   256
#define EAL_ARGS_PREFIX_MAX   64
#define EAL_ARGS_MAX_SOCKETS  8

typedef struct {
    device_spec_kind kind;
    char             value[DEVICE_SPEC_VALUE_MAX];
} device_spec;

typedef struct {
    /* rte_eal_init() 에 그대로 넘긴다.
     * EAL 이 argv 순서를 바꿀 수 있으므로 해제는 반드시 eal_args_free() 로. */
    int    argc;
    char **argv;

    /* 진단 출력과 HelloResponse 용 사본 */
    unsigned    lcores[EAL_ARGS_MAX_LCORES];
    size_t      n_lcores;
    device_spec dev;
    char        file_prefix[EAL_ARGS_PREFIX_MAX];

    /* 메모리 상한. mem_mb == 0 이면 인자를 붙이지 않는다(= EAL 기본 동작).
     *
     * n_sockets 는 /sys 에서 센 NUMA 노드 수이고, socket_local[n] 은 우리
     * cpuset 이 노드 n 을 건드리는지다. 이 둘이 있으면 --socket-mem/--socket-limit
     * 을 노드별로 정확히 줄 수 있고, 없으면(감지 실패) -m 으로 폴백한다.
     *
     * cpuset 이 두 노드에 걸쳐 있으면 각 노드에 mem_mb 를 요청하게 되어
     * 대개 기동이 실패하는데, 그건 **핀 설정이 잘못됐다는 정확한 신호**다.
     * 계측기에서 cross-NUMA 배치는 조용히 넘길 문제가 아니다. */
    unsigned mem_mb;
    unsigned n_sockets;
    int      socket_local[EAL_ARGS_MAX_SOCKETS];

    /* 내부 전용 — eal_args_free() 가 사용 */
    char **owned;
} eal_args;

/* 0 = 성공, -1 = 실패(err 에 사유를 채운다). */
int eal_args_build(eal_args *out, char *err, size_t errlen);

void eal_args_free(eal_args *a);

const char *device_spec_kind_str(device_spec_kind k);

#endif /* EAL_ARGS_H */
