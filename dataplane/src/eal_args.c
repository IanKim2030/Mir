#define _GNU_SOURCE

#include "eal_args.h"

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

/* device plugin 이 주입하는 환경변수 접두사.
 * 리소스명 mir.io/dpdk_pf → PCIDEVICE_MIR_IO_DPDK_PF (대문자화, './-' → '_') */
#define PCIDEVICE_ENV_PREFIX "PCIDEVICE_"

/* 수동 지정용 오버라이드. "pci:0000:3b:00.0" 또는 "mac:aa:bb:cc:dd:ee:ff".
 * 로컬 docker run 으로 선검증할 때 device plugin 없이 쓰기 위한 통로. */
#define DEVICE_SPEC_ENV "MIR_DEVICE_SPEC"

#define ARGV_CAP 32

const char *device_spec_kind_str(device_spec_kind k)
{
    switch (k) {
    case DEVICE_SPEC_PCI_BDF:  return "pci";
    case DEVICE_SPEC_MAC_ADDR: return "mac";
    default:                   return "none";
    }
}

static int push_arg(eal_args *a, const char *s)
{
    if (a->argc >= ARGV_CAP - 1)
        return -1;

    char *dup = strdup(s);
    if (!dup)
        return -1;

    a->argv[a->argc]  = dup;
    a->owned[a->argc] = dup;   /* EAL 이 argv 를 뒤섞어도 해제는 owned 로 한다 */
    a->argc++;
    a->argv[a->argc]  = NULL;
    return 0;
}

/*
 * 실제로 할당받은 cpuset 을 읽어 "-l 3,5,7" 형태의 목록을 만든다.
 * CPU Manager 가 어떤 코어를 줬는지는 실행 전에 알 수 없다.
 */
static int build_lcore_list(eal_args *a, char *out, size_t outlen,
                            char *err, size_t errlen)
{
    cpu_set_t set;
    CPU_ZERO(&set);

    if (sched_getaffinity(0, sizeof(set), &set) != 0) {
        snprintf(err, errlen, "sched_getaffinity 실패: %s", strerror(errno));
        return -1;
    }

    size_t used = 0;
    out[0] = '\0';

    for (int cpu = 0; cpu < CPU_SETSIZE && a->n_lcores < EAL_ARGS_MAX_LCORES; cpu++) {
        if (!CPU_ISSET(cpu, &set))
            continue;

        int n = snprintf(out + used, outlen - used, "%s%d",
                         a->n_lcores == 0 ? "" : ",", cpu);
        if (n < 0 || (size_t)n >= outlen - used) {
            snprintf(err, errlen, "lcore 목록이 버퍼를 넘침");
            return -1;
        }
        used += (size_t)n;
        a->lcores[a->n_lcores++] = (unsigned)cpu;
    }

    if (a->n_lcores == 0) {
        snprintf(err, errlen, "할당된 CPU가 없다 (cpuset 이 비어 있음)");
        return -1;
    }
    return 0;
}

/* "0000:3b:00.0,0000:3b:00.1" 처럼 여러 개가 올 수 있다. 첫 번째만 쓴다
 * — 파드 하나가 PF 하나를 점유하는 설계이므로 둘 이상이면 설정 실수다. */
