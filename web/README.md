# web — Mir GUI (Phase 6)

React + TypeScript SPA. **제어부(`mir-control`) 바이너리에 `go:embed` 로 내장**되어
`/` 로 서빙된다 — 별도 웹서버 없이 실행파일 하나가 UI 까지 준다.

## 개발

```bash
cd web
npm install
npm run dev          # Vite 개발 서버(:5173). /api 는 localhost:8080 으로 프록시
```

개발 중에는 `mir-control` 을 `:8080` 에 띄워 두면 프론트가 그 REST 를 호출한다.

## 빌드 (내장용)

```bash
npm run build        # tsc 타입체크 + vite build
```

산출물은 **`control/internal/webui/dist/`** 에 생성된다(Vite `outDir`). 이 경로는
Go 모듈 안이라 `//go:embed all:dist` 대상이 된다.

## ★ dist 는 저장소에 커밋한다

빌드 산출물을 커밋해 두는 이유: `go build` 와 Docker(control 이미지) 빌드가
**node 없이도** 되게 하기 위해서다. 프론트 툴체인이 백엔드 빌드의 전제가 되면
안 된다. 그래서 UI 를 바꾸면 **`npm run build` 후 dist 변경도 함께 커밋**한다.

## 구성

| 파일 | 역할 |
|---|---|
| `src/api.ts` | REST 타입·페처 (control/internal/api 의 JSON 과 일치) |
| `src/App.tsx` | 폴링(1s)·pps 델타·3영역 레이아웃·시나리오 제어 |
| `src/TxChart.tsx` | uPlot 롤링 처리량 차트 |

Phase 6 로드맵: **6a 라이브 대시보드(현재)** → 6b React Flow 시나리오 빌더
→ 6c WebSocket 스트리밍 + uPlot 심화.
