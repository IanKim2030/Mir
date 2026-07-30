/*
 * mir-dataplane — C/DPDK 데이터플레인.
 *
 * 이번 단계(Phase 0)의 완료 기준은 **컨테이너 안에서 rte_eal_init() 이 성공하고
 * NIC 포트가 인식되는 것**까지다. TX/RX 루프와 패킷 빌더는 Phase 1 이후다.
 *
 * 기동 순서:
 *   1. cpuset·device plugin 환경변수에서 EAL 인자를 조립      (eal_args.c)
 *   2. rte_eal_init
 *   3. 포트 probe 결과 출력
 *   4. 제어 스레드 기동 — 사이드카와 unix socket 으로 연결     (ipc_server.c)
 *   5. SIGTERM/SIGINT 까지 대기 후 정리
 */
#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_lcore.h>
#include <rte_log.h>

#include "eal_args.h"
#include "ipc_server.h"
#include "stats.h"

#ifndef MIR_VERSION
#define MIR_VERSION "0.0.0-dev"
#endif

#define DEFAULT_SOCK_PATH "/var/run/mir/dp.sock"

#define LOG(level, fmt, ...) \
    rte_log(RTE_LOG_ ## level, RTE_LOGTYPE_USER1, "main: " fmt "\n", ##__VA_ARGS__)

/* worker lcore 가 각자 자기 인덱스에만 쓰는 카운터 (채널 ①). */
struct mir_lcore_stats mir_stats[RTE_MAX_LCORE];

static volatile sig_atomic_t g_shutdown = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_shutdown = 1;
}

/*
 * 이후 생성되는 모든 스레드가 worker lcore 를 침범하지 못하도록,
 * 호출 스레드를 main lcore 에 고정한다.
 *
 * 리눅스에서 새 스레드는 **생성한 스레드의 affinity 마스크를 상속**하므로,
 * 여기서 한 번 좁혀 두면 이후 pthread_create 로 만든 스레드(제어 스레드,
 * 그리고 나중에 붙을 어떤 라이브러리의 내부 스레드든)가 전부 main lcore 에
 * 갇힌다. DPDK 버전별 control-thread cpuset 기본값에 의존하지 않기 위해
 * 명시적으로 건다.
 */
static void pin_self_to_main_lcore(void)
{
    unsigned main_lcore = rte_get_main_lcore();

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(main_lcore, &set);

    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
        LOG(WARNING, "main lcore(%u) 고정 실패: %s — 제어 스레드가 worker 코어로 샐 수 있다",
            main_lcore, strerror(errno));
    else
        LOG(INFO, "제어 스레드용 코어 고정: lcore %u", main_lcore);
}

static void format_mac(char *out, size_t outlen, const struct rte_ether_addr *a)
{
    snprintf(out, outlen, "%02x:%02x:%02x:%02x:%02x:%02x",
             a->addr_bytes[0], a->addr_bytes[1], a->addr_bytes[2],
             a->addr_bytes[3], a->addr_bytes[4], a->addr_bytes[5]);
}

/* 반환값 = 인식한 포트 수 */
static size_t probe_ports(mir_port_info *out, size_t max, const eal_args *args)
{
    size_t   n = 0;
    uint16_t port_id;

    RTE_ETH_FOREACH_DEV(port_id) {
        if (n >= max) {
            LOG(WARNING, "포트가 %zu개를 넘어 나머지는 무시한다", max);
            break;
        }

        struct rte_eth_dev_info info;
        memset(&info, 0, sizeof(info));
        if (rte_eth_dev_info_get(port_id, &info) != 0) {
            LOG(WARNING, "port %u: dev_info_get 실패 — 건너뜀", port_id);
            continue;
        }

        struct rte_ether_addr mac;
        memset(&mac, 0, sizeof(mac));
        rte_eth_macaddr_get(port_id, &mac);

        mir_port_info *p = &out[n];
        memset(p, 0, sizeof(*p));
        p->port_id   = port_id;
        p->numa_node = rte_eth_dev_socket_id(port_id);
        snprintf(p->driver, sizeof(p->driver), "%s",
                 info.driver_name ? info.driver_name : "?");
        format_mac(p->mac, sizeof(p->mac), &mac);
        snprintf(p->device_spec, sizeof(p->device_spec), "%s", args->dev.value);

        LOG(INFO, "port %u: driver=%s mac=%s numa=%d rxq_max=%u txq_max=%u",
            p->port_id, p->driver, p->mac, p->numa_node,
            info.max_rx_queues, info.max_tx_queues);
        n++;
    }
    return n;
}

