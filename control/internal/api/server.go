// Package api — GUI 가 호출하는 REST 계층.
//
// 이번 단계에서는 핸들러까지만 만들고 화면은 붙이지 않는다 (Phase 6).
//
// **제어부는 인스턴스의 라이프사이클을 조종하지 않는다.** 개수·리소스 변경과
// 재시작은 compose 파일과 운영 창구가 소유한다. 제어부가 그걸 하려면 장비마다
// docker.sock(= root 등가)을 받아야 하는데, GUI 가 웹으로 노출되는 구조에서
// 그건 명백한 후퇴이고 멀티 장비에서는 애초에 성립하지 않는다(소켓은 장비
// 로컬, 제어부는 원격). 그래서 여기 남는 것은 **관측과 시나리오**뿐이다.
package api

import (
	"encoding/json"
	"log/slog"
	"net/http"
	"strings"
	"time"

	"mir/internal/fleet"
	"mir/internal/pb"
	"mir/internal/registry"
)

// 인스턴스 상태. 기대(함대 설정)와 실측(HelloResponse)을 대조한 결과다.
const (
	stateOK          = "ok"
	stateUnreachable = "unreachable" // 연결 자체가 안 된다 — 컨테이너 다운/방화벽
	stateConnecting  = "connecting"  // 붙었지만 아직 Hello 를 못 받았다
	stateNoPorts     = "no-ports"    // 떠 있는데 NIC 을 하나도 못 잡았다
	statePFMismatch  = "pf-mismatch" // 설정과 다른 PF 를 잡았다
)

type Server struct {
	fleet   *fleet.Config
	reg     *registry.Registry
	version string
	log     *slog.Logger
}

func New(fc *fleet.Config, reg *registry.Registry, version string, log *slog.Logger) *Server {
	return &Server{fleet: fc, reg: reg, version: version, log: log}
}

func (s *Server) Routes() http.Handler {
	mux := http.NewServeMux()

	mux.HandleFunc("GET /healthz", s.healthz)
	mux.HandleFunc("GET /readyz", s.readyz)
	mux.HandleFunc("GET /api/capacity", s.capacity)
	mux.HandleFunc("GET /api/dataplanes", s.listDataplanes)

	return mux
}

// ───────────────────────────────────────────────────────────
// health
// ───────────────────────────────────────────────────────────

// healthz 는 **liveness** 다. 프로세스가 응답할 수 있으면 200.
// 함대 상태를 여기 섞으면 안 된다 — 데이터플레인이 안 떴다고 제어부를
// 재시작해 봐야 나아질 게 없고, 진단 창구만 사라진다.
func (s *Server) healthz(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{
		"ok":      true,
		"version": s.version,
	})
}

// readyz 는 **readiness** 다. 기대한 인스턴스가 전부 붙어 있을 때만 200,
// 아니면 503 과 함께 어느 인스턴스가 빠졌는지 알려 준다.
func (s *Server) readyz(w http.ResponseWriter, _ *http.Request) {
	views := s.instanceViews()

	missing := make([]string, 0)
	for _, v := range views {
		if v.State != stateOK {
			missing = append(missing, v.Name+"("+v.State+")")
		}
	}

	code := http.StatusOK
	if len(missing) > 0 {
		code = http.StatusServiceUnavailable
	}

	writeJSON(w, code, map[string]any{
		"ok":       len(missing) == 0,
		"version":  s.version,
		"expected": len(views),
		"ready":    len(views) - len(missing),
		"missing":  missing,
	})
}

// ───────────────────────────────────────────────────────────
// 인벤토리
// ───────────────────────────────────────────────────────────

type portView struct {
	PortID     uint32 `json:"portId"`
	Driver     string `json:"driver"`
	MAC        string `json:"mac"`
	NumaNode   int32  `json:"numaNode"`
	DeviceSpec string `json:"deviceSpec"`
}

type instanceView struct {
	Name       string `json:"name"`
	Machine    string `json:"machine"`
	Addr       string `json:"addr"`
	ExpectedPF string `json:"expectedPf"`

	State     string    `json:"state"`
	Message   string    `json:"message,omitempty"`
	Connected bool      `json:"connected"`
	LastSeen  time.Time `json:"lastSeen,omitempty"`

	Ports     []portView `json:"ports,omitempty"`
	Lcores    []uint32   `json:"lcores,omitempty"`
	MainLcore uint32     `json:"mainLcore,omitempty"`
	DPVersion string     `json:"dataplaneVersion,omitempty"`
	EventDrop uint64     `json:"eventDrop,omitempty"`
}

