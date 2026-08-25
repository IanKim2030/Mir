/*
 * pcap_reader — classic pcap + pcapng 하위집합 파서 (순수 C99).
 *
 * 파일 전체를 메모리로 읽고, 각 패킷의 data 포인터가 그 버퍼를 직접 가리킨다
 * (zero-copy). 그래서 data 는 reader 가 살아 있는 동안 유효하다 — 헤더의
 * "다음 next() 전까지"보다 실제로는 더 오래 산다. 호출자는 문서 계약만 믿으면
 * 안전하고, 리플레이 엔진은 어차피 각 프레임을 자기 버퍼로 복사한다.
 */
#define _GNU_SOURCE

#include "pcap_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 파일 크기 상한. 이보다 크면 열기를 거부한다 — 리플레이는 메모리에 올리는
 * 모델이라 무한정 큰 파일을 받으면 OOM 이 난다. 구간 재전송으로 나눠 쓴다. */
#define MAX_FILE_BYTES (2ull * 1024 * 1024 * 1024)   /* 2 GiB */

typedef enum { FMT_PCAP, FMT_PCAPNG } fmt_t;

struct pcap_reader {
    uint8_t *buf;
    size_t   len;
    size_t   pos;

    fmt_t    fmt;
    int      swap;         /* 파일 엔디안이 호스트와 다른가 */
    int      nsec;         /* classic: 타임스탬프가 ns 인가(아니면 µs) */
    uint32_t linktype;     /* 마지막으로 본 IDB/글로벌 링크타입 */
    uint64_t ts_div;       /* pcapng: if_tsresol 로부터 나온 분모(단위/초) */

    char err[192];
    char format[8];
};

/* ── 바이트 읽기 (엔디안 처리) ─────────────────────────────── */

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8  | (uint32_t)p[3];
}

/* swap 이면 파일이 빅엔디안(호스트 리틀엔디안 기준)이라는 뜻으로 통일한다. */
static uint16_t g16(const uint8_t *p, int swap) { return swap ? be16(p) : le16(p); }
static uint32_t g32(const uint8_t *p, int swap) { return swap ? be32(p) : le32(p); }

static void seterr(pcap_reader *r, const char *msg)
{
    snprintf(r->err, sizeof(r->err), "%s", msg);
}

/* ── 파일 읽기 ─────────────────────────────────────────────── */

static int read_whole_file(const char *path, uint8_t **out, size_t *outlen,
                           char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "파일을 열 수 없다: %s", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        snprintf(err, errlen, "fseek 실패: %s", path);
        fclose(f);
        return -1;
    }
    long sz = ftell(f);
    if (sz < 0) {
        snprintf(err, errlen, "ftell 실패: %s", path);
        fclose(f);
        return -1;
    }
    if ((unsigned long long)sz > MAX_FILE_BYTES) {
        snprintf(err, errlen, "파일이 너무 크다 (%ld B > %llu B 상한)",
                 sz, MAX_FILE_BYTES);
        fclose(f);
        return -1;
    }
    rewind(f);

    uint8_t *buf = malloc((size_t)sz);
    if (!buf) {
        snprintf(err, errlen, "메모리 부족 (%ld B)", sz);
        fclose(f);
        return -1;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        snprintf(err, errlen, "파일 읽기가 짧다: %s", path);
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    *out = buf;
    *outlen = (size_t)sz;
    return 0;
}

/* ── classic pcap ──────────────────────────────────────────── */

