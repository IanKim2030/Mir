#define _GNU_SOURCE

#include "tls.h"

#include <stdlib.h>
#include <string.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#define seterr(err, len, ...) \
    do { if ((err) && (len)) snprintf((err), (len), __VA_ARGS__); } while (0)

/* 세션 인바운드 버퍼 초기 용량. TLS 레코드(≤16KB)와 서버 인증서 flight 를
 * 담아야 하므로 넉넉히 잡고, 모자라면 두 배로 늘린다. */
#define TLS_IN_INIT 16384

struct mir_tls_config {
    mbedtls_ssl_config       conf;
    mbedtls_x509_crt         cacert;
    mbedtls_x509_crt         clicert;
    mbedtls_pk_context       pkey;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context  entropy;
    int                      has_own_cert;
};

struct mir_tls {
    mbedtls_ssl_context ssl;
    mir_tls_send_fn     send_cb;
    void               *send_ctx;

    uint8_t *in;       /* 인바운드(복호화 전) 바이트 */
    size_t   in_len;   /* 채워진 길이 */
    size_t   in_pos;   /* 소비한 위치 */
    size_t   in_cap;

    int  handshaked;
    char err[128];
};

/* ── PEM 을 NUL 종단으로 복사 (mbedtls_x509_crt_parse 요구) ──── */

/* mbedtls_x509_crt_parse / pk_parse_key 는 PEM 이 NUL 로 끝나고 길이에 그 NUL 이
 * 포함돼야 한다. 원본 바이트는 그렇지 않을 수 있으니 복사본을 만든다. */
static uint8_t *pem_dup_nul(const uint8_t *p, size_t len, size_t *out_len)
{
    uint8_t *b = malloc(len + 1);
    if (!b)
        return NULL;
    memcpy(b, p, len);
    b[len] = '\0';
    *out_len = len + 1;
    return b;
}

/* ── config ──────────────────────────────────────────────────── */

mir_tls_config *mir_tls_config_new(int verify,
                                   const uint8_t *ca, size_t ca_len,
                                   const uint8_t *cert, size_t cert_len,
                                   const uint8_t *key, size_t key_len,
                                   int ver_min, int ver_max,
                                   char *err, size_t errlen)
{
    /* mbedTLS 3.6 은 TLS 1.3 와 USE_PSA_CRYPTO 경로에서 PSA 가 초기화돼 있어야
     * 한다. 여러 번 불러도 안전하다(두 번째부터 no-op). 빠뜨리면 핸드셰이크가
     * 조용히 실패한다. */
    psa_status_t ps = psa_crypto_init();
    if (ps != PSA_SUCCESS) {
        seterr(err, errlen, "psa_crypto_init 실패 (%d)", (int)ps);
        return NULL;
    }

    mir_tls_config *c = calloc(1, sizeof(*c));
    if (!c) {
        seterr(err, errlen, "메모리 부족");
        return NULL;
    }

    mbedtls_ssl_config_init(&c->conf);
    mbedtls_x509_crt_init(&c->cacert);
    mbedtls_x509_crt_init(&c->clicert);
    mbedtls_pk_init(&c->pkey);
    mbedtls_ctr_drbg_init(&c->ctr_drbg);
    mbedtls_entropy_init(&c->entropy);

    const char *pers = "mir-dataplane-tls";
    int rc = mbedtls_ctr_drbg_seed(&c->ctr_drbg, mbedtls_entropy_func,
                                   &c->entropy, (const unsigned char *)pers,
                                   strlen(pers));
    if (rc != 0) {
        seterr(err, errlen, "ctr_drbg_seed 실패 (-0x%04x)", -rc);
        goto fail;
    }

    if (mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        seterr(err, errlen, "ssl_config_defaults 실패");
        goto fail;
    }

    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->ctr_drbg);

    /* 버전 범위. AUTO(0)면 mbedTLS 기본을 그대로 둔다. */
    if (ver_min == MIR_TLS_VER_1_2)
        mbedtls_ssl_conf_min_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    else if (ver_min == MIR_TLS_VER_1_3)
        mbedtls_ssl_conf_min_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_3);
    if (ver_max == MIR_TLS_VER_1_2)
        mbedtls_ssl_conf_max_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    else if (ver_max == MIR_TLS_VER_1_3)
        mbedtls_ssl_conf_max_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_3);

    /* 서버 인증서 검증. */
    if (verify) {
        if (!ca || ca_len == 0) {
            seterr(err, errlen, "verify_server 인데 ca_cert 가 없다");
            goto fail;
        }
        size_t n;
        uint8_t *pem = pem_dup_nul(ca, ca_len, &n);
        if (!pem) { seterr(err, errlen, "메모리 부족"); goto fail; }
        rc = mbedtls_x509_crt_parse(&c->cacert, pem, n);
        free(pem);
        if (rc != 0) {
            seterr(err, errlen, "ca_cert 파싱 실패 (-0x%04x)", -rc);
            goto fail;
        }
        mbedtls_ssl_conf_ca_chain(&c->conf, &c->cacert, NULL);
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        /* 대상 서버를 시험하는 도구라 검증 생략이 기본이다. */
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
    }

    /* 클라이언트 인증서 (mTLS). cert·key 둘 다 있어야 설정된다. */
    if (cert && cert_len && key && key_len) {
        size_t n;
        uint8_t *cpem = pem_dup_nul(cert, cert_len, &n);
        if (!cpem) { seterr(err, errlen, "메모리 부족"); goto fail; }
        rc = mbedtls_x509_crt_parse(&c->clicert, cpem, n);
        free(cpem);
        if (rc != 0) {
            seterr(err, errlen, "client_cert 파싱 실패 (-0x%04x)", -rc);
            goto fail;
        }

        size_t kn;
        uint8_t *kpem = pem_dup_nul(key, key_len, &kn);
        if (!kpem) { seterr(err, errlen, "메모리 부족"); goto fail; }
        rc = mbedtls_pk_parse_key(&c->pkey, kpem, kn, NULL, 0,
                                  mbedtls_ctr_drbg_random, &c->ctr_drbg);
        free(kpem);
        if (rc != 0) {
            seterr(err, errlen, "client_key 파싱 실패 (-0x%04x)", -rc);
            goto fail;
        }

        rc = mbedtls_ssl_conf_own_cert(&c->conf, &c->clicert, &c->pkey);
        if (rc != 0) {
            seterr(err, errlen, "conf_own_cert 실패 (-0x%04x)", -rc);
            goto fail;
        }
        c->has_own_cert = 1;
    }

    return c;

