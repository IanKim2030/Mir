#define _GNU_SOURCE

#include "ipc_server.h"
#include "stats.h"
#include "events.h"
#include "session.h"
#include "tx_engine.h"
#include "replay.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <rte_ethdev.h>
#include <rte_log.h>

#include "dataplane.pb-c.h"   /* meson custom_target 이 protoc-c 로 생성 */

#define TELEMETRY_INTERVAL_MS 100
#define POLL_TIMEOUT_MS       100
#define FRAME_HEADER_LEN      6            /* u32 length + u16 type */
#define FRAME_MAX_PAYLOAD     (1u << 20)   /* 1 MiB — 벌크는 공유 볼륨으로 간다 */

#define LOG(level, fmt, ...) \
    rte_log(RTE_LOG_ ## level, RTE_LOGTYPE_USER1, "ipc: " fmt "\n", ##__VA_ARGS__)

static ipc_server_config g_cfg;
static pthread_t         g_thread;
static atomic_int        g_stop     = 0;
static int               g_running  = 0;

/* ───────────────────────────────────────────────────────────
 * 프레이밍
 * ─────────────────────────────────────────────────────────── */

static int write_full(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p   += n;
        len -= (size_t)n;
    }
    return 0;
}

/* 1 = 읽음, 0 = 상대가 정상 종료, -1 = 오류 */
static int read_full(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n == 0)
            return 0;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p   += n;
        len -= (size_t)n;
    }
    return 1;
}

static int frame_send(int fd, uint16_t type, const uint8_t *payload, size_t len)
{
    if (len > FRAME_MAX_PAYLOAD)
        return -1;

    uint8_t hdr[FRAME_HEADER_LEN];
    uint32_t total = (uint32_t)(sizeof(uint16_t) + len);   /* type + payload */

    hdr[0] = (uint8_t)(total >> 24);
    hdr[1] = (uint8_t)(total >> 16);
    hdr[2] = (uint8_t)(total >> 8);
    hdr[3] = (uint8_t)(total);
    hdr[4] = (uint8_t)(type >> 8);
    hdr[5] = (uint8_t)(type);

    if (write_full(fd, hdr, sizeof(hdr)) != 0)
        return -1;
    if (len > 0 && write_full(fd, payload, len) != 0)
        return -1;
    return 0;
}

/*
 * 1 = 프레임 획득(*payload 는 호출자가 free), 0 = 연결 종료, -1 = 오류.
 */
static int frame_recv(int fd, uint16_t *type, uint8_t **payload, size_t *len)
{
    uint8_t hdr[FRAME_HEADER_LEN];

    int rc = read_full(fd, hdr, sizeof(hdr));
    if (rc <= 0)
        return rc;

    uint32_t total = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                     ((uint32_t)hdr[2] << 8)  |  (uint32_t)hdr[3];
    *type = (uint16_t)(((uint16_t)hdr[4] << 8) | hdr[5]);

    if (total < sizeof(uint16_t)) {
        LOG(ERR, "프레임 길이가 헤더보다 짧다 (%u)", total);
        return -1;
    }
    size_t plen = total - sizeof(uint16_t);
    if (plen > FRAME_MAX_PAYLOAD) {
        LOG(ERR, "프레임이 너무 크다 (%zu > %u)", plen, FRAME_MAX_PAYLOAD);
        return -1;
    }

    uint8_t *buf = NULL;
    if (plen > 0) {
        buf = malloc(plen);
        if (!buf)
            return -1;
        rc = read_full(fd, buf, plen);
        if (rc <= 0) {
            free(buf);
            return rc;
        }
    }

    *payload = buf;
    *len     = plen;
    return 1;
}

/* ───────────────────────────────────────────────────────────
 * 메시지 조립
 * ─────────────────────────────────────────────────────────── */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static int send_packed(int fd, uint16_t type, size_t len,
                       size_t (*pack)(const void *, uint8_t *), const void *msg)
{
    uint8_t stackbuf[2048];
    uint8_t *buf = len <= sizeof(stackbuf) ? stackbuf : malloc(len);
    if (!buf)
        return -1;

    pack(msg, buf);
    int rc = frame_send(fd, type, buf, len);

    if (buf != stackbuf)
        free(buf);
    return rc;
}

