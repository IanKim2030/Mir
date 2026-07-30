// Package api — GUI 가 호출하는 REST 계층.
//
// 이번 단계에서는 핸들러와 k8s 연동까지만 만들고 화면은 붙이지 않는다 (Phase 6).
package api

import (
	"encoding/json"
	"errors"
	"log/slog"
	"net/http"
	"time"

	"mir/internal/k8s"
	"mir/internal/pb"
	"mir/internal/registry"
)

type Server struct {
	k8s     *k8s.Client
	reg     *registry.Registry
	version string
	log     *slog.Logger
}

func New(kc *k8s.Client, reg *registry.Registry, version string, log *slog.Logger) *Server {
	return &Server{k8s: kc, reg: reg, version: version, log: log}
}

func (s *Server) Routes() http.Handler {
	mux := http.NewServeMux()

	mux.HandleFunc("GET /healthz", s.healthz)
	mux.HandleFunc("GET /api/capacity", s.capacity)
	mux.HandleFunc("GET /api/dataplanes", s.listDataplanes)
	mux.HandleFunc("PUT /api/dataplanes/scale", s.scale)
	mux.HandleFunc("PUT /api/dataplanes/resources", s.setResources)
	mux.HandleFunc("POST /api/dataplanes/{name}/restart", s.restart)

	return mux
}

// ───────────────────────────────────────────────────────────

type healthResponse struct {
	OK        bool   `json:"ok"`
	Version   string `json:"version"`
	Connected int    `json:"connected"`
	Total     int    `json:"total"`
}

func (s *Server) healthz(w http.ResponseWriter, r *http.Request) {
	statuses := s.reg.List()
	connected := 0
	for _, st := range statuses {
		if st.Connected {
			connected++
		}
	}
	writeJSON(w, http.StatusOK, healthResponse{
		OK:        true,
		Version:   s.version,
		Connected: connected,
		Total:     len(statuses),
	})
}

func (s *Server) capacity(w http.ResponseWriter, r *http.Request) {
	c, err := s.k8s.Capacity(r.Context())
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, c)
}

// ───────────────────────────────────────────────────────────

type dataplaneView struct {
	k8s.DataplanePod

	Connected bool      `json:"connected"`
	LastSeen  time.Time `json:"lastSeen,omitempty"`

	Ports     []portView `json:"ports,omitempty"`
	Lcores    []uint32   `json:"lcores,omitempty"`
	MainLcore uint32     `json:"mainLcore,omitempty"`
	DPVersion string     `json:"dataplaneVersion,omitempty"`
	EventDrop uint64     `json:"eventDrop,omitempty"`
}

type portView struct {
	PortID     uint32 `json:"portId"`
	Driver     string `json:"driver"`
	MAC        string `json:"mac"`
	NumaNode   int32  `json:"numaNode"`
	DeviceSpec string `json:"deviceSpec"`
}

// listDataplanes 는 k8s 가 아는 것(파드 상태)과 제어부가 아는 것(연결·텔레메트리)을
// 합쳐서 돌려준다. 둘 중 하나만 보면 원인 파악이 어렵다 — 예를 들어 Running 인데
// connected=false 면 사이드카나 EAL 초기화 쪽 문제로 좁혀진다.
func (s *Server) listDataplanes(w http.ResponseWriter, r *http.Request) {
	pods, err := s.k8s.ListDataplanes(r.Context())
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}

	byPod := s.reg.ByPod()
	out := make([]dataplaneView, 0, len(pods))

	for _, p := range pods {
		v := dataplaneView{DataplanePod: p}

		if st, ok := byPod[p.Name]; ok {
			v.Connected = st.Connected
			v.LastSeen = st.LastSeen

			if st.Hello != nil {
				v.Lcores = st.Hello.Lcores
				v.MainLcore = st.Hello.MainLcore
				v.DPVersion = st.Hello.DataplaneVersion
				v.Ports = toPortViews(st.Hello.Ports)
			}
			if st.Telemetry != nil {
				v.EventDrop = st.Telemetry.EventDrop
			}
		}
		out = append(out, v)
	}
	writeJSON(w, http.StatusOK, out)
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

type scaleRequest struct {
	Replicas *int32 `json:"replicas"`
}

func (s *Server) scale(w http.ResponseWriter, r *http.Request) {
	var req scaleRequest
	if err := decode(r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if req.Replicas == nil {
		writeError(w, http.StatusBadRequest, "replicas 가 필요하다")
		return
	}

	if err := s.k8s.Scale(r.Context(), *req.Replicas); err != nil {
		// 상한 초과는 400 으로 돌려준다. 통과시키면 초과분이 조용히 Pending 에
		// 쌓이고, 사용자는 파드 이벤트를 뒤져야 원인을 알게 된다.
		var over *k8s.ErrOverCapacity
		if errors.As(err, &over) {
			writeError(w, http.StatusBadRequest, err.Error())
			return
		}
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}

	writeJSON(w, http.StatusOK, map[string]any{
		"ok":       true,
		"replicas": *req.Replicas,
	})
}

func (s *Server) setResources(w http.ResponseWriter, r *http.Request) {
	var spec k8s.ResourceSpec
	if err := decode(r, &spec); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}

	if err := s.k8s.SetResources(r.Context(), spec); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}

	// 재시작을 수반한다는 사실을 응답에 명시한다. GUI 가 이 값을 보고
	// 사용자에게 확인을 받아야 한다 — 진행 중인 시나리오가 중단된다.
	writeJSON(w, http.StatusOK, map[string]any{
		"ok":              true,
		"restartRequired": true,
		"warning": "데이터플레인 파드가 롤링 재생성된다. 진행 중인 시나리오는 중단된다. " +
			"제어부와 GUI 는 영향받지 않으며, 새 파드가 뜨면 자동으로 재연결된다.",
	})
}

func (s *Server) restart(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	if name == "" {
		writeError(w, http.StatusBadRequest, "파드 이름이 필요하다")
		return
	}
	if err := s.k8s.RestartPod(r.Context(), name); err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusAccepted, map[string]any{"ok": true, "pod": name})
}

// ───────────────────────────────────────────────────────────

func decode(r *http.Request, v any) error {
	dec := json.NewDecoder(http.MaxBytesReader(nil, r.Body, 1<<20))
	dec.DisallowUnknownFields()
	return dec.Decode(v)
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

func writeError(w http.ResponseWriter, code int, msg string) {
	writeJSON(w, code, map[string]string{"error": msg})
}