fail:
    mir_tls_config_free(c);
    return NULL;
}

void mir_tls_config_free(mir_tls_config *c)
{
    if (!c)
        return;
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_x509_crt_free(&c->cacert);
    mbedtls_x509_crt_free(&c->clicert);
    mbedtls_pk_free(&c->pkey);
    mbedtls_ctr_drbg_free(&c->ctr_drbg);
    mbedtls_entropy_free(&c->entropy);
    free(c);
}

/* ── BIO 콜백 (non-blocking) ─────────────────────────────────── */

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    mir_tls *t = ctx;
    t->send_cb(t->send_ctx, buf, len);
    return (int)len;   /* session.c 콜백이 mbuf 로 삼키므로 항상 다 나갔다고 본다 */
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    mir_tls *t = ctx;
    size_t avail = t->in_len - t->in_pos;
    if (avail == 0)
        return MBEDTLS_ERR_SSL_WANT_READ;   /* 아직 데이터 없음 — 다음 feed 를 기다린다 */
    size_t n = len < avail ? len : avail;
    memcpy(buf, t->in + t->in_pos, n);
    t->in_pos += n;
    if (t->in_pos == t->in_len)             /* 다 소비했으면 버퍼를 비운다 */
        t->in_pos = t->in_len = 0;
    return (int)n;
}

/* ── 세션 ────────────────────────────────────────────────────── */

mir_tls *mir_tls_new(mir_tls_config *cfg, const char *sni,
                     mir_tls_send_fn send_cb, void *send_ctx)
{
    mir_tls *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;

    t->send_cb = send_cb;
    t->send_ctx = send_ctx;
    t->in = malloc(TLS_IN_INIT);
    if (!t->in) {
        free(t);
        return NULL;
    }
    t->in_cap = TLS_IN_INIT;

    mbedtls_ssl_init(&t->ssl);
    if (mbedtls_ssl_setup(&t->ssl, &cfg->conf) != 0) {
        snprintf(t->err, sizeof(t->err), "ssl_setup 실패");
        mir_tls_free(t);
        return NULL;
    }
    if (sni && *sni)
        mbedtls_ssl_set_hostname(&t->ssl, sni);

    mbedtls_ssl_set_bio(&t->ssl, t, bio_send, bio_recv, NULL);
    return t;
}

void mir_tls_free(mir_tls *t)
{
    if (!t)
        return;
    mbedtls_ssl_free(&t->ssl);
    free(t->in);
    free(t);
}

void mir_tls_feed(mir_tls *t, const uint8_t *data, size_t len)
{
    if (!t || len == 0)
        return;

    /* 남은 미소비분 앞으로 당겨 공간을 확보한다. */
    if (t->in_pos > 0) {
        memmove(t->in, t->in + t->in_pos, t->in_len - t->in_pos);
        t->in_len -= t->in_pos;
        t->in_pos = 0;
    }
    if (t->in_len + len > t->in_cap) {
        size_t cap = t->in_cap;
        while (t->in_len + len > cap)
            cap *= 2;
        uint8_t *nb = realloc(t->in, cap);
        if (!nb)
            return;   /* 못 늘리면 이번 데이터는 버린다 — 세션은 WANT_MORE 로 멈춘다 */
        t->in = nb;
        t->in_cap = cap;
    }
    memcpy(t->in + t->in_len, data, len);
    t->in_len += len;
}

static mir_tls_status map_err(mir_tls *t, int rc)
{
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
        return MIR_TLS_WANT_MORE;
    if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        return MIR_TLS_CLOSED;
    char b[96];
    mbedtls_strerror(rc, b, sizeof(b));
    snprintf(t->err, sizeof(t->err), "%s (-0x%04x)", b, -rc);
    return MIR_TLS_ERROR;
}

mir_tls_status mir_tls_pump(mir_tls *t, uint8_t *out, size_t outcap, size_t *outlen)
{
    if (outlen)
        *outlen = 0;

    if (!t->handshaked) {
        int rc = mbedtls_ssl_handshake(&t->ssl);
        if (rc == 0) {
            t->handshaked = 1;
            return MIR_TLS_HANDSHAKE_DONE;
        }
        return map_err(t, rc);
    }

    int rc = mbedtls_ssl_read(&t->ssl, out, outcap);
    if (rc > 0) {
        if (outlen)
            *outlen = (size_t)rc;
        return MIR_TLS_APP_DATA;
    }
    if (rc == 0)
        return MIR_TLS_CLOSED;
    return map_err(t, rc);
}

int mir_tls_write(mir_tls *t, const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int rc = mbedtls_ssl_write(&t->ssl, data + off, len - off);
        if (rc > 0) {
            off += (size_t)rc;
            continue;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;   /* 우리 BIO 는 항상 다 삼키므로 사실상 안 온다 */
        map_err(t, rc);
        return -1;
    }
    return 0;
}

const char *mir_tls_error(const mir_tls *t)
{
    return t ? t->err : "";
}
