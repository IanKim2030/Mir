/*
 * ipc_server — 인스턴스 내부 hop (채널 ③): C 데이터플레인 ⇄ Go 사이드카.
 *
 * unix socket 위에 4바이트 length-prefix 프레이밍을 얹고 payload 는
 * protobuf-c 로 인코딩한다. 와이어 포맷은 proto/dataplane.proto 주석과
 * 반드시 일치해야 한다.
 *
 *   +--------+--------+------------------+
 *   | u32 be | u16 be |     payload      |
 *   | length |  type  |    (protobuf)    |
 *   +--------+--------+------------------+
 *
 * C 가 서버인 이유: EAL 초기화가 수 초 걸리므로 사이드카가 재시도하며
 * 붙는 편이 자연스럽다. 반대로 하면 C 가 부팅 중 사이드카를 기다려야 한다.
 *
 * 소켓 I/O 는 전용 pthread 에서만 수행한다. 이 스레드는 rte_eal_init() 이
 * main lcore 에 고정해 둔 메인 스레드로부터 생성되므로 affinity 를 상속해
 * **worker lcore 를 절대 침범하지 않는다**.
 */
#ifndef MIR_IPC_SERVER_H
#define MIR_IPC_SERVER_H

#include "port.h"

#include <stddef.h>
#include <stdint.h>

#include "eal_args.h"

#define MIR_MAX_PORTS       32
#define MIR_PORT_DRIVER_MAX 64
#define MIR_PORT_MAC_MAX    24   /* "aa:bb:cc:dd:ee:ff" + 여유 */

typedef struct {
    uint32_t port_id;
    char     driver[MIR_PORT_DRIVER_MAX];
    char     mac[MIR_PORT_MAC_MAX];
    int32_t  numa_node;
    char     device_spec[DEVICE_SPEC_VALUE_MAX];

    /* rte_eth_dev_start 성공 여부. start 되지 않은 포트에 rte_eth_stats_get 을
     * 부르면 안 되므로 텔레메트리가 이 값을 보고 건너뛴다. */
    uint8_t  started;
} mir_port_info;

/*
 * 이 구조체가 가리키는 메모리는 ipc_server_stop() 이 끝날 때까지
 * 호출자가 살려 두어야 한다 (main.c 의 정적 저장소를 쓴다).
 */
typedef struct {
    const char          *sock_path;
    const char          *node_id;     /* 인스턴스명 */
    const char          *version;
    const mir_port_info *ports;
    size_t               n_ports;
    const unsigned      *lcores;
    size_t               n_lcores;
    unsigned             main_lcore;

    /* 송신 엔진이 쓸 포트 핸들. ports[] 와 같은 순서다.
     * 인스턴스당 PF 하나가 전제라 시나리오는 dev[0] 을 쓴다. */
    const mir_port      *dev;

    /* handshake(Mode B) 응답 전용 TX 큐. RX lcore 가 독점한다. */
    uint16_t             sess_txq;
} ipc_server_config;

/* 0 = 성공. 백그라운드 스레드를 띄우고 즉시 반환한다. */
int ipc_server_start(const ipc_server_config *cfg);

/* 스레드 종료를 요청하고 join 한다. 최대 100ms(폴 주기) 걸린다. */
void ipc_server_stop(void);

#endif /* MIR_IPC_SERVER_H */