static void take_first_csv(char *dst, size_t dstlen, const char *src)
{
    size_t i = 0;
    while (src[i] && src[i] != ',' && i < dstlen - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void resolve_device(eal_args *a)
{
    a->dev.kind     = DEVICE_SPEC_NONE;
    a->dev.value[0] = '\0';

    /* 1순위: 명시적 오버라이드 */
    const char *override = getenv(DEVICE_SPEC_ENV);
    if (override && *override) {
        if (strncmp(override, "pci:", 4) == 0) {
            a->dev.kind = DEVICE_SPEC_PCI_BDF;
            take_first_csv(a->dev.value, sizeof(a->dev.value), override + 4);
            return;
        }
        if (strncmp(override, "mac:", 4) == 0) {
            a->dev.kind = DEVICE_SPEC_MAC_ADDR;
            take_first_csv(a->dev.value, sizeof(a->dev.value), override + 4);
            return;
        }
        /* 접두사가 없으면 BDF 로 본다 */
        a->dev.kind = DEVICE_SPEC_PCI_BDF;
        take_first_csv(a->dev.value, sizeof(a->dev.value), override);
        return;
    }

    /* 2순위: device plugin 이 주입한 PCIDEVICE_* */
    for (char **e = environ; *e; e++) {
        if (strncmp(*e, PCIDEVICE_ENV_PREFIX, sizeof(PCIDEVICE_ENV_PREFIX) - 1) != 0)
            continue;

        const char *eq = strchr(*e, '=');
        if (!eq || !eq[1])
            continue;

        a->dev.kind = DEVICE_SPEC_PCI_BDF;
        take_first_csv(a->dev.value, sizeof(a->dev.value), eq + 1);
        return;
    }
}

/* hugetlbfs 파일 충돌을 막기 위해 파드마다 고유한 prefix 를 쓴다.
 * emptyDir 로 마운트가 격리되어 있어도, 로컬 docker run 으로 여러 개를
 * 띄울 때는 실제로 충돌한다. */
static void resolve_file_prefix(eal_args *a)
{
    const char *host = getenv("HOSTNAME");
    if (host && *host) {
        snprintf(a->file_prefix, sizeof(a->file_prefix), "%s", host);
        return;
    }
    if (gethostname(a->file_prefix, sizeof(a->file_prefix)) != 0)
        snprintf(a->file_prefix, sizeof(a->file_prefix), "mir");
    a->file_prefix[sizeof(a->file_prefix) - 1] = '\0';
}

int eal_args_build(eal_args *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof(*out));

    out->argv  = calloc(ARGV_CAP, sizeof(char *));
    out->owned = calloc(ARGV_CAP, sizeof(char *));
    if (!out->argv || !out->owned) {
        snprintf(err, errlen, "메모리 부족");
        eal_args_free(out);
        return -1;
    }

    char lcore_list[1024];
    if (build_lcore_list(out, lcore_list, sizeof(lcore_list), err, errlen) != 0) {
        eal_args_free(out);
        return -1;
    }

    resolve_device(out);
    resolve_file_prefix(out);

    int rc = 0;
    rc |= push_arg(out, "mir-dataplane");
    rc |= push_arg(out, "-l");
    rc |= push_arg(out, lcore_list);
    rc |= push_arg(out, "--file-prefix");
    rc |= push_arg(out, out->file_prefix);
    rc |= push_arg(out, "--proc-type=primary");

    switch (out->dev.kind) {
    case DEVICE_SPEC_PCI_BDF:
        /* allowlist. 이걸 주지 않으면 EAL 이 보이는 모든 장치를 probe 한다. */
        rc |= push_arg(out, "-a");
        rc |= push_arg(out, out->dev.value);
        break;

    case DEVICE_SPEC_MAC_ADDR:
        /* Azure MANA 는 MAC 으로 포트를 고르므로 allowlist 를 걸지 않고
         * probe 후 main.c 가 MAC 을 비교해 선택한다. Phase 8 에서 구현. */
        break;

    case DEVICE_SPEC_NONE:
    default:
        /* 장치가 없어도 EAL 초기화 자체는 성공한다. 포트 0개로 뜨고
         * main.c 가 경고를 남긴다 — 설정 실수를 조용히 넘기지 않기 위함. */
        break;
    }

    if (rc != 0) {
        snprintf(err, errlen, "EAL 인자 조립 실패");
        eal_args_free(out);
        return -1;
    }
    return 0;
}

void eal_args_free(eal_args *a)
{
    if (!a)
        return;

    if (a->owned) {
        for (int i = 0; i < a->argc; i++)
            free(a->owned[i]);
        free(a->owned);
        a->owned = NULL;
    }
    free(a->argv);
    a->argv = NULL;
    a->argc = 0;
}
