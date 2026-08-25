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
	"mir/internal/webui"
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

	// 모드 A — L2~L4 생성 송신 (Phase 2)
	mux.HandleFunc("POST /api/scenarios/start", s.startScenario)
	mux.HandleFunc("POST /api/scenarios/stop", s.stopScenario)

	// 수신 판정 (Phase 3)
	mux.HandleFunc("GET /api/events", s.listEvents)

	// 실시간 스트림 (Phase 6c) — SSE 로 스냅샷 푸시.
	mux.HandleFunc("GET /api/stream", s.stream)

	// GUI (Phase 6) — 내장 React SPA. "/" 는 catch-all 이라 위의 구체적
	// 패턴(/api·/healthz·/readyz)이 먼저 잡힌다(Go 1.22 mux 우선순위).
	mux.Handle("/", webui.Handler())

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

	// NIC 하드웨어 카운터(rte_eth_stats_get). 텔레메트리 스냅샷을 아직 못
	// 받았으면 nil 이다.
	//
	// 이 값이 중요한 이유: 데이터플레인이 "몇 개 보냈다"고 보고하는 것과
	// **NIC 이 실제로 몇 개 내보냈는지**는 다른 사실이다. tx_burst 가 mbuf 를
	// 받아 갔어도 선로에 나가지 않을 수 있다. 둘을 대조할 수 있어야 한다.
	Stats *portStats `json:"stats,omitempty"`
}

type portStats struct {
	TxPkts  uint64 `json:"txPkts"`
	TxBytes uint64 `json:"txBytes"`
	TxDrop  uint64 `json:"txDrop"`
	TxErr   uint64 `json:"txErr"`
	RxPkts  uint64 `json:"rxPkts"`
	RxBytes uint64 `json:"rxBytes"`
	RxDrop  uint64 `json:"rxDrop"`
	RxErr   uint64 `json:"rxErr"`
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

	// 진행 중인 시나리오 (텔레메트리에서). 비어 있으면 유휴.
	ActiveScenario string `json:"activeScenario,omitempty"`
	TxLcores       uint32 `json:"txLcores,omitempty"`
	TxDrop         uint64 `json:"txDrop,omitempty"`

	// 수신 분류 (Phase 3). handshake 응답을 집계한 것.
	Rx *rxClass `json:"rx,omitempty"`

	// handshake 제어 집계 (Phase 4, 모드 B). 유휴면 nil.
	Handshake *handshakeStats `json:"handshake,omitempty"`
}

type handshakeStats struct {
	Sessions  uint32 `json:"sessions"`
	Sent      uint32 `json:"sent"`
	SynAck    uint32 `json:"synAck"`
	Completed uint32 `json:"completed"`
	Refused   uint32 `json:"refused"`
	TimedOut  uint32 `json:"timedOut"`
	RttMinUs  uint32 `json:"rttMinUs"`
	RttAvgUs  uint32 `json:"rttAvgUs"`
	RttMaxUs  uint32 `json:"rttMaxUs"`

	// 데이터 경로 (Phase 5a). l7Request 를 줬을 때만 진행.
	Established uint32 `json:"established"`
	ReqSent     uint32 `json:"reqSent"`
	Responded   uint32 `json:"responded"`
	Closed      uint32 `json:"closed"`
	BytesRx     uint64 `json:"bytesRx"`
	Http2xx     uint32 `json:"http2xx"`

	// TLS (Phase 5b).
	TLSOk     uint32 `json:"tlsOk"`
	TLSFailed uint32 `json:"tlsFailed"`
}

