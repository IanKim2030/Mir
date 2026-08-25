package api

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"time"

	"google.golang.org/protobuf/encoding/protojson"

	"mir/internal/pb"
	"mir/internal/registry"
)

// 시나리오 명령의 왕복 상한. 데이터플레인의 start 는 프레임을 굽고 worker 를
// launch 하는 동기 작업이라 즉시 끝나지만, 붙지 않은 인스턴스를 기다리며
// 무한정 매달리지 않도록 잘라 둔다.
const scenarioTimeout = 10 * time.Second

// startRequest 는 GUI/CLI 가 보내는 형태다.
//
// packet 만 protojson 으로 받는 이유: oneof(l3/l4)를 손으로 매핑하면 proto 가
// 바뀔 때마다 두 곳을 고쳐야 하고, 그 어긋남이 조용한 버그가 된다. 바깥
// 껍데기는 필드가 몇 개뿐이라 평범한 구조체가 낫다(오류 메시지가 훨씬 낫다).
type startRequest struct {
	ScenarioID string   `json:"scenarioId"`
	Targets    []string `json:"targets"` // 비면 붙어 있는 전체
	RatePps    uint64   `json:"ratePps"`
	DurationS  uint32   `json:"durationS"`
	TxLcores   uint32   `json:"txLcores"`
	StartAtNs  uint64   `json:"startAtNs"`

	// packet(모드 A) 과 handshake(모드 B)는 배타. 정확히 하나여야 한다.
	Packet    json.RawMessage `json:"packet"`
	Handshake json.RawMessage `json:"handshake"`
}

type stopRequest struct {
	ScenarioID string   `json:"scenarioId"`
	Targets    []string `json:"targets"`
}

// 인스턴스별 결과. 하나가 실패해도 나머지 결과를 보여야 하므로
// 전체를 200 으로 돌려주고 각 항목에 성공 여부를 담는다.
type instanceResult struct {
	Name    string `json:"name"`
	OK      bool   `json:"ok"`
	Message string `json:"message,omitempty"`
	Error   string `json:"error,omitempty"`
}

type scenarioResponse struct {
	ScenarioID string           `json:"scenarioId"`
	OK         bool             `json:"ok"` // 전부 성공했는가
	Results    []instanceResult `json:"results"`
}

func decodeJSON(r *http.Request, v any) error {
	dec := json.NewDecoder(http.MaxBytesReader(nil, r.Body, 1<<20))
	dec.DisallowUnknownFields()
	return dec.Decode(v)
}

// resolveTargets 는 요청의 대상 목록을 확정한다.
// 비어 있으면 붙어 있는 전체다 — "전부에 걸어라"가 가장 흔한 사용이다.
func (s *Server) resolveTargets(req []string) ([]string, error) {
	if len(req) > 0 {
		return req, nil
	}
	names := s.reg.Names()
	if len(names) == 0 {
		return nil, errors.New("붙어 있는 인스턴스가 없다")
	}
	return names, nil
}

func (s *Server) startScenario(w http.ResponseWriter, r *http.Request) {
	var req startRequest
	if err := decodeJSON(r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if req.ScenarioID == "" {
		writeError(w, http.StatusBadRequest, "scenarioId 가 필요하다")
		return
	}
	hasPacket := len(req.Packet) > 0
	hasHandshake := len(req.Handshake) > 0
	if hasPacket == hasHandshake {
		writeError(w, http.StatusBadRequest,
			"packet(모드 A) 또는 handshake(모드 B) 중 정확히 하나가 필요하다")
		return
	}

	cmd := &pb.StartScenarioRequest{
		ScenarioId: req.ScenarioID,
		RatePps:    req.RatePps,
		DurationS:  req.DurationS,
		TxLcores:   req.TxLcores,
		StartAtNs:  req.StartAtNs,
	}

	// DiscardUnknown 을 켜지 않는다 — 오타 난 필드가 조용히 무시되면
	// "왜 내가 설정한 대로 안 되지"를 추적할 수 없다.
	if hasPacket {
		spec := &pb.PacketSpec{}
		if err := protojson.Unmarshal(req.Packet, spec); err != nil {
			writeError(w, http.StatusBadRequest, "packet 명세 해석 실패: "+err.Error())
			return
		}
		cmd.Packet = spec
	} else {
		spec := &pb.HandshakeSpec{}
		if err := protojson.Unmarshal(req.Handshake, spec); err != nil {
			writeError(w, http.StatusBadRequest, "handshake 명세 해석 실패: "+err.Error())
			return
		}
		cmd.Handshake = spec
	}

	targets, err := s.resolveTargets(req.Targets)
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}

	s.log.Info("시나리오 시작 요청",
		"id", req.ScenarioID, "targets", targets, "mode",
		map[bool]string{true: "A/packet", false: "B/handshake"}[hasPacket])

	resp := scenarioResponse{ScenarioID: req.ScenarioID, OK: true}
	for _, name := range targets {
		ctx, cancel := context.WithTimeout(r.Context(), scenarioTimeout)
		ack, err := s.reg.StartScenario(ctx, name, cmd)
		cancel()
		resp.Results = append(resp.Results, toResult(name, ack, err))
	}
	for _, x := range resp.Results {
		if !x.OK {
			resp.OK = false
		}
	}

	writeJSON(w, http.StatusOK, resp)
}

func (s *Server) stopScenario(w http.ResponseWriter, r *http.Request) {
	var req stopRequest
	if err := decodeJSON(r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}

	targets, err := s.resolveTargets(req.Targets)
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}

	cmd := &pb.StopScenarioRequest{ScenarioId: req.ScenarioID}
	s.log.Info("시나리오 정지 요청", "id", req.ScenarioID, "targets", targets)

	resp := scenarioResponse{ScenarioID: req.ScenarioID, OK: true}
	for _, name := range targets {
		ctx, cancel := context.WithTimeout(r.Context(), scenarioTimeout)
		ack, err := s.reg.StopScenario(ctx, name, cmd)
		cancel()
		resp.Results = append(resp.Results, toResult(name, ack, err))
	}
	for _, x := range resp.Results {
		if !x.OK {
			resp.OK = false
		}
	}

	writeJSON(w, http.StatusOK, resp)
}

func toResult(name string, ack *pb.Ack, err error) instanceResult {
	if err != nil {
		var nf *registry.ErrNotFound
		if errors.As(err, &nf) {
			return instanceResult{Name: name, Error: nf.Error()}
		}
		return instanceResult{Name: name, Error: err.Error()}
	}
	return instanceResult{Name: name, OK: ack.Ok, Message: ack.Message}
}