static int init_pcap(pcap_reader *r, char *err, size_t errlen)
{
    if (r->len < 24) {
        snprintf(err, errlen, "pcap 글로벌 헤더가 잘렸다");
        return -1;
    }
    uint32_t magic = le32(r->buf);   /* 엔디안 판별 전이라 일단 LE 로 읽는다 */

    switch (magic) {
    case 0xa1b2c3d4: r->swap = 0; r->nsec = 0; break;  /* µs, 동일 엔디안 */
    case 0xd4c3b2a1: r->swap = 1; r->nsec = 0; break;  /* µs, 반대 */
    case 0xa1b23c4d: r->swap = 0; r->nsec = 1; break;  /* ns, 동일 */
    case 0x4d3cb2a1: r->swap = 1; r->nsec = 1; break;  /* ns, 반대 */
    default:
        snprintf(err, errlen, "pcap 매직이 아니다 (0x%08x)", magic);
        return -1;
    }

    r->linktype = g32(r->buf + 20, r->swap);
    r->pos = 24;
    r->fmt = FMT_PCAP;
    snprintf(r->format, sizeof(r->format), "pcap");
    return 0;
}

static int next_pcap(pcap_reader *r, pcap_packet *out)
{
    if (r->pos + 16 > r->len)
        return 0;   /* EOF */

    const uint8_t *h = r->buf + r->pos;
    uint32_t ts_sec  = g32(h + 0, r->swap);
    uint32_t ts_frac = g32(h + 4, r->swap);
    uint32_t caplen  = g32(h + 8, r->swap);
    uint32_t origlen = g32(h + 12, r->swap);

    if (r->pos + 16 + caplen > r->len) {
        seterr(r, "패킷 데이터가 파일 끝을 넘는다");
        return -1;
    }

    out->ts_ns    = (uint64_t)ts_sec * 1000000000ull +
                    (r->nsec ? ts_frac : (uint64_t)ts_frac * 1000ull);
    out->caplen   = caplen;
    out->origlen  = origlen;
    out->linktype = r->linktype;
    out->data     = r->buf + r->pos + 16;

    r->pos += 16 + caplen;
    return 1;
}

/* ── pcapng ────────────────────────────────────────────────── */

static int init_pcapng(pcap_reader *r, char *err, size_t errlen)
{
    /* 첫 블록은 SHB(0x0A0D0D0A). byte-order magic 으로 엔디안을 정한다. */
    if (r->len < 12) {
        snprintf(err, errlen, "pcapng 헤더가 잘렸다");
        return -1;
    }
    /* byte-order magic 은 오프셋 8. 0x1A2B3C4D 를 어느 엔디안으로 읽어야
     * 맞는지로 swap 을 결정한다. */
    uint32_t bom_le = le32(r->buf + 8);
    if (bom_le == 0x1A2B3C4D)
        r->swap = 0;
    else if (be32(r->buf + 8) == 0x1A2B3C4D)
        r->swap = 1;
    else {
        snprintf(err, errlen, "pcapng byte-order magic 이 아니다");
        return -1;
    }

    r->ts_div   = 1000000;   /* if_tsresol 기본값 = 10^-6 (µs) */
    r->pos      = 0;
    r->linktype = 0;
    r->fmt = FMT_PCAPNG;
    snprintf(r->format, sizeof(r->format), "pcapng");
    return 0;
}

/* IDB 옵션에서 if_tsresol(코드 9)을 찾아 ts_div 를 갱신한다. */
static void parse_idb_tsresol(pcap_reader *r, const uint8_t *body, uint32_t blen)
{
    /* IDB body: linktype(2) reserved(2) snaplen(4) 뒤에 옵션들 */
    if (blen < 8)
        return;
    uint32_t off = 8;
    while (off + 4 <= blen) {
        uint16_t code = g16(body + off, r->swap);
        uint16_t olen = g16(body + off + 2, r->swap);
        off += 4;
        if (code == 0)          /* opt_endofopt */
            break;
        if (code == 9 && olen >= 1) {   /* if_tsresol */
            uint8_t v = body[off];
            uint64_t d = 1;
            if (v & 0x80) {     /* 2의 거듭제곱 */
                for (uint8_t i = 0; i < (v & 0x7f); i++) d *= 2;
            } else {            /* 10의 거듭제곱 */
                for (uint8_t i = 0; i < v; i++) d *= 10;
            }
            r->ts_div = d ? d : 1000000;
        }
        off += (olen + 3u) & ~3u;   /* 4바이트 정렬 */
    }
}