type rxClass struct {
	TCPSyn    uint64 `json:"tcpSyn"`
	TCPSynAck uint64 `json:"tcpSynAck"`
	TCPRst    uint64 `json:"tcpRst"`
	TCPFin    uint64 `json:"tcpFin"`
	TCPAck    uint64 `json:"tcpAck"`
	TCPOther  uint64 `json:"tcpOther"`
	UDP       uint64 `json:"udp"`
	NonIP     uint64 `json:"nonIp"`
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
// capacityData 는 장비 단위 롤업을 계산한다. REST 핸들러와 SSE 스트림이 공유한다.
func (s *Server) capacityData(views []instanceView) capacityResponse {
	byName := map[string]instanceView{}
	for _, v := range views {
		byName[v.Name] = v
	}

	resp := capacityResponse{Machines: make([]machineView, 0, len(s.fleet.Machines))}
	for mi := range s.fleet.Machines {
		m := &s.fleet.Machines[mi]
		mv := machineView{Name: m.Name, Address: m.Address, Expected: len(m.Instances)}
		for _, in := range m.Instances {
			if byName[in.Name].State == stateOK {
				mv.Ready++
			}
		}
		resp.Expected += mv.Expected
		resp.Ready += mv.Ready
		resp.Machines = append(resp.Machines, mv)
	}
	return resp
}

func (s *Server) capacity(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, s.capacityData(s.instanceViews()))
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
		v.Ports = toPortViews(st.Hello.Ports, st.Telemetry)
		if st.Telemetry != nil {
			v.EventDrop = st.Telemetry.EventDrop
			v.ActiveScenario = st.Telemetry.ActiveScenario
			v.TxLcores = st.Telemetry.TxLcores
			v.TxDrop = st.Telemetry.TxDrop
			if rx := st.Telemetry.Rx; rx != nil {
				v.Rx = &rxClass{
					TCPSyn: rx.TcpSyn, TCPSynAck: rx.TcpSynAck,
					TCPRst: rx.TcpRst, TCPFin: rx.TcpFin, TCPAck: rx.TcpAck,
					TCPOther: rx.TcpOther, UDP: rx.Udp, NonIP: rx.NonIp,
				}
			}
			if hs := st.Telemetry.Handshake; hs != nil {
				v.Handshake = &handshakeStats{
					Sessions: hs.Sessions, Sent: hs.Sent, SynAck: hs.Synack,
					Completed: hs.Completed, Refused: hs.Refused, TimedOut: hs.TimedOut,
					RttMinUs: hs.RttMinUs, RttAvgUs: hs.RttAvgUs, RttMaxUs: hs.RttMaxUs,
					Established: hs.Established, ReqSent: hs.ReqSent, Responded: hs.Responded,
					Closed: hs.Closed, BytesRx: hs.BytesRx, Http2xx: hs.Http_2Xx,
					TLSOk: hs.TlsOk, TLSFailed: hs.TlsFailed,
				}
			}
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

// toPortViews 는 Hello 의 정적 정보(포트 구성)에 텔레메트리의 동적 카운터를
// portId 로 맞춰 붙인다. snap 이 nil 이면 카운터 없이 구성만 돌려준다.
func toPortViews(ports []*pb.PortInfo, snap *pb.TelemetrySnapshot) []portView {
	byID := map[uint32]*pb.PortStats{}
	if snap != nil {
		for _, s := range snap.Ports {
			byID[s.PortId] = s
		}
	}

	out := make([]portView, 0, len(ports))
	for _, p := range ports {
		v := portView{
			PortID:     p.PortId,
			Driver:     p.Driver,
			MAC:        p.Mac,
			NumaNode:   p.NumaNode,
			DeviceSpec: p.DeviceSpec,
		}
		if s, ok := byID[p.PortId]; ok {
			v.Stats = &portStats{
				TxPkts:  s.TxPkts,
				TxBytes: s.TxBytes,
				TxDrop:  s.TxDrop,
				TxErr:   s.TxErr,
				RxPkts:  s.RxPkts,
				RxBytes: s.RxBytes,
				RxDrop:  s.RxDrop,
				RxErr:   s.RxErr,
			}
		}
		out = append(out, v)
	}
	return out
}

// ───────────────────────────────────────────────────────────

func writeError(w http.ResponseWriter, code int, msg string) {
	writeJSON(w, code, map[string]string{"error": msg})
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}
