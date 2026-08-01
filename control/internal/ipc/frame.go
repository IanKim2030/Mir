// Package ipc — 파드 내부 hop(채널 ③)의 와이어 포맷과 연결 관리.
//
// C 데이터플레인의 dataplane/src/ipc_server.c 와 **반드시 일치**해야 한다.
//
//	+--------+--------+------------------+
//	| u32 be | u16 be |     payload      |
//	| length |  type  |    (protobuf)    |
//	+--------+--------+------------------+
//
//	length = sizeof(type) + len(payload)   — 자기 자신은 제외
//
// 이 hop 은 네트워크가 아니라 같은 파드 안의 unix socket 이다. 파티션도 TLS 도
// 없고 두 컨테이너가 생명주기를 공유하므로, 직접 짠 프레이밍이 감당할 만하다.
// 파드 밖으로 나가는 hop(④)은 gRPC 가 담당한다.
package ipc

import (
	"encoding/binary"
	"fmt"
	"io"

	"mir/internal/pb"
)

const (
	// HeaderLen 은 u32 length + u16 type.
	HeaderLen = 6

	// MaxPayload — 벌크 데이터(PCAP 등)는 이 채널이 아니라 공유 볼륨으로
	// 가므로 1 MiB 면 충분하다. 상한이 없으면 손상된 헤더 하나가 거대한
	// 할당을 유발한다.
	MaxPayload = 1 << 20

	typeLen = 2
)

// WriteFrame 은 한 프레임을 기록한다.
func WriteFrame(w io.Writer, typ pb.MsgType, payload []byte) error {
	if len(payload) > MaxPayload {
		return fmt.Errorf("ipc: 프레임이 너무 크다 (%d > %d)", len(payload), MaxPayload)
	}

	buf := make([]byte, HeaderLen+len(payload))
	binary.BigEndian.PutUint32(buf[0:4], uint32(typeLen+len(payload)))
	binary.BigEndian.PutUint16(buf[4:6], uint16(typ))
	copy(buf[HeaderLen:], payload)

	// 헤더와 payload 를 한 번에 쓴다. 두 번 나눠 쓰면 상대가 헤더만 읽고
	// 멈춘 상태에서 부분 프레임을 보게 될 여지가 생긴다.
	_, err := w.Write(buf)
	return err
}

// ReadFrame 은 한 프레임을 읽는다. 정상 종료 시 io.EOF 를 돌려준다.
func ReadFrame(r io.Reader) (pb.MsgType, []byte, error) {
	var hdr [HeaderLen]byte
	if _, err := io.ReadFull(r, hdr[:]); err != nil {
		return 0, nil, err
	}

	total := binary.BigEndian.Uint32(hdr[0:4])
	typ := pb.MsgType(binary.BigEndian.Uint16(hdr[4:6]))

	if total < typeLen {
		return 0, nil, fmt.Errorf("ipc: 프레임 길이가 헤더보다 짧다 (%d)", total)
	}
	plen := int(total - typeLen)
	if plen > MaxPayload {
		return 0, nil, fmt.Errorf("ipc: 프레임이 너무 크다 (%d > %d)", plen, MaxPayload)
	}
	if plen == 0 {
		return typ, nil, nil
	}

	payload := make([]byte, plen)
	if _, err := io.ReadFull(r, payload); err != nil {
		return 0, nil, err
	}
	return typ, payload, nil
}
