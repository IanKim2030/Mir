package api

import (
	"net/http"
	"time"
)

// eventView 는 판정 이벤트의 외부 표현이다.
type eventView struct {
	Instance string    `json:"instance"`
	TsNs     uint64    `json:"tsNs"`
	Time     time.Time `json:"time"`
	Kind     string    `json:"kind"`
	PortID   uint32    `json:"portId"`
	FlowKey  string    `json:"flowKey,omitempty"`
	RttUs    uint32    `json:"rttUs,omitempty"`
	Detail   string    `json:"detail,omitempty"`
}

type eventsResponse struct {
	// Seen 은 데이터플레인에서 올라온 총 이벤트 수(버퍼를 넘어 흘러간 것 포함).
	// len(Events) 보다 크면 오래된 이벤트가 버퍼에서 밀려났다는 뜻이다.
	Seen   uint64      `json:"seen"`
	Events []eventView `json:"events"`
}

// listEvents 는 최근 판정 이벤트를 시간순으로 돌려준다.
//
// 이벤트는 데이터플레인에서 이미 레이트 제한돼 올라온 **이상동작 표본**이다.
// 정확한 수는 /api/dataplanes 의 rx 카운터에 있다 — 둘의 역할이 다르다.
func (s *Server) listEvents(w http.ResponseWriter, _ *http.Request) {
	events, seen := s.reg.Events()

	out := eventsResponse{Seen: seen, Events: make([]eventView, 0, len(events))}
	for _, re := range events {
		ev := re.Event
		out.Events = append(out.Events, eventView{
			Instance: re.Instance,
			TsNs:     ev.TsNs,
			Time:     time.Unix(0, int64(ev.TsNs)),
			Kind:     ev.Kind.String(),
			PortID:   ev.PortId,
			FlowKey:  ev.FlowKey,
			RttUs:    ev.RttUs,
			Detail:   ev.Detail,
		})
	}
	writeJSON(w, http.StatusOK, out)
}
