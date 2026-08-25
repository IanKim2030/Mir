// Package registry — 제어부가 N개 데이터플레인 사이드카에 붙어 있는 상태를 관리한다.
//
// Resolver 가 알려 주는 주소 집합을 주기적으로 조회해(reconcile) 연결 집합을
// 맞춘다. 인스턴스가 재시작되면 잠시 끊겼다가 다시 붙는데, 여기서 자동으로
// 처리하므로 **재시작 후 복구에 별도 처리가 필요 없다**.
//
// 지금은 5초 주기 reconcile 이다. 주소 출처가 정적 설정이라 더 잦게 돌 이유가
// 없고, 실제 연결 복구는 peer 단위 백오프가 담당한다.
package registry

import (
	"context"
	"fmt"
	"log/slog"
	"sort"
	"sync"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials"

	"mir/internal/pb"
)

const (
	reconcileInterval = 5 * time.Second
	helloTimeout      = 5 * time.Second
	retryBackoff      = 2 * time.Second
)

// Resolver 는 "지금 붙어 있어야 할 데이터플레인" 집합을 알려 준다.
// 반환값은 주소("host:port") → 인스턴스 이름.
//
// 이 한 겹이 제어부를 오케스트레이터로부터 떼어 놓는다 — registry 의 나머지
// (연결·핸드셰이크·스트림·백오프)는 주소가 어디서 왔는지 알 필요가 없다.
type Resolver interface {
	Resolve(ctx context.Context) (map[string]string, error)
}

// Status 는 하나의 데이터플레인에 대한 제어부 측 관측 결과다.
type Status struct {
	Name      string                `json:"name"`
	Addr      string                `json:"addr"`
	Connected bool                  `json:"connected"`
	Hello     *pb.HelloResponse     `json:"-"`
	Telemetry *pb.TelemetrySnapshot `json:"-"`
	LastSeen  time.Time             `json:"lastSeen"`
}

// RecentEvent 는 인스턴스 이름을 붙인 판정 이벤트다.
type RecentEvent struct {
	Instance string    `json:"instance"`
	Event    *pb.Event `json:"-"`
}

// eventBufSize 는 링 버퍼 크기. 이벤트는 이미 데이터플레인에서 레이트 제한돼
// 올라오므로(종류당 5개/초) 이 정도면 최근 수 분을 담는다.
const eventBufSize = 512

type Registry struct {
	resolver Resolver
	creds    credentials.TransportCredentials
	version  string
	log      *slog.Logger

	mu    sync.RWMutex
	peers map[string]*peer // key = "host:port"

	evMu   sync.Mutex
	events []RecentEvent // 링 버퍼
	evNext int           // 다음 쓸 위치
	evSeen uint64        // 총 수신 수 (버퍼를 넘어 흘러간 것 포함)
}

func New(
	resolver Resolver,
	creds credentials.TransportCredentials,
	version string,
	log *slog.Logger,
) *Registry {
	return &Registry{
		resolver: resolver,
		creds:    creds,
		version:  version,
		log:      log,
		peers:    make(map[string]*peer),
		events:   make([]RecentEvent, 0, eventBufSize),
	}
}

// addEvent 는 이벤트를 링 버퍼에 넣는다 (peer 의 이벤트 고루틴에서 호출).
func (r *Registry) addEvent(instance string, ev *pb.Event) {
	r.evMu.Lock()
	defer r.evMu.Unlock()

	r.evSeen++
	re := RecentEvent{Instance: instance, Event: ev}
	if len(r.events) < eventBufSize {
		r.events = append(r.events, re)
		return
	}
	r.events[r.evNext] = re
	r.evNext = (r.evNext + 1) % eventBufSize
}

// Events 는 최근 이벤트를 시간순(오래된 것 → 최신)으로 돌려준다.
func (r *Registry) Events() ([]RecentEvent, uint64) {
	r.evMu.Lock()
	defer r.evMu.Unlock()

	out := make([]RecentEvent, 0, len(r.events))
	if len(r.events) < eventBufSize {
		out = append(out, r.events...)
	} else {
		// 링이 꽉 찼으면 evNext 부터가 가장 오래된 것이다.
		out = append(out, r.events[r.evNext:]...)
		out = append(out, r.events[:r.evNext]...)
	}
	return out, r.evSeen
}

