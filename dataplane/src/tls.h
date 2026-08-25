/*
 * tls — mbedTLS 클라이언트 TLS 를 우리 TCP-over-DPDK 위에 얹는 래퍼 (Phase 5b).
 *
 * **왜 래퍼인가.** mbedTLS 는 동기 API 처럼 보이지만 non-blocking BIO 를
 * 지원한다 — recv 콜백이 데이터가 없으면 WANT_READ 를 돌려주고,
 * mbedtls_ssl_handshake() 가 그대로 WANT_READ 로 반환한다. 우리는 RX lcore
 * 폴링 루프에서 블록할 수 없으므로, TCP 데이터가 도착할 때마다 인바운드
 * 버퍼에 넣고(mir_tls_feed) 상태 기계를 한 스텝씩 돌린다(mir_tls_pump).
 *
 * **크립토는 RX lcore 인라인.** Mode B 는 블라스트와 배타라 라인레이트 수신을
 * 굶길 일이 없고, 요구사항이 이미 "세션 수 제한"을 전제한다. 핸드셰이크의 수십
 * µs 블록은 소수 세션엔 무해하다. 대규모는 전용 lcore/cryptodev 로 나중에.
 *
 * **아웃바운드는 콜백으로.** TLS 가 만든 레코드는 session.c 의 send_seg_data 로
 * 나간다 — tls 모듈은 TCP seq/ack 를 모른다. 경계를 콜백으로 끊는다.
 */
#ifndef MIR_TLS_H
#define MIR_TLS_H

#include <stddef.h>
#include <stdint.h>

/* 전 세션 공용 설정 (CA·클라 인증서·버전 범위). 한 번 만들고 세션마다 setup. */
typedef struct mir_tls_config mir_tls_config;

/* 세션당 TLS 상태. */
typedef struct mir_tls mir_tls;

/* mir_tls_pump 결과. */
typedef enum {
    MIR_TLS_WANT_MORE = 0,   /* 더 받아야 진행한다 (WANT_READ) */
    MIR_TLS_HANDSHAKE_DONE,  /* 핸드셰이크가 방금 끝났다 */
    MIR_TLS_APP_DATA,        /* 복호화된 앱 데이터를 out 에 채웠다 */
    MIR_TLS_CLOSED,          /* close_notify 수신 */
    MIR_TLS_ERROR,           /* 실패 (mir_tls_error 로 사유) */
} mir_tls_status;

/* TLS 버전 코드 — proto 의 TlsVersion 과 값을 맞춘다. */
enum {
    MIR_TLS_VER_AUTO = 0,
    MIR_TLS_VER_1_2  = 1,
    MIR_TLS_VER_1_3  = 2,
};

/* 아웃바운드 콜백: TLS 레코드를 선로로 내보낸다. ctx 는 세션 핸들. */
typedef void (*mir_tls_send_fn)(void *ctx, const uint8_t *data, size_t len);

/*
 * 공유 설정을 만든다. PEM 은 NUL 종단이 아니어도 되며 내부에서 복사한다.
 *   verify   : 1 이면 서버 인증서를 ca(PEM)로 검증, 실패 시 핸드셰이크 중단.
 *   cert/key : 클라이언트 인증서(mTLS). 둘 다 있어야 설정된다. 없으면 NULL/0.
 *   ver_min/max : MIR_TLS_VER_*. AUTO 면 mbedTLS 기본.
 * 실패 시 NULL 반환하고 err 를 채운다.
 */
mir_tls_config *mir_tls_config_new(int verify,
                                   const uint8_t *ca, size_t ca_len,
                                   const uint8_t *cert, size_t cert_len,
                                   const uint8_t *key, size_t key_len,
                                   int ver_min, int ver_max,
                                   char *err, size_t errlen);

void mir_tls_config_free(mir_tls_config *c);

/*
 * 세션 TLS 컨텍스트를 만든다. cfg 는 mir_tls_config_new 결과(공유).
 *   sni      : SNI 서버 이름 (NULL 이면 생략).
 *   send_cb  : 아웃바운드 레코드 콜백.  send_ctx : 그 콜백의 ctx(세션 핸들).
 * 실패 시 NULL.
 */
mir_tls *mir_tls_new(mir_tls_config *cfg, const char *sni,
                     mir_tls_send_fn send_cb, void *send_ctx);

void mir_tls_free(mir_tls *t);

/* 수신한 TCP 페이로드(복호화 전)를 인바운드 버퍼에 넣는다. */
void mir_tls_feed(mir_tls *t, const uint8_t *data, size_t len);

/*
 * 상태 기계를 한 스텝 전진시킨다 (mir_tls_feed 뒤에 호출).
 * 핸드셰이크 중이면 handshake 를, 끝났으면 read 를 돌린다.
 *   out/outcap : APP_DATA 일 때 복호화된 바이트를 여기 채운다.
 *   *outlen    : 채운 길이.
 * 반환은 mir_tls_status. APP_DATA 는 반복 호출로 더 꺼낼 수 있다.
 */
mir_tls_status mir_tls_pump(mir_tls *t, uint8_t *out, size_t outcap, size_t *outlen);

/* 앱 데이터를 TLS 위로 보낸다 (핸드셰이크 완료 후). 0=성공, -1=실패. */
int mir_tls_write(mir_tls *t, const uint8_t *data, size_t len);

/* 가장 최근 오류 문자열 (""=없음). */
const char *mir_tls_error(const mir_tls *t);

#endif /* MIR_TLS_H */
