/*
 * mir-dataplane — C/DPDK 데이터플레인.
 *
 * Phase 0 의 완료 기준은 rte_eal_init() 성공 + 포트 인식이었고, Phase 1 은
 * 거기에 **포트 start 와 hello packet 송신**을 얹는다. 즉 지금 증명하려는
 * 것은 "mbuf 를 조립해 tx_burst 로 내보내면 실제로 선로에 나간다"까지다.
 * 속도 제어·다중 lcore·L3/L4 헤더 빌더는 Phase 2 다.
 *
 * 기동 순서:
 *   1. cpuset·환경변수에서 EAL 인자를 조립                    (eal_args.c)
 *   2. rte_eal_init
 *   3. 포트 probe 결과 출력
 *   4. 포트 구성·start·링크 확인                             (port.c)
 *   5. MIR_HELLO_TX_COUNT 가 설정돼 있으면 hello packet 송신  (tx_hello.c)
 *   6. 제어 스레드 기동 — 사이드카와 unix socket 으로 연결     (ipc_server.c)
 *   7. SIGTERM/SIGINT 까지 대기 후 정리
 *
 * 포트 구성이나 hello 송신이 실패해도 프로세스는 죽지 않는다. 제어 채널이
 * 살아 있어야 운영자가 hello response 로 상태를 확인할 수 있고, 기동 실패로
 * 컨테이너가 재시작 루프에 빠지면 그 진단 경로마저 사라진다.
 *
 * 대신 "살아 있음"이 "정상"으로 오해되지 않도록, 포트를 하나도 못 잡으면
 * 사이드카가 health 를 NOT_SERVING 으로 유지한다 (internal/agent).
 */
#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <rte_dev.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_lcore.h>
#include <rte_log.h>

#include "eal_args.h"
#include "ipc_server.h"
#include "port.h"
#include "rx_engine.h"
#include "session.h"
#include "stats.h"
#include "tx_engine.h"
#include "tx_hello.h"

#ifndef MIR_VERSION
#define MIR_VERSION "0.0.0-dev"
#endif

#define DEFAULT_SOCK_PATH "/var/run/mir/dp.sock"

/* 링크 협상 대기. 100G 광 링크는 수 초까지 걸린다. */
#define LINK_WAIT_MS 9000

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
        /* **요청한** BDF 가 아니라 EAL 이 실제로 붙인 장치 이름을 싣는다.
         * 요청값을 되돌려주면 제어부의 "기대 vs 실측" 대조가 설정을 설정과
         * 비교하는 꼴이 되어 아무것도 검증하지 못한다. PCI 장치면 이 값이
         * BDF 문자열("0000:43:00.0")이다. */
        const char *devname = info.device ? rte_dev_name(info.device) : NULL;
        snprintf(p->device_spec, sizeof(p->device_spec), "%s",
                 devname && *devname ? devname : args->dev.value);

        LOG(INFO, "port %u: driver=%s mac=%s numa=%d rxq_max=%u txq_max=%u",
            p->port_id, p->driver, p->mac, p->numa_node,
            info.max_rx_queues, info.max_tx_queues);
        n++;
    }
    return n;
}

/*
 * hello packet 송신 — 환경변수로 켰을 때만 동작한다.
 *
 * 제어 채널(StartScenario)이 아니라 환경변수로 거는 이유: 실물 반입 시점에는
 * 제어부·사이드카가 아직 안 떠 있어도 NIC 만 놓고 TX 경로를 확인할 수 있어야
 * 한다. 이 단계에서 의존 대상을 늘리면 "무엇이 고장났는지"를 좁히지 못한다.
 */