int main(void)
{
    eal_args args;
    char     err[256] = {0};

    printf("mir-dataplane %s\n", MIR_VERSION);

    /* ── 1. EAL 인자 조립 ─────────────────────────────────────── */
    if (eal_args_build(&args, err, sizeof(err)) != 0) {
        fprintf(stderr, "EAL 인자 조립 실패: %s\n", err);
        return 1;
    }

    printf("  lcores      : ");
    for (size_t i = 0; i < args.n_lcores; i++)
        printf("%s%u", i ? "," : "", args.lcores[i]);
    printf("  (%zu개, cpuset 에서 읽음)\n", args.n_lcores);
    printf("  device      : %s:%s\n",
           device_spec_kind_str(args.dev.kind),
           args.dev.value[0] ? args.dev.value : "(없음)");
    printf("  file-prefix : %s\n", args.file_prefix);
    printf("  EAL argv    :");
    for (int i = 0; i < args.argc; i++)
        printf(" %s", args.argv[i]);
    printf("\n\n");

    if (args.dev.kind == DEVICE_SPEC_NONE)
        fprintf(stderr,
                "경고: 장치가 지정되지 않았다. device plugin 이 PCIDEVICE_* 를 "
                "주입했는지, 또는 MIR_DEVICE_SPEC 를 설정했는지 확인할 것.\n");

    /* ── 2. EAL 초기화 ────────────────────────────────────────── */
    int consumed = rte_eal_init(args.argc, args.argv);
    if (consumed < 0) {
        fprintf(stderr, "rte_eal_init 실패: %s\n", rte_strerror(rte_errno));
        fprintf(stderr,
                "  흔한 원인: hugepage 부족 / IOMMU 그룹 not viable / "
                "vfio 장치 미마운트\n");
        eal_args_free(&args);
        return 1;
    }
    LOG(INFO, "rte_eal_init 성공 (main lcore=%u, lcore 수=%u)",
        rte_get_main_lcore(), rte_lcore_count());

    pin_self_to_main_lcore();

    /* ── 3. 포트 probe ────────────────────────────────────────── */
    static mir_port_info ports[MIR_MAX_PORTS];
    size_t n_ports = probe_ports(ports, MIR_MAX_PORTS, &args);

    if (n_ports == 0)
        LOG(WARNING,
            "인식된 포트가 0개다. vfio 바인딩과 device plugin 할당을 확인할 것");
    else
        LOG(INFO, "포트 %zu개 인식", n_ports);

    /* ── 4. 제어 스레드 기동 ──────────────────────────────────── */
    const char *sock_path = getenv("MIR_IPC_SOCKET");
    if (!sock_path || !*sock_path)
        sock_path = DEFAULT_SOCK_PATH;

    const char *node_id = getenv("HOSTNAME");
    if (!node_id || !*node_id)
        node_id = args.file_prefix;

    ipc_server_config cfg = {
        .sock_path  = sock_path,
        .node_id    = node_id,
        .version    = MIR_VERSION,
        .ports      = ports,
        .n_ports    = n_ports,
        .lcores     = args.lcores,
        .n_lcores   = args.n_lcores,
        .main_lcore = rte_get_main_lcore(),
    };

    if (ipc_server_start(&cfg) != 0) {
        fprintf(stderr, "제어 스레드 기동 실패\n");
        rte_eal_cleanup();
        eal_args_free(&args);
        return 1;
    }

    /* ── 5. 종료 대기 ─────────────────────────────────────────── */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    LOG(INFO, "준비 완료 — 사이드카 연결 대기 (%s)", sock_path);

    while (!g_shutdown) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    LOG(INFO, "종료 신호 수신 — 정리 중");
    ipc_server_stop();
    rte_eal_cleanup();
    eal_args_free(&args);
    LOG(INFO, "정상 종료");
    return 0;
}