/* protobuf-c 의 pack 함수는 시그니처가 메시지마다 다르므로 얇게 감싼다. */
static size_t pack_hello_response(const void *m, uint8_t *out)
{
    return mir__v1__hello_response__pack(m, out);
}
static size_t pack_telemetry(const void *m, uint8_t *out)
{
    return mir__v1__telemetry_snapshot__pack(m, out);
}
static size_t pack_ack(const void *m, uint8_t *out)
{
    return mir__v1__ack__pack(m, out);
}
static size_t pack_event(const void *m, uint8_t *out)
{
    return mir__v1__event__pack(m, out);
}

/*
 * 이벤트 링을 비우고 각 이벤트를 EVENT 프레임으로 보낸다 (제어 스레드에서).
 *
 * 판정은 RX lcore 에서 링에 넣기만 하고, 실제 전송(인코딩+socket write)은
 * 여기서 한다 — busy-poll 하는 RX lcore 를 socket I/O 로 멈추게 두지 않는다.
 * 한 번에 최대 EVENT_DRAIN_MAX 개만 보내 텔레메트리 주기를 방해하지 않는다.
 */
#define EVENT_DRAIN_MAX 64

static int send_events(int fd)
{
    mir_event evs[EVENT_DRAIN_MAX];
    unsigned n = mir_events_drain(evs, EVENT_DRAIN_MAX);

    for (unsigned i = 0; i < n; i++) {
        Mir__V1__Event ev = MIR__V1__EVENT__INIT;
        ev.ts_ns    = evs[i].ts_ns;
        ev.kind     = (Mir__V1__Event__Kind)evs[i].kind;
        ev.port_id  = evs[i].port_id;
        ev.flow_key = evs[i].flow_key;
        ev.rtt_us   = evs[i].rtt_us;
        ev.detail   = evs[i].detail;

        size_t len = mir__v1__event__get_packed_size(&ev);
        if (send_packed(fd, MIR__V1__MSG_TYPE__MSG_TYPE_EVENT,
                        len, pack_event, &ev) != 0)
            return -1;
    }
    return 0;
}

static int send_hello_response(int fd)
{
    Mir__V1__HelloResponse resp = MIR__V1__HELLO_RESPONSE__INIT;

    Mir__V1__PortInfo  info[MIR_MAX_PORTS];
    Mir__V1__PortInfo *info_ptr[MIR_MAX_PORTS];

    size_t n = g_cfg.n_ports > MIR_MAX_PORTS ? MIR_MAX_PORTS : g_cfg.n_ports;
    for (size_t i = 0; i < n; i++) {
        mir__v1__port_info__init(&info[i]);
        info[i].port_id     = g_cfg.ports[i].port_id;
        info[i].driver      = (char *)g_cfg.ports[i].driver;
        info[i].mac         = (char *)g_cfg.ports[i].mac;
        info[i].numa_node   = g_cfg.ports[i].numa_node;
        info[i].device_spec = (char *)g_cfg.ports[i].device_spec;
        info_ptr[i]         = &info[i];
    }

    uint32_t lcores[EAL_ARGS_MAX_LCORES];
    size_t   nl = g_cfg.n_lcores > EAL_ARGS_MAX_LCORES
                      ? EAL_ARGS_MAX_LCORES : g_cfg.n_lcores;
    for (size_t i = 0; i < nl; i++)
        lcores[i] = (uint32_t)g_cfg.lcores[i];

    resp.dataplane_version = (char *)g_cfg.version;
    resp.node_id           = (char *)g_cfg.node_id;
    resp.n_ports           = n;
    resp.ports             = info_ptr;
    resp.n_lcores          = nl;
    resp.lcores            = lcores;
    resp.main_lcore        = g_cfg.main_lcore;

    size_t len = mir__v1__hello_response__get_packed_size(&resp);
    return send_packed(fd, MIR__V1__MSG_TYPE__MSG_TYPE_HELLO_RESPONSE,
                       len, pack_hello_response, &resp);
}