static void run_hello_tx(const mir_port_info *ports, mir_port *dev, size_t n_ports)
{
    mir_hello_conf hello;
    char           herr[256] = {0};

    int on = mir_hello_conf_from_env(&hello, herr, sizeof(herr));
    if (on < 0) {
        LOG(ERR, "hello 설정 오류: %s — 송신을 건너뛴다", herr);
        return;
    }
    if (on == 0)
        return;

    /* port_id 는 PMD 가 부여한 값이라 0 부터 연속이라는 보장이 없다. */
    size_t idx = SIZE_MAX;
    for (size_t i = 0; i < n_ports; i++) {
        if (ports[i].port_id == hello.port_id) {
            idx = i;
            break;
        }
    }
    if (idx == SIZE_MAX) {
        LOG(ERR, "MIR_HELLO_TX_PORT=%u 는 인식된 포트가 아니다", hello.port_id);
        return;
    }
    if (!dev[idx].started) {
        LOG(ERR, "port %u 는 start 되지 않았다 — hello 송신 불가", hello.port_id);
        return;
    }

    LOG(INFO, "hello 송신: port=%u count=%u size=%uB burst=%u dst=%02x:%02x:%02x:%02x:%02x:%02x",
        hello.port_id, hello.count, hello.pkt_size, hello.burst,
        hello.dst.addr_bytes[0], hello.dst.addr_bytes[1], hello.dst.addr_bytes[2],
        hello.dst.addr_bytes[3], hello.dst.addr_bytes[4], hello.dst.addr_bytes[5]);

    uint32_t sent = 0, dropped = 0;
    if (mir_tx_hello(&hello, dev[idx].pool, &sent, &dropped,
                     herr, sizeof(herr)) != 0) {
        LOG(ERR, "hello 송신 실패: %s", herr);
        return;
    }

    LOG(INFO, "hello 송신 완료: %u/%u 전송, %u 폐기 (%llu bytes)",
        sent, hello.count, dropped,
        (unsigned long long)sent * hello.pkt_size);

    if (dropped > 0 || sent < hello.count)
        LOG(WARNING, "전량 전송되지 않았다 — 링크 상태와 상대 장비를 확인할 것");
}