type peer struct {
	reg    *Registry
	addr   string
	name   string
	cc     *grpc.ClientConn
	client pb.DataPlaneClient
	cancel context.CancelFunc

	mu        sync.RWMutex
	connected bool
	hello     *pb.HelloResponse
	telemetry *pb.TelemetrySnapshot
	lastSeen  time.Time
}

// Run 은 ctx 가 끝날 때까지 연결 집합을 조정한다.
func (r *Registry) Run(ctx context.Context) {
	t := time.NewTicker(reconcileInterval)
	defer t.Stop()

	r.reconcile(ctx)
	for {
		select {
		case <-ctx.Done():
			r.closeAll()
			return
		case <-t.C:
			r.reconcile(ctx)
		}
	}
}

func (r *Registry) reconcile(ctx context.Context) {
	want, err := r.resolver.Resolve(ctx)
	if err != nil {
		r.log.Warn("인스턴스 목록 조회 실패", "err", err)
		return
	}

	r.mu.Lock()
	defer r.mu.Unlock()

	for addr, p := range r.peers {
		if _, ok := want[addr]; !ok {
			r.log.Info("데이터플레인 연결 해제", "instance", p.name, "addr", addr)
			p.close()
			delete(r.peers, addr)
		}
	}

	for addr, name := range want {
		if _, ok := r.peers[addr]; ok {
			continue
		}
		p, err := r.dial(ctx, addr, name)
		if err != nil {
			r.log.Warn("데이터플레인 연결 실패", "addr", addr, "err", err)
			continue
		}
		r.peers[addr] = p
		r.log.Info("데이터플레인 연결 시작", "instance", name, "addr", addr)
	}
}

func (r *Registry) dial(ctx context.Context, addr, name string) (*peer, error) {
	// passthrough 를 명시해 gRPC 단계의 이름 해석을 끈다. 주소 하나가
	// 백엔드 하나로 고정되는 게 여기서는 정확히 원하는 동작이고, 실제
	// 호스트명 해석은 다이얼 시점에 net 이 알아서 한다.
	cc, err := grpc.NewClient("passthrough:///"+addr,
		grpc.WithTransportCredentials(r.creds))
	if err != nil {
		return nil, err
	}

	pctx, cancel := context.WithCancel(ctx)
	p := &peer{
		reg:    r,
		addr:   addr,
		name:   name,
		cc:     cc,
		client: pb.NewDataPlaneClient(cc),
		cancel: cancel,
	}
	go p.run(pctx, r.version, r.log)
	return p, nil
}

func (r *Registry) closeAll() {
	r.mu.Lock()
	defer r.mu.Unlock()
	for addr, p := range r.peers {
		p.close()
		delete(r.peers, addr)
	}
}

// List 는 현재 관측 상태의 스냅샷을 돌려준다.
func (r *Registry) List() []Status {
	r.mu.RLock()
	defer r.mu.RUnlock()

	out := make([]Status, 0, len(r.peers))
	for _, p := range r.peers {
		p.mu.RLock()
		out = append(out, Status{
			Name:      p.name,
			Addr:      p.addr,
			Connected: p.connected,
			Hello:     p.hello,
			Telemetry: p.telemetry,
			LastSeen:  p.lastSeen,
		})
		p.mu.RUnlock()
	}
	return out
}

// ByName 은 인스턴스 이름으로 색인한 상태를 돌려준다
// (함대 설정의 기대 인벤토리와 대조할 때 쓴다).
func (r *Registry) ByName() map[string]Status {
	out := map[string]Status{}
	for _, s := range r.List() {
		if s.Name != "" {
			out[s.Name] = s
		}
	}
	return out
}

// ───────────────────────────────────────────────────────────

func (p *peer) close() {
	p.cancel()
	_ = p.cc.Close()
}

func (p *peer) run(ctx context.Context, version string, log *slog.Logger) {
	for ctx.Err() == nil {
		if !p.handshakeAndStream(ctx, version, log) {
			select {
			case <-ctx.Done():
				return
			case <-time.After(retryBackoff):
			}
		}
	}
}

