package registry

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"sort"
	"sync"
	"testing"

	"google.golang.org/grpc/credentials/insecure"
)

// fakeResolver 는 테스트가 주소 집합을 마음대로 바꿀 수 있게 한다.
type fakeResolver struct {
	mu   sync.Mutex
	m    map[string]string
	err  error
	hits int
}

func (f *fakeResolver) Resolve(context.Context) (map[string]string, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.hits++
	if f.err != nil {
		return nil, f.err
	}
	out := make(map[string]string, len(f.m))
	for k, v := range f.m {
		out[k] = v
	}
	return out, nil
}

func (f *fakeResolver) set(m map[string]string) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.m = m
}

func newTestRegistry(f *fakeResolver) (*Registry, context.Context, context.CancelFunc) {
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	ctx, cancel := context.WithCancel(context.Background())
	return New(f, insecure.NewCredentials(), "test", log), ctx, cancel
}

func peerNames(r *Registry) []string {
	r.mu.RLock()
	defer r.mu.RUnlock()

	out := make([]string, 0, len(r.peers))
	for addr := range r.peers {
		out = append(out, addr)
	}
	sort.Strings(out)
	return out
}

func TestReconcileAddsAndRemovesPeers(t *testing.T) {
	f := &fakeResolver{m: map[string]string{
		"10.0.0.1:9100": "gen-1-dp0",
		"10.0.0.1:9101": "gen-1-dp1",
	}}
	r, ctx, cancel := newTestRegistry(f)
	defer cancel()

	r.reconcile(ctx)
	if got, want := len(peerNames(r)), 2; got != want {
		t.Fatalf("peer %d개, 기대 %d개", got, want)
	}

	// 하나가 설정에서 빠지면 연결도 닫혀야 한다.
	f.set(map[string]string{"10.0.0.1:9100": "gen-1-dp0"})
	r.reconcile(ctx)

	got := peerNames(r)
	if len(got) != 1 || got[0] != "10.0.0.1:9100" {
		t.Fatalf("peer = %v, 기대 [10.0.0.1:9100]", got)
	}

	// 새 장비가 붙으면 늘어난다.
	f.set(map[string]string{
		"10.0.0.1:9100": "gen-1-dp0",
		"10.0.0.2:9100": "gen-2-dp0",
	})
	r.reconcile(ctx)

	if got, want := len(peerNames(r)), 2; got != want {
		t.Fatalf("peer %d개, 기대 %d개", got, want)
	}
}

// 같은 주소가 계속 있으면 연결을 새로 만들지 않는다 —
// 5초마다 재연결하면 텔레메트리 스트림이 끊긴다.
func TestReconcileIsStableForUnchangedAddrs(t *testing.T) {
	f := &fakeResolver{m: map[string]string{"10.0.0.1:9100": "gen-1-dp0"}}
	r, ctx, cancel := newTestRegistry(f)
	defer cancel()

	r.reconcile(ctx)
	r.mu.RLock()
	first := r.peers["10.0.0.1:9100"]
	r.mu.RUnlock()

	r.reconcile(ctx)
	r.mu.RLock()
	second := r.peers["10.0.0.1:9100"]
	r.mu.RUnlock()

	if first != second {
		t.Fatal("변화가 없는데 peer 를 다시 만들었다")
	}
}

// resolver 가 실패하면 기존 연결을 건드리지 않는다. 설정 파일을 잠깐 못 읽었다고
// 멀쩡히 돌던 데이터플레인 연결을 전부 끊어서는 안 된다.
func TestResolveErrorKeepsExistingPeers(t *testing.T) {
	f := &fakeResolver{m: map[string]string{"10.0.0.1:9100": "gen-1-dp0"}}
	r, ctx, cancel := newTestRegistry(f)
	defer cancel()

	r.reconcile(ctx)

	f.mu.Lock()
	f.err = errors.New("일시적 실패")
	f.mu.Unlock()
	r.reconcile(ctx)

	if got := peerNames(r); len(got) != 1 {
		t.Fatalf("peer = %v, 기존 연결이 유지돼야 한다", got)
	}
}

func TestByNameIndexesByInstanceName(t *testing.T) {
	f := &fakeResolver{m: map[string]string{"10.0.0.1:9100": "gen-1-dp0"}}
	r, ctx, cancel := newTestRegistry(f)
	defer cancel()

	r.reconcile(ctx)

	byName := r.ByName()
	if _, ok := byName["gen-1-dp0"]; !ok {
		t.Fatalf("ByName 에 gen-1-dp0 이 없다: %v", byName)
	}
}
