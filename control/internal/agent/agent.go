// Package agent — 데이터플레인 인스턴스의 사이드카.
//
// 역할은 **릴레이, 그리고 그 이상은 하지 않기**다. 판정도 집계도 제어부가 한다.
//
//	C 데이터플레인 --(③ unix socket)--> agent --(④ gRPC)--> 제어부
//
// gRPC 를 C 에 직접 넣지 않은 이유는 성능이 아니라 스레드 격리다. C 프로세스에
// gRPC 스레드 풀이 생기면 컨테이너 cpuset(= worker 코어 포함)을 떠돌며
// busy-poll 루프를 선점할 수 있다. 사이드카는 별도 컨테이너라 애초에 worker
// 코어에 올라갈 물리적 경로가 없다 — 규약이 아니라 구조로 막는다.
package agent

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"sync"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/health"
	healthpb "google.golang.org/grpc/health/grpc_health_v1"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"

	"mir/internal/ipc"
	"mir/internal/pb"
)

// ServiceName 은 헬스 체크에 등록하는 이름이다. grpc_health_probe 와
// compose 의 healthcheck 가 이 이름을 쓴다.
const ServiceName = "mir.v1.DataPlane"

// probeVersion 은 사이드카가 스스로 던지는 준비 확인 Hello 의 표식이다.
// 제어부의 Hello 와 로그에서 구분된다.
const probeVersion = "agent-probe"

const (
	dialTimeout = 2 * time.Second
	callTimeout = 3 * time.Second

	minBackoff = 200 * time.Millisecond
	maxBackoff = 5 * time.Second

	// 구독자 버퍼. 넘치면 블로킹하지 않고 버린다 — 느린 GUI 하나가
	// 릴레이 전체를 멈추게 두지 않는다.
	subBuffer = 32
)

// Agent 는 pb.DataPlaneServer 를 구현한다.
type Agent struct {
	pb.UnimplementedDataPlaneServer

	sockPath string
	log      *slog.Logger
	health   *health.Server

	mu   sync.RWMutex
	conn *ipc.Conn

	// 프레임에 request id 가 없으므로 한 번에 하나의 요청만 in-flight 로 둔다.
	// 명령은 초당 수 건이라 이 단순화의 대가가 없다. 동시 요청이 필요해지면
	// proto 에 correlation id 를 넣어야 한다.
	callMu  sync.Mutex
	replies chan replyFrame

	telemetry *hub[*pb.TelemetrySnapshot]
	events    *hub[*pb.Event]
}

type replyFrame struct {
	typ     pb.MsgType
	payload []byte
}

func New(sockPath string, log *slog.Logger, h *health.Server) *Agent {
	return &Agent{
		sockPath:  sockPath,
		log:       log,
		health:    h,
		replies:   make(chan replyFrame, 1),
		telemetry: newHub[*pb.TelemetrySnapshot](),
		events:    newHub[*pb.Event](),
	}
}

// ───────────────────────────────────────────────────────────
// 연결 관리
// ───────────────────────────────────────────────────────────

// Run 은 데이터플레인에 연결하고, 끊기면 백오프 재시도한다. ctx 가 끝날 때까지 돈다.
func (a *Agent) Run(ctx context.Context) {
	backoff := minBackoff

	for ctx.Err() == nil {
		conn, err := ipc.Dial(a.sockPath, dialTimeout)
		if err != nil {
			// C 는 rte_eal_init 에 수 초가 걸린다. 그 동안 소켓이 없는 것은
			// 오류가 아니라 정상 기동 과정이므로 Debug 로만 남긴다.
			a.setConn(nil)
			a.log.Debug("데이터플레인 대기 중", "sock", a.sockPath, "err", err)

			select {
			case <-ctx.Done():
				return
			case <-time.After(backoff):
			}
			backoff = min(backoff*2, maxBackoff)
			continue
		}

		a.log.Info("데이터플레인 연결됨", "sock", a.sockPath)
		a.setConn(conn)
		backoff = minBackoff

		// 준비 확인은 readLoop 이 돌고 있어야 응답을 받을 수 있으므로
		// 별도 고루틴에서 한다. 연결 단위로 취소해 이전 연결의 확인이
		// 다음 연결의 상태를 건드리지 못하게 묶는다.
		connCtx, cancelConn := context.WithCancel(ctx)
		go a.probeReady(connCtx)

		a.readLoop(ctx, conn)

		cancelConn()
		a.setConn(nil)
		_ = conn.Close()
		if ctx.Err() == nil {
			a.log.Warn("데이터플레인 연결 끊김 — 재연결 시도")
		}
	}
}

