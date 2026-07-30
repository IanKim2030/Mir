// mir-control — 제어부.
//
//   - headless Service 의 EndpointSlice 를 조회해 N개 사이드카에 gRPC 로 붙는다
//   - k8s API 로 데이터플레인 개수·리소스를 조정한다
//   - GUI 용 REST 를 노출한다 (화면은 Phase 6)
//
// 데이터플레인과 **별도 파드**여야 하는 이유: 개수·리소스 변경은 반드시
// 데이터플레인 재시작을 수반하는데, 같은 파드였다면 제어부가 자기 자신을
// 죽이는 명령을 내리게 된다. 자세한 근거는 internal/k8s 패키지 주석 참조.
package main

import (
	"context"
	"errors"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"mir/internal/api"
	"mir/internal/k8s"
	"mir/internal/registry"
)

var version = "0.1.0"

func main() {
	log := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{
		Level: parseLevel(env("MIR_LOG_LEVEL", "info")),
	}))
	slog.SetDefault(log)

	var (
		namespace  = env("MIR_NAMESPACE", "mir")
		deployment = env("MIR_DATAPLANE_DEPLOYMENT", "mir-dataplane")
		service    = env("MIR_DATAPLANE_SERVICE", "mir-dataplane")
		labelSel   = env("MIR_DATAPLANE_SELECTOR", "app=mir-dataplane")
		pfResource = env("MIR_PF_RESOURCE", "mir.io/dpdk_pf")
		grpcPort   = env("MIR_DATAPLANE_GRPC_PORT", "9100")
		listen     = env("MIR_HTTP_LISTEN", ":8080")
	)

	log.Info("mir-control 기동",
		"version", version, "namespace", namespace,
		"deployment", deployment, "service", service)

	kc, err := k8s.New(k8s.Config{
		Namespace:  namespace,
		Deployment: deployment,
		PFResource: pfResource,
		LabelSel:   labelSel,
		Log:        log,
	})
	if err != nil {
		log.Error("k8s 클라이언트 생성 실패", "err", err)
		os.Exit(1)
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	reg := registry.New(kc.Clientset(), namespace, service, grpcPort, version, log)
	go reg.Run(ctx)

	srv := &http.Server{
		Addr:              listen,
		Handler:           api.New(kc, reg, version, log).Routes(),
		ReadHeaderTimeout: 10 * time.Second,
	}

	errCh := make(chan error, 1)
	go func() {
		log.Info("HTTP 리슨", "addr", listen)
		errCh <- srv.ListenAndServe()
	}()

	select {
	case <-ctx.Done():
		log.Info("종료 신호 수신")
	case err := <-errCh:
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Error("HTTP 서버 종료", "err", err)
			os.Exit(1)
		}
	}

	shutCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := srv.Shutdown(shutCtx); err != nil {
		log.Warn("graceful shutdown 실패", "err", err)
	}
	log.Info("mir-control 종료")
}

func env(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func parseLevel(s string) slog.Level {
	var l slog.Level
	if err := l.UnmarshalText([]byte(s)); err != nil {
		return slog.LevelInfo
	}
	return l
}