static int next_pcapng(pcap_reader *r, pcap_packet *out)
{
    for (;;) {
        if (r->pos + 8 > r->len)
            return 0;   /* EOF */

        uint32_t btype = g32(r->buf + r->pos, r->swap);
        uint32_t blen  = g32(r->buf + r->pos + 4, r->swap);

        if (blen < 12 || r->pos + blen > r->len) {
            seterr(r, "pcapng 블록 길이가 잘못됐다");
            return -1;
        }

        const uint8_t *body = r->buf + r->pos + 8;
        uint32_t bodylen = blen - 12;   /* type+len(8) + trailing len(4) */

        switch (btype) {
        case 0x00000001:   /* IDB */
            if (bodylen >= 8) {
                r->linktype = g16(body, r->swap);
                parse_idb_tsresol(r, body, bodylen);
            }
            break;

        case 0x00000006: { /* EPB */
            if (bodylen < 20) { seterr(r, "EPB 가 짧다"); return -1; }
            uint32_t ts_hi  = g32(body + 4, r->swap);
            uint32_t ts_lo  = g32(body + 8, r->swap);
            uint32_t caplen = g32(body + 12, r->swap);
            uint32_t origlen= g32(body + 16, r->swap);
            if (20u + caplen > bodylen) { seterr(r, "EPB caplen 초과"); return -1; }

            uint64_t ts = ((uint64_t)ts_hi << 32) | ts_lo;
            out->ts_ns    = r->ts_div ? (ts * 1000000000ull / r->ts_div) : 0;
            out->caplen   = caplen;
            out->origlen  = origlen;
            out->linktype = r->linktype;
            out->data     = body + 20;
            r->pos += blen;
            return 1;
        }

        case 0x00000003: { /* SPB — caplen 이 없다, snaplen/전체로 유추 */
            if (bodylen < 4) { seterr(r, "SPB 가 짧다"); return -1; }
            uint32_t origlen = g32(body, r->swap);
            /* SPB 데이터는 body 뒤 전부(정렬 패딩 제외). caplen 은 bodylen-4. */
            uint32_t caplen = bodylen - 4;
            if (caplen > origlen) caplen = origlen;   /* 패딩 보정 */
            out->ts_ns    = 0;   /* SPB 는 타임스탬프가 없다 */
            out->caplen   = caplen;
            out->origlen  = origlen;
            out->linktype = r->linktype;
            out->data     = body + 4;
            r->pos += blen;
            return 1;
        }

        default:
            /* SHB(0x0A0D0D0A) 포함 나머지는 건너뛴다. */
            break;
        }

        r->pos += blen;
    }
}

/* ── 공개 API ──────────────────────────────────────────────── */

pcap_reader *pcap_reader_open(const char *path, char *err, size_t errlen)
{
    pcap_reader *r = calloc(1, sizeof(*r));
    if (!r) {
        snprintf(err, errlen, "메모리 부족");
        return NULL;
    }

    if (read_whole_file(path, &r->buf, &r->len, err, errlen) != 0) {
        free(r);
        return NULL;
    }
    if (r->len < 4) {
        snprintf(err, errlen, "파일이 너무 짧다");
        free(r->buf);
        free(r);
        return NULL;
    }

    uint32_t first = le32(r->buf);
    int rc;
    if (first == 0x0A0D0D0A)
        rc = init_pcapng(r, err, errlen);
    else
        rc = init_pcap(r, err, errlen);

    if (rc != 0) {
        free(r->buf);
        free(r);
        return NULL;
    }
    return r;
}

int pcap_reader_next(pcap_reader *r, pcap_packet *out)
{
    return r->fmt == FMT_PCAPNG ? next_pcapng(r, out) : next_pcap(r, out);
}

const char *pcap_reader_error(const pcap_reader *r)  { return r->err; }
const char *pcap_reader_format(const pcap_reader *r) { return r->format; }

void pcap_reader_close(pcap_reader *r)
{
    if (!r) return;
    free(r->buf);
    free(r);
}