static int send_telemetry(int fd)
{
    struct mir_stats_total total;
    mir_stats_sum(&total);

    Mir__V1__TelemetrySnapshot snap = MIR__V1__TELEMETRY_SNAPSHOT__INIT;

    Mir__V1__PortStats  ps[MIR_MAX_PORTS];
    Mir__V1__PortStats *ps_ptr[MIR_MAX_PORTS];

    size_t n = g_cfg.n_ports > MIR_MAX_PORTS ? MIR_MAX_PORTS : g_cfg.n_ports;
    for (size_t i = 0; i < n; i++) {
        mir__v1__port_stats__init(&ps[i]);
        ps[i].port_id = g_cfg.ports[i].port_id;
        ps_ptr[i]     = &ps[i];

        /* 포트별 수치는 **NIC 하드웨어 카운터**에서 읽는다. per-lcore 카운터
         * (mir_stats)는 포트로 분해되지 않을뿐더러, 우리가 큐에 넣은 수와 NIC
         * 이 실제로 내보낸 수가 어긋나는 것 자체가 Phase 1 에서 봐야 할
         * 정보다. rte_eth_stats_get 은 제어 경로 API 라 이 스레드에서 불러도
         * worker 의 burst 루프를 방해하지 않는다. */
        if (!g_cfg.ports[i].started)
            continue;

        struct rte_eth_stats es;
        if (rte_eth_stats_get((uint16_t)g_cfg.ports[i].port_id, &es) != 0)
            continue;

        ps[i].tx_pkts  = es.opackets;
        ps[i].tx_bytes = es.obytes;
        ps[i].tx_err   = es.oerrors;
        ps[i].rx_pkts  = es.ipackets;
        ps[i].rx_bytes = es.ibytes;
        ps[i].rx_drop  = es.imissed + es.rx_nombuf;
        ps[i].rx_err   = es.ierrors;
    }

    snap.ts_ns         = now_ns();
    snap.node_id       = (char *)g_cfg.node_id;
    snap.n_ports       = n;
    snap.ports         = ps_ptr;
    snap.event_drop    = total.event_drop;
    snap.active_lcores = (uint32_t)g_cfg.n_lcores;
    snap.tx_drop       = total.tx_drop;

    Mir__V1__RxClass rx = MIR__V1__RX_CLASS__INIT;
    rx.tcp_syn     = total.rx_tcp_syn;
    rx.tcp_syn_ack = total.rx_tcp_syn_ack;
    rx.tcp_rst     = total.rx_tcp_rst;
    rx.tcp_fin     = total.rx_tcp_fin;
    rx.tcp_ack     = total.rx_tcp_ack;
    rx.tcp_other   = total.rx_tcp_other;
    rx.udp         = total.rx_udp;
    rx.non_ip      = total.rx_non_ip;
    snap.rx        = &rx;

    Mir__V1__HandshakeStats hs = MIR__V1__HANDSHAKE_STATS__INIT;
    mir_session_stats ss;
    mir_session_get_stats(&ss);
    if (ss.active) {
        hs.sessions   = ss.sessions;
        hs.sent       = ss.sent;
        hs.synack     = ss.synack;
        hs.completed  = ss.completed;
        hs.refused    = ss.refused;
        hs.timed_out  = ss.timed_out;
        hs.rtt_min_us = ss.rtt_count ? ss.rtt_min_us : 0;
        hs.rtt_max_us = ss.rtt_max_us;
        hs.rtt_avg_us = ss.rtt_count
                            ? (uint32_t)(ss.rtt_sum_us / ss.rtt_count) : 0;
        hs.established = ss.established;
        hs.req_sent    = ss.req_sent;
        hs.responded   = ss.responded;
        hs.closed      = ss.closed;
        hs.bytes_rx    = ss.bytes_rx;
        hs.http_2xx    = ss.http_2xx;
        snap.handshake = &hs;
    }

    /* 포트 카운터만으로는 "누가 시켜서 나가는 트래픽인지" 알 수 없다.
     * 데이터플레인이 스스로 무엇을 하고 있다고 생각하는지를 올려야
     * 제어부가 명령한 것과 대조할 수 있다. */
    mir_tx_status tx;
    mir_replay_status rp;
    if (mir_tx_status_get(&tx)) {
        snap.active_scenario = tx.scenario_id;
        snap.tx_lcores       = tx.tx_lcores;
    } else if (mir_replay_status_get(&rp)) {
        /* 리플레이도 스스로 끝났으면 여기서 거둔다(status_get 의 부수효과).
         * tx 카운터는 mir_stats 합산으로 이미 흐르므로 이름만 올린다. */
        snap.active_scenario = rp.scenario_id;
        snap.tx_lcores       = 1;
    }

    size_t len = mir__v1__telemetry_snapshot__get_packed_size(&snap);
    return send_packed(fd, MIR__V1__MSG_TYPE__MSG_TYPE_TELEMETRY,
                       len, pack_telemetry, &snap);
}

