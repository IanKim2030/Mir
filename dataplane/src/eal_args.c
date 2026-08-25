#define _GNU_SOURCE

#include "eal_args.h"

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

/* 장치 주입 환경변수 접두사 (폴백 경로).
 * 오케스트레이터의 device plugin 이 쓰던 규약으로, 그런 환경에 얹을 때를
 * 위해 남겨 둔다. Compose 배포에서는 MIR_DEVICE_SPEC 가 정본이다. */
#define PCIDEVICE_ENV_PREFIX "PCIDEVICE_"

/* 장치 지정. "pci:0000:3b:00.0" 또는 "mac:aa:bb:cc:dd:ee:ff".
 * Compose 배포에서는 이게 정본이고, PCIDEVICE_* 는 폴백이다. */
#define DEVICE_SPEC_ENV "MIR_DEVICE_SPEC"

/* 이 인스턴스가 쓸 hugepage 상한(MB). 미지정이면 상한을 걸지 않는다. */
#define MEM_MB_ENV "MIR_MEM_MB"

/* 1TB. 이보다 큰 값은 오타로 본다 (자릿수를 하나 더 친 경우). */
#define MEM_MB_MAX (1024L * 1024L)

#define SYS_NODE_DIR "/sys/devices/system/node"

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
 * 어떤 코어를 받았는지는 실행 전에 알 수 없다 — compose 의 cpuset 이든
 * taskset 이든 결과는 sched_getaffinity 로만 확인된다.
 */