int main(void)
{
    eal_args args;
    char     err[256] = {0};

    /* 컨테이너 로그로 나갈 때 stdout 은 블록 버퍼링이 된다. 그러면 아래
     * 진단 배너가 EAL 의 stderr 출력보다 **뒤에** 찍히고, 기동에 실패해
     * 프로세스가 죽으면 버퍼째 유실된다. 기동 실패를 진단하는 1차 자료가
     * 바로 이 배너라 그걸 잃으면 안 된다. */
    setvbuf(stdout, NULL, _IOLBF, 0);

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

    if (args.mem_mb > 0) {
        printf("  memory      : %u MB", args.mem_mb);
        if (args.n_sockets > 0) {
            printf("  (NUMA 노드 %u개 중 ", args.n_sockets);
            for (unsigned n = 0, first = 1; n < args.n_sockets; n++) {
                if (!args.socket_local[n])
                    continue;
                printf("%snode%u", first ? "" : ",", n);
                first = 0;
            }
            printf(" 에 배정)");
        } else {
            printf("  (NUMA 정보 없음 — 총량으로 지정)");
        }
        printf("\n");
    } else {
        printf("  memory      : 상한 없음 (MIR_MEM_MB 미설정)\n");
    }

    printf("  EAL argv    :");
    for (int i = 0; i < args.argc; i++)
        printf(" %s", args.argv[i]);
    printf("\n\n");

    if (args.dev.kind == DEVICE_SPEC_NONE)
        fprintf(stderr,
                "경고: 장치가 지정되지 않았다. MIR_DEVICE_SPEC 를 설정했는지 "
                "확인할 것 (예: MIR_DEVICE_SPEC=pci:0000:43:00.0).\n");

    /* 인스턴스를 여러 개 띄우는데 상한이 없으면 먼저 뜬 쪽이 hugepage 를
     * 전부 가져가고 나머지가 기동에 실패한다. 조용히 넘기지 않는다. */
    if (args.mem_mb == 0)
        fprintf(stderr,
                "경고: MIR_MEM_MB 가 없어 hugepage 상한이 걸리지 않았다. "
                "한 장비에 인스턴스를 여러 개 띄운다면 반드시 설정할 것.\n");

    /* ── 2. EAL 초기화 ────────────────────────────────────────── */
    int consumed = rte_eal_init(args.argc, args.argv);
    if (consumed < 0) {
        fprintf(stderr, "rte_eal_init 실패: %s\n", rte_strerror(rte_errno));
        fprintf(stderr,
                "  흔한 원인: hugepage 부족 / IOMMU 그룹 not viable / "
                "vfio 장치 미마운트\n");
        fprintf(stderr,
                "  MIR_MEM_MB 를 썼다면 그 값 × 인스턴스 수가 호스트 hugepage "
                "총량을 넘지 않는지 확인할 것.\n");
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
            "인식된 포트가 0개다. vfio 바인딩과 MIR_DEVICE_SPEC 를 확인할 것");
    else
        LOG(INFO, "포트 %zu개 인식", n_ports);

    /* ── 4. lcore 배치 ────────────────────────────────────────── */
    /*
     * main   = 제어 스레드 (IPC 서버)
     * RX     = 상시 수신 폴링 (Phase 3) — 하나를 잡아 종료까지 돈다
     * 나머지 = TX worker
     *
     * RX 를 상시 켜 두는 이유: 판정은 블라스트 중이 아니어도 켜져 있어야 한다.
     * 유휴 상태에서 상대가 RST 를 쏘는 것도 잡아야 하기 때문이다. 대가는 코어
     * 하나를 항상 점유하는 것이고, 코어가 귀해지는 Phase 7 에서 재검토한다. */
    unsigned main_lcore = rte_get_main_lcore();
    unsigned rx_lcore   = RTE_MAX_LCORE;
    for (size_t i = 0; i < args.n_lcores; i++) {
        if (args.lcores[i] != main_lcore) {
            rx_lcore = args.lcores[i];   /* main 이 아닌 첫 lcore 를 RX 로 */
            break;
        }
    }

    /* TX 큐 배치:
     *   0 .. n_workers-1  : Mode A blast worker 전용
     *   n_workers          : RX lcore 의 handshake(Mode B) 응답 전용
     * 큐를 시나리오 시작 시점에 늘릴 수는 없다 — rte_eth_dev_configure 는 포트
     * stop 상태에서만 되고, 그러면 링크가 내려갔다 올라온다. 그래서 최대치로
     * 미리 잡는다. */
    uint16_t n_workers = (uint16_t)(args.n_lcores > 2 ? args.n_lcores - 2 : 1);
    uint16_t sess_txq  = n_workers;                 /* RX lcore 전용 TX 큐 */
    uint16_t n_tx_q    = (uint16_t)(n_workers + 1);

    /* ── 5. 포트 구성·start ───────────────────────────────────── */
    static mir_port dev[MIR_MAX_PORTS];

    for (size_t i = 0; i < n_ports; i++) {
        char perr[256] = {0};
        if (mir_port_setup(ports[i].port_id, n_tx_q, &dev[i], perr, sizeof(perr)) != 0) {
            LOG(ERR, "port %u 구성 실패: %s", ports[i].port_id, perr);
            continue;
        }
        ports[i].started = 1;
        LOG(INFO, "port %u: TX 큐 %u개 (worker %u + handshake 1), RX 큐 1개",
            ports[i].port_id, n_tx_q, n_workers);
        mir_port_wait_link(ports[i].port_id, LINK_WAIT_MS, NULL);
    }

    /* ── 6. RX 폴링 시작 (첫 start 된 포트에서) ───────────────── */
    if (rx_lcore != RTE_MAX_LCORE) {
        for (size_t i = 0; i < n_ports; i++) {
            if (!dev[i].started)
                continue;
            char rerr[256] = {0};
            if (mir_rx_start(&dev[i], rx_lcore, rerr, sizeof(rerr)) != 0)
                LOG(ERR, "RX 폴링 시작 실패: %s", rerr);
            break;   /* 인스턴스당 PF 하나 — 첫 포트만 */
        }
    } else {
        LOG(WARNING, "RX 폴링용 lcore 가 없다 (코어가 부족하다) — 수신 판정 비활성");
    }

    /* ── 7. hello packet 송신 ─────────────────────────────────── */
    run_hello_tx(ports, dev, n_ports);

    /* ── 6. 제어 스레드 기동 ──────────────────────────────────── */
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
        .dev        = dev,
        .sess_txq   = sess_txq,
    };

    if (ipc_server_start(&cfg) != 0) {
        fprintf(stderr, "제어 스레드 기동 실패\n");
        for (size_t i = 0; i < n_ports; i++)
            mir_port_close(&dev[i]);
        rte_eal_cleanup();
        eal_args_free(&args);
        return 1;
    }

    /* ── 7. 종료 대기 ─────────────────────────────────────────── */
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

    /* 순서가 중요하다. 제어 스레드를 먼저 세워야 그 스레드가 텔레메트리를
     * 만들며 rte_eth_stats_get() 을 부르는 도중에 포트가 닫히는 일이 없다. */
    ipc_server_stop();

    /* worker 가 아직 tx/rx_burst 를 돌고 있는데 포트를 닫으면 그대로 깨진다.
     * 반드시 포트보다 먼저 세운다. */
    mir_tx_shutdown();
    mir_session_shutdown();
    mir_rx_stop();

    for (size_t i = 0; i < n_ports; i++)
        mir_port_close(&dev[i]);
    rte_eal_cleanup();
    eal_args_free(&args);
    LOG(INFO, "정상 종료");
    return 0;
}
