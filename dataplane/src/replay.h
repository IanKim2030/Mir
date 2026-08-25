/*
 * replay — 모드 C(PCAP 리플레이)의 송신 엔진.
 *
 * pcap_path 가 가리키는 파일을 통째로 메모리에 올려(pcap_reader), Ethernet
 * 프레임을 **선로에 나온 그대로** 재전송한다. 캡처된 프레임은 이미 체크섬이
 * 박혀 있으므로 오프로드를 켜지 않고 바이트를 그대로 복사해 보낸다.
 *
 * 규약.
 *
 * 1. **lcore 하나만 쓴다.** 리플레이는 순서 보존이 본질이라 여러 worker 로
 *    쪼개면 프레임 순서가 깨진다. worker lcore 하나를 골라 queue 0 로만 낸다
 *    (모드 A/B 와 배타이므로 큐 충돌이 없다).
 *
 * 2. **타이밍은 TSC 로 잰다.** preserve_timing 이면 프레임 간 간격을 원본
 *    ts_ns 차이(×배속)에서 계산해 busy-poll 로 기다린다. nanosleep 은 해상도가
 *    맞지 않는다. 타임스탬프가 없는(SPB 등) 구간은 최대 속도로 나간다.
 *
 * 3. **파일은 명령이 아니라 경로로 온다.** GB 급일 수 있어 제어 채널을 타지
 *    않는다 — 공유 볼륨을 데이터플레인이 RO 로 마운트한다(StartScenarioRequest).
 */
#ifndef MIR_REPLAY_H
#define MIR_REPLAY_H

#include <stddef.h>
#include <stdint.h>

#include "dataplane.pb-c.h"
#include "port.h"

typedef struct {
    char     scenario_id[64];
    char     pcap_path[256];
    char     format[8];       /* "pcap" | "pcapng" */
    unsigned lcore;           /* 재생에 쓴 worker lcore */
    uint32_t n_frames;        /* 메모리에 올린(=재생 대상) Ethernet 프레임 수 */
    uint32_t n_skipped;       /* 링크타입 불일치·과대 프레임으로 건너뛴 수 */
    uint32_t loops_target;    /* 실제 재생 횟수 (0 요청은 1 로 정규화) */
    int      preserve_timing;
} mir_replay_status;

/*
 * 리플레이를 시작한다. 이미 실행 중이면 거부한다(모드 A/B 와도 겹칠 수 없다 —
 * 호출자가 STOP 으로 먼저 정지시킨다).
 *
 *   req    : pcap_path 와 replay 옵션을 담은 명령
 *   port   : 송신 포트 (인스턴스당 PF 하나)
 *   lcores : EAL 이 준 lcore 목록 (main 포함)
 *
 * 0 = 성공(백그라운드 lcore 에서 재생 시작). -1 = 실패, err 에 사유.
 */
int mir_replay_start(const Mir__V1__StartScenarioRequest *req,
                     const mir_port *port,
                     const unsigned *lcores, size_t n_lcores,
                     char *err, size_t errlen);

/* 실행 중이면 멈추고 lcore 가 빠져나올 때까지 기다린다. 유휴면 0. */
int mir_replay_stop(char *err, size_t errlen);

/* 실행 중이면 1 을 돌려주고 out 을 채운다. 유휴면 0.
 * loop 가 유한이라 스스로 끝났으면 여기서 거둔다(tx_engine 과 같은 규약). */
int mir_replay_status_get(mir_replay_status *out);

/* 종료 경로에서 호출. 실행 중이면 멈추고 자원을 해제한다. */
void mir_replay_shutdown(void);

#endif /* MIR_REPLAY_H */