func (a *Agent) setConn(c *ipc.Conn) {
	a.mu.Lock()
	a.conn = c
	a.mu.Unlock()

	// 연결이 끊기면 즉시 NOT_SERVING. 반대로 **연결만으로 SERVING 이 되지는
	// 않는다** — 포트를 실제로 쥐었는지는 probeReady 가 확인한다.
	if c == nil {
		a.setServing(false)
	}
}

func (a *Agent) setServing(ok bool) {
	st := healthpb.HealthCheckResponse_NOT_SERVING
	if ok {
		st = healthpb.HealthCheckResponse_SERVING
	}
	a.health.SetServingStatus(ServiceName, st)
	a.health.SetServingStatus("", st)
}

// probeReady 는 연결 직후 스스로 Hello 를 던져 데이터플레인이 포트를 실제로
// 쥐었는지 확인한 뒤에만 SERVING 으로 올린다.
//
// 소켓이 붙었다는 건 C 프로세스가 살아 있다는 뜻일 뿐이다. main.c 는 포트를
// 하나도 못 잡아도 경고만 남기고 계속 도는 설계라(설정 실수를 조용히 넘기지
// 않으려고 일부러 그렇게 뒀다), 여기서 거르지 않으면 BDF 를 잘못 준 인스턴스가
// "정상"으로 보고되고 제어부가 트래픽을 낼 수 없는 곳에 시나리오를 배정한다.
func (a *Agent) probeReady(ctx context.Context) {
	backoff := minBackoff

	for ctx.Err() == nil {
		hctx, cancel := context.WithTimeout(ctx, callTimeout)
		resp, err := a.Hello(hctx, &pb.HelloRequest{ControlVersion: probeVersion})
		cancel()

		if err == nil {
			// 포트 집합은 rte_eal_init 시점에 확정되고 나중에 늘지 않는다.
			// 그래서 0개는 재시도할 값이 아니라 그대로 실패다.
			if len(resp.Ports) == 0 {
				a.log.Error("데이터플레인이 포트를 하나도 잡지 못했다 — NOT_SERVING 유지",
					"hint", "vfio 바인딩과 MIR_DEVICE_SPEC 를 확인할 것")
				return
			}

			a.log.Info("데이터플레인 준비 확인",
				"ports", len(resp.Ports),
				"lcores", len(resp.Lcores),
				"version", resp.DataplaneVersion)
			a.setServing(true)
			return
		}

		a.log.Debug("준비 확인 실패 — 재시도", "err", err)

		select {
		case <-ctx.Done():
			return
		case <-time.After(backoff):
		}
		backoff = min(backoff*2, maxBackoff)
	}
}

func (a *Agent) readLoop(ctx context.Context, conn *ipc.Conn) {
	for {
		typ, payload, err := conn.Read()
		if err != nil {
			if ctx.Err() == nil && !errors.Is(err, io.EOF) {
				a.log.Warn("프레임 읽기 실패", "err", err)
			}
			return
		}

		switch typ {
		case pb.MsgType_MSG_TYPE_TELEMETRY:
			snap := &pb.TelemetrySnapshot{}
			if err := proto.Unmarshal(payload, snap); err != nil {
				a.log.Warn("텔레메트리 디코드 실패", "err", err)
				continue
			}
			if n := a.telemetry.publish(snap); n > 0 {
				a.log.Warn("구독자가 느려 텔레메트리 드롭", "dropped", n)
			}

		case pb.MsgType_MSG_TYPE_EVENT:
			ev := &pb.Event{}
			if err := proto.Unmarshal(payload, ev); err != nil {
				a.log.Warn("이벤트 디코드 실패", "err", err)
				continue
			}
			if n := a.events.publish(ev); n > 0 {
				a.log.Warn("구독자가 느려 이벤트 드롭", "dropped", n)
			}

		case pb.MsgType_MSG_TYPE_HELLO_RESPONSE, pb.MsgType_MSG_TYPE_ACK:
			select {
			case a.replies <- replyFrame{typ: typ, payload: payload}:
			default:
				// 대기 중인 호출이 없다 = 이전 요청이 타임아웃된 뒤 늦게 온 응답.
				a.log.Warn("주인 없는 응답 — 버림", "type", typ.String())
			}

		default:
			a.log.Warn("알 수 없는 프레임 타입", "type", int32(typ))
		}
	}
}