static int send_ack(int fd, int ok, const char *message)
{
    Mir__V1__Ack ack = MIR__V1__ACK__INIT;
    ack.ok      = ok ? 1 : 0;
    ack.message = (char *)message;

    size_t len = mir__v1__ack__get_packed_size(&ack);
    return send_packed(fd, MIR__V1__MSG_TYPE__MSG_TYPE_ACK, len, pack_ack, &ack);
}

/* ───────────────────────────────────────────────────────────
 * 수신 처리
 * ─────────────────────────────────────────────────────────── */

static int handle_frame(int fd, uint16_t type, const uint8_t *payload, size_t len)
{
    switch (type) {
    case MIR__V1__MSG_TYPE__MSG_TYPE_HELLO_REQUEST: {
        Mir__V1__HelloRequest *req =
            mir__v1__hello_request__unpack(NULL, len, payload);
        if (req) {
            LOG(INFO, "hello (control=%s)",
                req->control_version ? req->control_version : "?");
            mir__v1__hello_request__free_unpacked(req, NULL);
        }
        return send_hello_response(fd);
    }

    case MIR__V1__MSG_TYPE__MSG_TYPE_START_SCENARIO: {
        Mir__V1__StartScenarioRequest *req =
            mir__v1__start_scenario_request__unpack(NULL, len, payload);
        if (!req)
            return send_ack(fd, 0, "start 요청을 해석하지 못했다");

        char terr[256] = {0};
        char msg[256]  = {0};
        int  rc;

        if (g_cfg.n_ports == 0 || !g_cfg.dev) {
            mir__v1__start_scenario_request__free_unpacked(req, NULL);
            return send_ack(fd, 0,
                "포트가 없다 — vfio 바인딩과 MIR_DEVICE_SPEC 를 확인할 것");
        }

        if (req->pcap_path && *req->pcap_path) {
            /* 모드 C — PCAP 리플레이. pcap_path 가 있으면 packet/handshake 는
             * 무시한다(proto 규약). worker lcore 하나가 순차 재생한다. */
            LOG(INFO, "replay 요청: id=%s path=%s loop=%u timing=%d",
                req->scenario_id ? req->scenario_id : "",
                req->pcap_path,
                req->replay ? req->replay->loop : 0,
                req->replay ? req->replay->preserve_timing : 0);
            rc = mir_replay_start(req, &g_cfg.dev[0], g_cfg.lcores,
                                  g_cfg.n_lcores, terr, sizeof(terr));
            mir_replay_status rp;
            if (rc == 0 && mir_replay_status_get(&rp))
                snprintf(msg, sizeof(msg),
                         "리플레이 시작됨: %s 프레임 %u개 재생=%u회 timing=%s",
                         rp.format, rp.n_frames, rp.loops_target,
                         rp.preserve_timing ? "보존" : "최대");
        } else if (req->handshake) {
            /* 모드 B — handshake 제어. RX lcore 가 실제 상태를 돌린다. */
            LOG(INFO, "handshake 요청: id=%s dst=%s:%u sessions=%u action=%d",
                req->scenario_id ? req->scenario_id : "",
                req->handshake->dst_ip ? req->handshake->dst_ip : "?",
                req->handshake->dst_port, req->handshake->sessions,
                req->handshake->on_synack);
            rc = mir_session_request_start(req->handshake, &g_cfg.dev[0],
                                           g_cfg.sess_txq, terr, sizeof(terr));
            if (rc == 0)
                snprintf(msg, sizeof(msg), "handshake 시작됨: sessions=%u",
                         req->handshake->sessions ? req->handshake->sessions : 1);
        } else {
            /* 모드 A — 무상태 블라스트. */
            LOG(INFO, "start 요청: id=%s rate=%llu duration=%us lcores=%u",
                req->scenario_id ? req->scenario_id : "",
                (unsigned long long)req->rate_pps,
                req->duration_s, req->tx_lcores);
            rc = mir_tx_start(req, &g_cfg.dev[0], g_cfg.lcores, g_cfg.n_lcores,
                              terr, sizeof(terr));
            mir_tx_status st;
            if (rc == 0 && mir_tx_status_get(&st))
                snprintf(msg, sizeof(msg),
                         "시작됨: worker=%u frame=%uB 변형=%u 체크섬=%s",
                         st.tx_lcores, st.frame_len, st.n_variants,
                         st.offload ? "NIC" : "SW");
        }
        mir__v1__start_scenario_request__free_unpacked(req, NULL);

        if (rc != 0) {
            LOG(ERR, "start 실패: %s", terr);
            return send_ack(fd, 0, terr);
        }
        return send_ack(fd, 1, msg[0] ? msg : "시작됨");
    }

    case MIR__V1__MSG_TYPE__MSG_TYPE_STOP_SCENARIO: {
        Mir__V1__StopScenarioRequest *req =
            mir__v1__stop_scenario_request__unpack(NULL, len, payload);
        if (req)
            mir__v1__stop_scenario_request__free_unpacked(req, NULL);

        /* 어느 모드가 돌든 다 세운다 — 무엇이 실행 중인지 호출자가 몰라도
         * 정지가 되게 한다. */
        mir_tx_status st;
        mir_session_stats ss;
        mir_replay_status rp;
        mir_tx_status_get(&st);
        mir_session_get_stats(&ss);
        int was = st.tx_lcores > 0 || ss.active || mir_replay_status_get(&rp);

        char terr[256] = {0};
        if (mir_tx_stop(terr, sizeof(terr)) != 0)
            return send_ack(fd, 0, terr);
        mir_session_request_stop();
        mir_replay_stop(terr, sizeof(terr));

        return send_ack(fd, 1, was ? "정지됨" : "실행 중인 시나리오가 없었다");
    }

    default:
        LOG(WARNING, "알 수 없는 프레임 타입 %u — 무시", type);
        return 0;
    }
}