type machineView struct {
	Name     string `json:"name"`
	Address  string `json:"address"`
	Expected int    `json:"expected"`
	Ready    int    `json:"ready"`
}

type capacityResponse struct {
	Machines []machineView `json:"machines"`
	Expected int           `json:"expected"`
	Ready    int           `json:"ready"`
}

// capacity 는 장비 단위 롤업이다. 인스턴스 단위 상세는 /api/dataplanes.
func (s *Server) capacity(w http.ResponseWriter, _ *http.Request) {
	byName := map[string]instanceView{}
	for _, v := range s.instanceViews() {
		byName[v.Name] = v
	}

	resp := capacityResponse{Machines: make([]machineView, 0, len(s.fleet.Machines))}

	for mi := range s.fleet.Machines {
		m := &s.fleet.Machines[mi]
		mv := machineView{
			Name:     m.Name,
			Address:  m.Address,
			Expected: len(m.Instances),
		}
		for _, in := range m.Instances {
			if byName[in.Name].State == stateOK {
				mv.Ready++
			}
		}
		resp.Expected += mv.Expected
		resp.Ready += mv.Ready
		resp.Machines = append(resp.Machines, mv)
	}

	writeJSON(w, http.StatusOK, resp)
}

func (s *Server) listDataplanes(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, s.instanceViews())
}

// instanceViews 는 기대(함대 설정)를 기준으로 순회하며 실측(registry)을 겹친다.
//
// 순회 기준이 기대인 것이 핵심이다. 실측을 기준으로 돌면 **없는 인스턴스가
// 목록에서 조용히 빠져** 버려서, 가장 알고 싶은 상태("떠 있어야 하는데 없다")를
// 표현할 수가 없다.
func (s *Server) instanceViews() []instanceView {
	observed := s.reg.ByName()

	out := make([]instanceView, 0, len(s.fleet.Machines)*4)

	for _, in := range s.fleet.Instances() {
		v := instanceView{
			Name:       in.Name,
			Machine:    in.Machine,
			Addr:       in.Addr,
			ExpectedPF: in.PF,
			State:      stateUnreachable,
		}

		st, ok := observed[in.Name]
		if !ok || !st.Connected {
			out = append(out, v)
			continue
		}

		v.Connected = true
		v.LastSeen = st.LastSeen

		if st.Hello == nil {
			v.State = stateConnecting
			out = append(out, v)
			continue
		}

		v.Lcores = st.Hello.Lcores
		v.MainLcore = st.Hello.MainLcore
		v.DPVersion = st.Hello.DataplaneVersion
		v.Ports = toPortViews(st.Hello.Ports)
		if st.Telemetry != nil {
			v.EventDrop = st.Telemetry.EventDrop
		}

		switch {
		case len(v.Ports) == 0:
			v.State = stateNoPorts
			v.Message = "NIC 을 하나도 잡지 못했다 — vfio 바인딩과 MIR_DEVICE_SPEC 를 확인할 것"

		case !hasPF(v.Ports, in.PF):
			v.State = statePFMismatch
			v.Message = "설정된 PF(" + in.PF + ") 가 아니라 " +
				strings.Join(deviceSpecs(v.Ports), ",") + " 를 잡았다"

		default:
			v.State = stateOK
		}

		out = append(out, v)
	}
	return out
}

// hasPF 는 실제로 잡은 장치 중 기대한 BDF 가 있는지 본다.
// 대소문자 차이는 무시한다 — lspci 출력과 sysfs 표기가 갈릴 수 있다.
func hasPF(ports []portView, want string) bool {
	for _, p := range ports {
		if strings.EqualFold(p.DeviceSpec, want) {
			return true
		}
	}
	return false
}

func deviceSpecs(ports []portView) []string {
	out := make([]string, 0, len(ports))
	for _, p := range ports {
		out = append(out, p.DeviceSpec)
	}
	return out
}

func toPortViews(ports []*pb.PortInfo) []portView {
	out := make([]portView, 0, len(ports))
	for _, p := range ports {
		out = append(out, portView{
			PortID:     p.PortId,
			Driver:     p.Driver,
			MAC:        p.Mac,
			NumaNode:   p.NumaNode,
			DeviceSpec: p.DeviceSpec,
		})
	}
	return out
}

// ───────────────────────────────────────────────────────────

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}
