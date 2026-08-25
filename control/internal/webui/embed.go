// Package webui 는 빌드된 React SPA 를 바이너리에 내장해 서빙한다.
//
// **왜 내장인가.** 제어부는 단일 Go 바이너리로 배포한다(REQUIREMENTS 6-1).
// 헤드리스 100G 서버에 정적 파일 디렉터리를 따로 두지 않고, 실행파일 하나가
// UI 까지 서빙해 브라우저로 접속하게 한다.
//
// dist/ 는 `web/` 의 Vite 빌드 산출물이다(`npm run build`). 산출물을 저장소에
// 커밋해 두어 `go build` 와 Docker 빌드가 node 없이도 된다 — 프론트 툴체인이
// 백엔드 빌드의 전제가 되지 않게 한다.
package webui

import (
	"embed"
	"io/fs"
	"net/http"
	"strings"
)

//go:embed all:dist
var distFS embed.FS

// Handler 는 내장 SPA 를 서빙한다. 존재하는 파일은 그대로 주고, 그 외 경로는
// index.html 로 폴백한다 — 클라이언트 라우팅(SPA)에서 새로고침·딥링크가
// 404 가 되지 않게 한다. /api·/healthz 등은 mux 가 더 구체적 패턴으로 먼저
// 잡으므로 여기 오지 않는다.
func Handler() http.Handler {
	sub, err := fs.Sub(distFS, "dist")
	if err != nil {
		panic(err) // 빌드 타임 임베드라 여기서 실패하면 개발 중 즉시 드러난다
	}
	files := http.FileServerFS(sub)

	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		p := strings.TrimPrefix(r.URL.Path, "/")
		if p == "" {
			p = "index.html"
		}
		if _, err := fs.Stat(sub, p); err != nil {
			// 없는 경로 → SPA 진입점으로.
			http.ServeFileFS(w, r, sub, "index.html")
			return
		}
		files.ServeHTTP(w, r)
	})
}