static int build_lcore_list(eal_args *a, const cpu_set_t *set,
                            char *out, size_t outlen,
                            char *err, size_t errlen)
{
    size_t used = 0;
    out[0] = '\0';

    for (int cpu = 0; cpu < CPU_SETSIZE && a->n_lcores < EAL_ARGS_MAX_LCORES; cpu++) {
        if (!CPU_ISSET(cpu, set))
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
 * — 인스턴스 하나가 PF 하나를 점유하는 설계이므로 둘 이상이면 설정 실수다. */
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

    /* 2순위: 외부에서 주입한 PCIDEVICE_* (폴백) */
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

/* 한 장비에서 인스턴스를 여러 개 띄우면 hugetlbfs 파일 이름이 충돌한다.
 * 인스턴스마다 고유한 prefix 를 줘서 막는다 — compose 가 HOSTNAME 을
 * <machine>-dp<id> 로 넣어 주므로 그 값이 그대로 쓰인다. */
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

/* ── 메모리 상한 ──────────────────────────────────────────── */

static int read_small_file(const char *path, char *buf, size_t buflen)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    size_t n = fread(buf, 1, buflen - 1, f);
    fclose(f);
    buf[n] = '\0';
    return 0;
}

/* "0-11,24-35" 또는 "2,3,4" 형식을 훑어 set 과 겹치는 CPU 가 있는지 본다. */
static int cpulist_intersects(const char *list, const cpu_set_t *set)
{
    const char *p = list;

    while (*p) {
        char *end;
        long lo = strtol(p, &end, 10);
        if (end == p)
            break;
        p = end;

        long hi = lo;
        if (*p == '-') {
            p++;
            hi = strtol(p, &end, 10);
            if (end == p)
                break;
            p = end;
        }

        for (long c = lo; c <= hi; c++) {
            if (c >= 0 && c < CPU_SETSIZE && CPU_ISSET((int)c, set))
                return 1;
        }

        if (*p != ',')
            break;
        p++;
    }
    return 0;
}

/*
 * MIR_MEM_MB 와 NUMA 토폴로지를 읽는다.
 *
 * 노드 열거가 안 되는 환경(NUMA 미지원 커널, /sys 미마운트)에서는
 * n_sockets 를 0 으로 두고, 호출자가 -m 폴백을 쓴다.
 */
static int resolve_memory(eal_args *a, const cpu_set_t *set,
                          char *err, size_t errlen)
{
    a->mem_mb    = 0;
    a->n_sockets = 0;

    const char *s = getenv(MEM_MB_ENV);
    if (!s || !*s)
        return 0;

    errno = 0;
    char *end;
    long mb = strtol(s, &end, 10);

    /* 상한을 두지 않으면 큰 값이 unsigned 로 잘려 **의도보다 작은 상한**이
     * 걸린다. 조용히 틀린 값이 되는 쪽이 거부하는 것보다 나쁘다. */
    if (errno != 0 || *end != '\0' || mb <= 0 || mb > MEM_MB_MAX) {
        snprintf(err, errlen, "%s 값이 잘못됐다: \"%s\" (1~%ld 사이의 정수 MB)",
                 MEM_MB_ENV, s, (long)MEM_MB_MAX);
        return -1;
    }
    a->mem_mb = (unsigned)mb;

    for (unsigned n = 0; n < EAL_ARGS_MAX_SOCKETS; n++) {
        char path[256];

        snprintf(path, sizeof(path), SYS_NODE_DIR "/node%u", n);
        if (access(path, F_OK) != 0)
            break;
        a->n_sockets = n + 1;

        snprintf(path, sizeof(path), SYS_NODE_DIR "/node%u/cpulist", n);

        char buf[512];
        if (read_small_file(path, buf, sizeof(buf)) == 0 &&
            cpulist_intersects(buf, set))
            a->socket_local[n] = 1;
    }
    return 0;
}

/*
 * "--socket-mem 4096,0" 의 값 부분을 만든다.
 * 노드를 못 셌거나 우리 코어가 어느 노드에도 안 걸리면 -1 (호출자가 -m 폴백).
 */
static int build_socket_list(const eal_args *a, char *out, size_t outlen)
{
    if (a->n_sockets == 0)
        return -1;

    int any = 0;
    for (unsigned n = 0; n < a->n_sockets; n++)
        any |= a->socket_local[n];
    if (!any)
        return -1;

    size_t used = 0;
    out[0] = '\0';

    for (unsigned n = 0; n < a->n_sockets; n++) {
        int val = a->socket_local[n] ? (int)a->mem_mb : 0;

        int written = snprintf(out + used, outlen - used, "%s%d",
                               n == 0 ? "" : ",", val);
        if (written < 0 || (size_t)written >= outlen - used)
            return -1;
        used += (size_t)written;
    }
    return 0;
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

    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) {
        snprintf(err, errlen, "sched_getaffinity 실패: %s", strerror(errno));
        eal_args_free(out);
        return -1;
    }

    char lcore_list[1024];
    if (build_lcore_list(out, &set, lcore_list, sizeof(lcore_list), err, errlen) != 0) {
        eal_args_free(out);
        return -1;
    }

    if (resolve_memory(out, &set, err, errlen) != 0) {
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

    /* 메모리 상한.
     *
     * --socket-mem 은 기동 시 그만큼을 먼저 확보하므로, hugepage 가 모자라면
     * **폴트 시점 SIGBUS 가 아니라 기동 실패**로 즉시 드러난다. 인스턴스를
     * 여러 개 띄우는 구성에서 이 fail-fast 성질이 상한의 핵심이다.
     * --socket-limit 은 그 뒤 동적 증가까지 같은 값으로 막는다. */
    if (out->mem_mb > 0) {
        char socket_list[256];

        if (build_socket_list(out, socket_list, sizeof(socket_list)) == 0) {
            rc |= push_arg(out, "--socket-mem");
            rc |= push_arg(out, socket_list);
            rc |= push_arg(out, "--socket-limit");
            rc |= push_arg(out, socket_list);
        } else {
            /* NUMA 정보를 못 읽었다. 노드 구분 없이 총량으로만 건다. */
            char mb[32];
            snprintf(mb, sizeof(mb), "%u", out->mem_mb);
            rc |= push_arg(out, "-m");
            rc |= push_arg(out, mb);
        }
    }

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