// call 은 요청을 보내고 기대한 타입의 응답을 기다린다.
func (a *Agent) call(
	ctx context.Context,
	reqType pb.MsgType, req proto.Message,
	wantType pb.MsgType, resp proto.Message,
) error {
	a.mu.RLock()
	conn := a.conn
	a.mu.RUnlock()

	if conn == nil {
		return status.Error(codes.Unavailable, "데이터플레인에 연결되지 않음")
	}

	a.callMu.Lock()
	defer a.callMu.Unlock()

	// 이전 호출이 타임아웃된 뒤 늦게 도착한 응답이 남아 있을 수 있다.
	drain(a.replies)

	if err := conn.Send(reqType, req); err != nil {
		return status.Errorf(codes.Unavailable, "요청 전송 실패: %v", err)
	}

	select {
	case <-ctx.Done():
		return status.FromContextError(ctx.Err()).Err()

	case <-time.After(callTimeout):
		return status.Error(codes.DeadlineExceeded, "데이터플레인이 응답하지 않음")

	case r := <-a.replies:
		if r.typ != wantType {
			return status.Errorf(codes.Internal,
				"예상치 못한 응답 타입 %s (기대 %s)", r.typ.String(), wantType.String())
		}
		if err := proto.Unmarshal(r.payload, resp); err != nil {
			return status.Errorf(codes.Internal, "응답 디코드 실패: %v", err)
		}
		return nil
	}
}

func drain(ch chan replyFrame) {
	for {
		select {
		case <-ch:
		default:
			return
		}
	}
}

// ───────────────────────────────────────────────────────────
// pb.DataPlaneServer
// ───────────────────────────────────────────────────────────

func (a *Agent) Hello(ctx context.Context, req *pb.HelloRequest) (*pb.HelloResponse, error) {
	resp := &pb.HelloResponse{}
	if err := a.call(ctx,
		pb.MsgType_MSG_TYPE_HELLO_REQUEST, req,
		pb.MsgType_MSG_TYPE_HELLO_RESPONSE, resp); err != nil {
		return nil, err
	}
	return resp, nil
}

func (a *Agent) StartScenario(ctx context.Context, req *pb.StartScenarioRequest) (*pb.Ack, error) {
	ack := &pb.Ack{}
	if err := a.call(ctx,
		pb.MsgType_MSG_TYPE_START_SCENARIO, req,
		pb.MsgType_MSG_TYPE_ACK, ack); err != nil {
		return nil, err
	}
	return ack, nil
}

func (a *Agent) StopScenario(ctx context.Context, req *pb.StopScenarioRequest) (*pb.Ack, error) {
	ack := &pb.Ack{}
	if err := a.call(ctx,
		pb.MsgType_MSG_TYPE_STOP_SCENARIO, req,
		pb.MsgType_MSG_TYPE_ACK, ack); err != nil {
		return nil, err
	}
	return ack, nil
}

func (a *Agent) StreamTelemetry(
	_ *pb.StreamTelemetryRequest,
	srv grpc.ServerStreamingServer[pb.TelemetrySnapshot],
) error {
	return stream(srv.Context(), a.telemetry, srv.Send)
}

func (a *Agent) StreamEvents(
	_ *pb.StreamEventsRequest,
	srv grpc.ServerStreamingServer[pb.Event],
) error {
	return stream(srv.Context(), a.events, srv.Send)
}

func stream[T any](ctx context.Context, h *hub[T], send func(T) error) error {
	ch := h.subscribe()
	defer h.unsubscribe(ch)

	for {
		select {
		case <-ctx.Done():
			return nil
		case v, ok := <-ch:
			if !ok {
				return nil
			}
			if err := send(v); err != nil {
				return err
			}
		}
	}
}

// ───────────────────────────────────────────────────────────
// 브로드캐스트 허브
// ───────────────────────────────────────────────────────────

type hub[T any] struct {
	mu   sync.Mutex
	subs map[chan T]struct{}
}

func newHub[T any]() *hub[T] {
	return &hub[T]{subs: make(map[chan T]struct{})}
}

func (h *hub[T]) subscribe() chan T {
	ch := make(chan T, subBuffer)

	h.mu.Lock()
	defer h.mu.Unlock()
	h.subs[ch] = struct{}{}
	return ch
}

func (h *hub[T]) unsubscribe(ch chan T) {
	h.mu.Lock()
	defer h.mu.Unlock()

	if _, ok := h.subs[ch]; ok {
		delete(h.subs, ch)
		close(ch)
	}
}

// publish 는 드롭한 구독자 수를 돌려준다.
// 느린 구독자 때문에 릴레이가 멈추면 안 되므로 절대 블로킹하지 않는다 —
// C 쪽 rte_ring 이 full 일 때 버리는 것과 같은 원칙이다.
func (h *hub[T]) publish(v T) int {
	h.mu.Lock()
	defer h.mu.Unlock()

	dropped := 0
	for ch := range h.subs {
		select {
		case ch <- v:
		default:
			dropped++
		}
	}
	return dropped
}
