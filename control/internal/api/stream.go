package api

import (
	"encoding/json"
	"net/http"
	"time"
)

// snapshot 은 SSE 로 브라우저에 밀어 넣는 한 프레임이다. 폴링으로 세 번 부르던
// (/api/dataplanes · /api/capacity · /api/events) 것을 한 번에 담는다.
type snapshot struct {
	Instances []instanceView   `json:"instances"`
	Capacity  capacityResponse `json:"capacity"`
	Events    eventsResponse   `json:"events"`
}

// streamInterval 은 스냅샷 푸시 주기다. 텔레메트리 자체가 데이터플레인에서
// 100ms 로 올라오므로, 그보다 촘촘히 밀 이유가 없다. 500ms 면 사람이 보기에
// 실시간이고 대역·CPU 부담이 없다.
const streamInterval = 500 * time.Millisecond

// stream 은 Server-Sent Events 로 스냅샷을 밀어 준다.
//
// **왜 WebSocket 이 아니라 SSE 인가.** 텔레메트리는 서버→브라우저 단방향이고,
// 명령(시나리오 시작/정지)은 이미 REST(POST)로 간다. 양방향이 필요 없으므로
// SSE 면 충분한데, SSE 는 **의존성이 0**이다(순수 net/http). 이 프로젝트가
// 일관되게 지켜 온 최소 의존성 원칙(Go 직접 의존 3개, dataplane C++ 무반입)과
// 맞고, WebSocket 라이브러리를 하나 더 들이지 않는다. 브라우저는 표준
// EventSource 로 받고 자동 재연결까지 공짜로 얻는다.
func (s *Server) stream(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")
	w.Header().Set("X-Accel-Buffering", "no") // 리버스 프록시 버퍼링 방지

	rc := http.NewResponseController(w)
	enc := json.NewEncoder(w)

	send := func() bool {
		views := s.instanceViews()
		snap := snapshot{
			Instances: views,
			Capacity:  s.capacityData(views),
			Events:    s.eventsData(),
		}
		if _, err := w.Write([]byte("data: ")); err != nil {
			return false
		}
		if err := enc.Encode(snap); err != nil { // Encode 가 개행을 붙여 준다
			return false
		}
		if _, err := w.Write([]byte("\n")); err != nil { // SSE 이벤트 종료(빈 줄)
			return false
		}
		return rc.Flush() == nil
	}

	// 붙자마자 첫 프레임을 보내 화면이 즉시 채워지게 한다.
	if !send() {
		return
	}

	ticker := time.NewTicker(streamInterval)
	defer ticker.Stop()
	ctx := r.Context()
	for {
		select {
		case <-ctx.Done(): // 클라이언트가 끊었다
			return
		case <-ticker.C:
			if !send() {
				return
			}
		}
	}
}
