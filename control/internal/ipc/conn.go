package ipc

import (
	"net"
	"sync"
	"time"

	"google.golang.org/protobuf/proto"

	"mir/internal/pb"
)

// Conn 은 C 데이터플레인과의 unix socket 연결이다.
//
// 쓰기는 여러 고루틴에서 일어날 수 있어 뮤텍스로 직렬화한다. 읽기는 agent 의
// 단일 reader 고루틴만 수행하므로 잠그지 않는다.
type Conn struct {
	c net.Conn

	wmu sync.Mutex
}

// Dial 은 소켓에 연결한다.
//
// 실패는 **정상 상태**로 취급해야 한다 — C 쪽은 rte_eal_init 에 수 초가 걸리고
// 그 동안 소켓이 존재하지 않는다. 호출자가 백오프 재시도한다.
func Dial(path string, timeout time.Duration) (*Conn, error) {
	c, err := net.DialTimeout("unix", path, timeout)
	if err != nil {
		return nil, err
	}
	return &Conn{c: c}, nil
}

// Send 는 protobuf 메시지를 프레임으로 감싸 보낸다. m 이 nil 이면 빈 payload.
func (c *Conn) Send(typ pb.MsgType, m proto.Message) error {
	var payload []byte
	if m != nil {
		var err error
		payload, err = proto.Marshal(m)
		if err != nil {
			return err
		}
	}

	c.wmu.Lock()
	defer c.wmu.Unlock()
	return WriteFrame(c.c, typ, payload)
}

// Read 는 프레임 하나를 읽는다. 단일 reader 고루틴에서만 호출할 것.
func (c *Conn) Read() (pb.MsgType, []byte, error) {
	return ReadFrame(c.c)
}

func (c *Conn) Close() error { return c.c.Close() }