// 반환값은 "정상적으로 스트림을 소비했는가"다. false 면 호출자가 백오프한다.
func (p *peer) handshakeAndStream(ctx context.Context, version string, log *slog.Logger) bool {
	hctx, cancel := context.WithTimeout(ctx, helloTimeout)
	hello, err := p.client.Hello(hctx, &pb.HelloRequest{ControlVersion: version})
	cancel()
	if err != nil {
		p.setConnected(false)
		log.Debug("hello 실패", "instance", p.name, "err", err)
		return false
	}

	p.mu.Lock()
	p.hello = hello
	p.connected = true
	p.lastSeen = time.Now()
	p.mu.Unlock()

	log.Info("데이터플레인 준비됨",
		"instance", p.name, "ports", len(hello.Ports),
		"lcores", len(hello.Lcores), "version", hello.DataplaneVersion)

	stream, err := p.client.StreamTelemetry(ctx, &pb.StreamTelemetryRequest{})
	if err != nil {
		p.setConnected(false)
		return false
	}

	// 이벤트 스트림은 별도 고루틴에서 소비한다. 텔레메트리 스트림이 끊기면
	// 이 attempt 를 취소해 이벤트 고루틴도 함께 정리한다 — 두 스트림의
	// 수명을 한 attempt 로 묶는다.
	attemptCtx, cancelAttempt := context.WithCancel(ctx)
	defer cancelAttempt()
	go p.consumeEvents(attemptCtx, log)

	for {
		snap, err := stream.Recv()
		if err != nil {
			p.setConnected(false)
			if ctx.Err() == nil {
				log.Debug("텔레메트리 스트림 종료", "instance", p.name, "err", err)
			}
			return false
		}
		p.mu.Lock()
		p.telemetry = snap
		p.lastSeen = time.Now()
		p.mu.Unlock()
	}
}

// consumeEvents 는 판정 이벤트 스트림을 registry 의 링 버퍼로 흘린다.
// 실패는 치명적이지 않다 — 이벤트는 표본이고, 텔레메트리 쪽이 재연결을 주도한다.
func (p *peer) consumeEvents(ctx context.Context, log *slog.Logger) {
	stream, err := p.client.StreamEvents(ctx, &pb.StreamEventsRequest{})
	if err != nil {
		return
	}
	for {
		ev, err := stream.Recv()
		if err != nil {
			return
		}
		p.reg.addEvent(p.name, ev)
		log.Debug("판정 이벤트",
			"instance", p.name, "kind", ev.Kind.String(), "flow", ev.FlowKey)
	}
}

func (p *peer) setConnected(v bool) {
	p.mu.Lock()
	p.connected = v
	p.mu.Unlock()
}

// ───────────────────────────────────────────────────────────
// 명령 — 시나리오 시작/정지
//
// 개별 패킷과 달리 이건 초당 수 건짜리 제어 평면이라 gRPC 왕복이 문제되지 않는다.
// ───────────────────────────────────────────────────────────

// ErrNotFound 는 이름에 해당하는 인스턴스가 연결 집합에 없을 때다.
type ErrNotFound struct{ Name string }

func (e *ErrNotFound) Error() string {
	return fmt.Sprintf("인스턴스 %q 가 연결돼 있지 않다", e.Name)
}

func (r *Registry) peerByName(name string) (*peer, error) {
	r.mu.RLock()
	defer r.mu.RUnlock()

	for _, p := range r.peers {
		if p.name == name {
			return p, nil
		}
	}
	return nil, &ErrNotFound{Name: name}
}

// StartScenario 는 인스턴스 하나에 시작 명령을 보낸다.
func (r *Registry) StartScenario(
	ctx context.Context, name string, req *pb.StartScenarioRequest,
) (*pb.Ack, error) {
	p, err := r.peerByName(name)
	if err != nil {
		return nil, err
	}
	return p.client.StartScenario(ctx, req)
}

// StopScenario 는 인스턴스 하나에 정지 명령을 보낸다.
func (r *Registry) StopScenario(
	ctx context.Context, name string, req *pb.StopScenarioRequest,
) (*pb.Ack, error) {
	p, err := r.peerByName(name)
	if err != nil {
		return nil, err
	}
	return p.client.StopScenario(ctx, req)
}

// Names 는 현재 붙어 있는 인스턴스 이름을 돌려준다 (대상 미지정 시 전체 배포용).
func (r *Registry) Names() []string {
	r.mu.RLock()
	defer r.mu.RUnlock()

	out := make([]string, 0, len(r.peers))
	for _, p := range r.peers {
		if p.name != "" {
			out = append(out, p.name)
		}
	}
	sort.Strings(out)
	return out
}
