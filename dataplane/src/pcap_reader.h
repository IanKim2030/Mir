/*
 * pcap_reader — 백엔드 독립적인 PCAP / PCAPNG 파서.
 *
 * classic pcap(.pcap, µs/ns, 양쪽 엔디안)와 pcapng(.pcapng)의 흔한
 * 하위집합(SHB/IDB/EPB/SPB, if_tsresol)을 지원한다. 각 패킷을
 * 나노초 절대 타임스탬프와 링크타입으로 정규화해 순차적으로 돌려준다.
 *
 * 외부 라이브러리 의존성 없음(순수 C99).
 */
#ifndef PCAP_READER_H
#define PCAP_READER_H

#include <stdint.h>
#include <stddef.h>

/* 자주 쓰는 링크타입(LINKTYPE_*). 그대로 L2 전송하려면 ETHERNET 이어야 한다. */
#define LINKTYPE_ETHERNET 1

typedef struct pcap_reader pcap_reader;

typedef struct {
    uint64_t       ts_ns;    /* 캡처 시각(에폭 기준 나노초, 상대 계산용) */
    uint32_t       caplen;   /* data 에 실제로 담긴 바이트 수 */
    uint32_t       origlen;  /* 원래 온-와이어 길이(잘렸을 수 있음) */
    uint32_t       linktype; /* LINKTYPE_* */
    const uint8_t *data;     /* 내부 버퍼 포인터. 다음 next() 호출 전까지만 유효 */
} pcap_packet;

/* 실패 시 NULL 반환하고 err(errlen)로 사유를 채운다. */
pcap_reader *pcap_reader_open(const char *path, char *err, size_t errlen);

/* 1 = 패킷 얻음, 0 = 파일 끝(EOF), -1 = 오류(pcap_reader_error 참고). */
int pcap_reader_next(pcap_reader *r, pcap_packet *out);

/* 가장 최근 오류 메시지("" = 없음). */
const char *pcap_reader_error(const pcap_reader *r);

/* "pcap" 또는 "pcapng". */
const char *pcap_reader_format(const pcap_reader *r);

void pcap_reader_close(pcap_reader *r);

#endif /* PCAP_READER_H */
