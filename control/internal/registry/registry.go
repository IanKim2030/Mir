// Package registry — 제어부가 N개 데이터플레인 사이드카에 붙어 있는 상태를 관리한다.
//
// headless Service 의 EndpointSlice 를 주기적으로 조회해(reconcile) 연결 집합을
// 맞춘다. 데이터플레인이 리소스 변경으로 롤링 재생성되면 주소가 바뀌는데,
// 여기서 자동으로 끊고 다시 붙기 때문에 **재시작 후 복구에 별도 처리가 필요 없다**.
//
// 지금은 5초 주기 reconcile 이다. informer 로 바꾸면 지연이 즉시로 줄지만,
// 이 규모에서는 사용자가 체감할 차이가 없어 단순한 쪽을 택했다.
package registry

import (
	"context"
	"log/slog"
	"sync"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"

	discoveryv1 "k8s.io/api/discovery/v1"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/client-go/kubernetes"

	"mir/internal/pb"
)

const (
	reconcileInterval = 5 * time.Second
	helloTimeout      = 5 * time.Second
	retryBackoff      = 2 * time.Second
)

// Status 는 하나의 데이터플레인에 대한 제어부 측 관측 결과다.
type Status struct {
	Pod       string                `json:"pod"`
	Addr      string                `json:"addr"`
	Connected bool                  `json:"connected"`
	Hello     *pb.HelloResponse     `json:"-"`
	Telemetry *pb.TelemetrySnapshot `json:"-"`
	LastSeen  time.Time             `json:"lastSeen"`
}

type Registry struct {
	cs        kubernetes.Interface
	namespace string
	service   string
	port      string
	version   string
	log       *slog.Logger

	mu    sync.RWMutex
	peers map[string]*peer // key = "ip:port"
}

func New(cs kubernetes.Interface, namespace, service, port, version string, log *slog.Logger) *Registry {
	return &Registry{
		cs:        cs,
		namespace: namespace,
		service:   service,
		port:      port,
		version:   version,
		log:       log,
		peers:     make(map[string]*peer),
	}
}

type peer struct {
	addr   string
	pod    string
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
	slices, err := r.cs.DiscoveryV1().EndpointSlices(r.namespace).List(ctx, metav1.ListOptions{
		LabelSelector: discoveryv1.LabelServiceName + "=" + r.service,
	})
	if err != nil {
		r.log.Warn("EndpointSlice 조회 실패", "err", err)
		return
	}

	want := map[string]string{} // addr -> pod 이름
	for i := range slices.Items {
		for _, ep := range slices.Items[i].Endpoints {
			// Ready 가 아닌 엔드포인트는 건너뛴다. 사이드카는 C 데이터플레인이
			// 붙기 전까지 NOT_SERVING 이므로, EAL 초기화 중인 파드에는
			// 헛되이 연결하지 않게 된다.
			if ep.Conditions.Ready != nil && !*ep.Conditions.Ready {
				continue
			}
			pod := ""
			if ep.TargetRef != nil {
				pod = ep.TargetRef.Name
			}
			for _, addr := range ep.Addresses {
				want[addr+":"+r.port] = pod
			}
		}
	}

	r.mu.Lock()
	defer r.mu.Unlock()

	for addr, p := range r.peers {
		if _, ok := want[addr]; !ok {
			r.log.Info("데이터플레인 연결 해제", "pod", p.pod, "addr", addr)
			p.close()
			delete(r.peers, addr)
		}
	}

	for addr, pod := range want {
		if _, ok := r.peers[addr]; ok {
			continue
		}
		p, err := r.dial(ctx, addr, pod)
		if err != nil {
			r.log.Warn("데이터플레인 연결 실패", "addr", addr, "err", err)
			continue
		}
		r.peers[addr] = p
		r.log.Info("데이터플레인 연결 시작", "pod", pod, "addr", addr)
	}
}

func (r *Registry) dial(ctx context.Context, addr, pod string) (*peer, error) {
	// passthrough 를 명시한다. 기본 dns 리졸버는 파드 IP 를 도메인으로
	// 해석하려 들어 불필요한 조회를 만든다.
	cc, err := grpc.NewClient("passthrough:///"+addr,
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		return nil, err
	}

	pctx, cancel := context.WithCancel(ctx)
	p := &peer{
		addr:   addr,
		pod:    pod,
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
			Pod:       p.pod,
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

// ByPod 는 파드 이름으로 색인한 상태를 돌려준다 (k8s 파드 목록과 병합용).
func (r *Registry) ByPod() map[string]Status {
	out := map[string]Status{}
	for _, s := range r.List() {
		if s.Pod != "" {
			out[s.Pod] = s
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
		log.Debug("hello 실패", "pod", p.pod, "err", err)
		return false
	}

	p.mu.Lock()
	p.hello = hello
	p.connected = true
	p.lastSeen = time.Now()
	p.mu.Unlock()

	log.Info("데이터플레인 준비됨",
		"pod", p.pod, "ports", len(hello.Ports),
		"lcores", len(hello.Lcores), "version", hello.DataplaneVersion)

	stream, err := p.client.StreamTelemetry(ctx, &pb.StreamTelemetryRequest{})
	if err != nil {
		p.setConnected(false)
		return false
	}

	for {
		snap, err := stream.Recv()
		if err != nil {
			p.setConnected(false)
			if ctx.Err() == nil {
				log.Debug("텔레메트리 스트림 종료", "pod", p.pod, "err", err)
			}
			return false
		}
		p.mu.Lock()
		p.telemetry = snap
		p.lastSeen = time.Now()
		p.mu.Unlock()
	}
}

func (p *peer) setConnected(v bool) {
	p.mu.Lock()
	p.connected = v
	p.mu.Unlock()
}
