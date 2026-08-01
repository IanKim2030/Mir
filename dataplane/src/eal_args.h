/*
 * eal_args — 컨테이너 환경에서 DPDK EAL 인자를 런타임에 조립한다.
 *
 * k8s CPU Manager(static)는 **임의의** 배타 코어를 컨테이너에 할당하므로
 * `-l 1-4` 같은 하드코딩은 반드시 깨진다. 마찬가지로 PCI 주소도 device
 * plugin 이 파드마다 다르게 주입한다. 둘 다 실행 시점에 읽어야 한다.
 *
 *   lcore  ← sched_getaffinity(2)  (실제 할당된 cpuset)
 *   장치   ← PCIDEVICE_* 환경변수  (device plugin 주입) 또는 MIR_DEVICE_SPEC
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

    /* 내부 전용 — eal_args_free() 가 사용 */
    char **owned;
} eal_args;

/* 0 = 성공, -1 = 실패(err 에 사유를 채운다). */
int eal_args_build(eal_args *out, char *err, size_t errlen);

void eal_args_free(eal_args *a);

const char *device_spec_kind_str(device_spec_kind k);

#endif /* EAL_ARGS_H */