/* ───────────────────────────────────────────────────────────
 * 서버 스레드
 * ─────────────────────────────────────────────────────────── */

static int listen_socket(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG(ERR, "socket 실패: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        LOG(ERR, "소켓 경로가 너무 길다: %s", path);
        close(fd);
        return -1;
    }
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    /* 컨테이너가 재시작하면 공유 볼륨에 예전 소켓 파일이 남아 bind 가 실패한다. */
    unlink(path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG(ERR, "bind(%s) 실패: %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    /* 사이드카가 다른 UID 로 돌 수 있다. 이 소켓은 컨테이너 쌍이 공유하는 볼륨 안에만
     * 존재하고 컨테이너 밖에서는 도달할 수 없으므로 0666 이어도 노출이 없다. */
    if (chmod(path, 0666) != 0)
        LOG(WARNING, "chmod(%s) 실패: %s", path, strerror(errno));

    if (listen(fd, 4) != 0) {
        LOG(ERR, "listen 실패: %s", strerror(errno));
        close(fd);
        return -1;
    }

    LOG(INFO, "listen %s", path);
    return fd;
}

static void *server_thread(void *arg)
{
    (void)arg;

    int lfd = listen_socket(g_cfg.sock_path);
    if (lfd < 0)
        return NULL;

    int      cfd      = -1;
    uint64_t next_tel = mono_ms() + TELEMETRY_INTERVAL_MS;

    while (!atomic_load(&g_stop)) {
        struct pollfd pfd[2];
        nfds_t n = 0;

        pfd[n].fd = lfd;      pfd[n].events = POLLIN; n++;
        if (cfd >= 0) {
            pfd[n].fd = cfd;  pfd[n].events = POLLIN; n++;
        }

        int rc = poll(pfd, n, POLL_TIMEOUT_MS);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            LOG(ERR, "poll 실패: %s", strerror(errno));
            break;
        }

        /* 신규 연결 — 사이드카가 재시작했다면 기존 연결을 교체한다. */
        int replaced = 0;
        if (pfd[0].revents & POLLIN) {
            int nfd = accept(lfd, NULL, NULL);
            if (nfd >= 0) {
                /* 상대가 프레임 중간에 멈춰도 영원히 매달리지 않게 한다.
                 * 타임아웃이 걸리면 연결을 닫고, 사이드카가 재연결한다. */
                struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
                setsockopt(nfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                setsockopt(nfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

                if (cfd >= 0) {
                    LOG(INFO, "사이드카 재연결 — 기존 연결 교체");
                    close(cfd);
                }
                cfd      = nfd;
                replaced = 1;   /* pfd[1].revents 는 이제 옛 fd 의 것이다 */
                LOG(INFO, "사이드카 연결됨");
            }
        }

        /* 수신.
         * 연결을 교체한 직후에는 건너뛴다 — pfd[1].revents 가 방금 닫은 fd 의
         * 상태라, 그걸 믿고 새 fd 를 읽으면 데이터가 없어도 read 에 들어간다. */
        if (cfd >= 0 && n > 1 && !replaced &&
            (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            uint16_t type;
            uint8_t *payload = NULL;
            size_t   len     = 0;

            int fr = frame_recv(cfd, &type, &payload, &len);
            if (fr <= 0) {
                LOG(INFO, "사이드카 연결 종료");
                close(cfd);
                cfd = -1;
            } else {
                if (handle_frame(cfd, type, payload, len) != 0) {
                    LOG(WARNING, "응답 전송 실패 — 연결을 닫는다");
                    close(cfd);
                    cfd = -1;
                }
                free(payload);
            }
        }

        /* 이벤트는 매 루프 비운다 (POLL_TIMEOUT_MS 주기). 텔레메트리보다 자주
         * 보내야 이상동작을 빨리 알린다. 링이 비어 있으면 비용이 없다. */
        if (cfd >= 0 && send_events(cfd) != 0) {
            LOG(WARNING, "이벤트 전송 실패 — 연결을 닫는다");
            close(cfd);
            cfd = -1;
        }

        /* 텔레메트리 주기 push */
        uint64_t now = mono_ms();
        if (now >= next_tel) {
            next_tel = now + TELEMETRY_INTERVAL_MS;
            if (cfd >= 0 && send_telemetry(cfd) != 0) {
                LOG(WARNING, "텔레메트리 전송 실패 — 연결을 닫는다");
                close(cfd);
                cfd = -1;
            }
        }
    }

    if (cfd >= 0)
        close(cfd);
    close(lfd);
    unlink(g_cfg.sock_path);
    LOG(INFO, "서버 스레드 종료");
    return NULL;
}

/* ───────────────────────────────────────────────────────────
 * 공개 API
 * ─────────────────────────────────────────────────────────── */

int ipc_server_start(const ipc_server_config *cfg)
{
    if (g_running)
        return -1;

    g_cfg = *cfg;
    atomic_store(&g_stop, 0);

    if (pthread_create(&g_thread, NULL, server_thread, NULL) != 0) {
        LOG(ERR, "pthread_create 실패: %s", strerror(errno));
        return -1;
    }
    g_running = 1;
    return 0;
}

void ipc_server_stop(void)
{
    if (!g_running)
        return;

    atomic_store(&g_stop, 1);
    pthread_join(g_thread, NULL);
    g_running = 0;
}
