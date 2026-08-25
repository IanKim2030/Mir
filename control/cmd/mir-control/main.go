// mir-control — 제어부.
//
//   - 함대 설정에 적힌 인스턴스들에 gRPC 로 붙는다 (장비 여러 대에 걸쳐도 된다)
//   - 기대(설정)와 실측(HelloResponse)을 대조해 상태를 보고한다
//   - GUI 용 REST 를 노출한다 (화면은 Phase 6)
//
// 데이터플레인과 **별도 프로세스**여야 하는 이유는 그대로다 — Go 와 C 를 한
// 프로세스로 합치면 cgo 로 DPDK 를 부르게 되고, gRPC 스레드 풀이 busy-poll
// 코어를 침범할 경로가 생긴다. 오케스트레이터를 걷어낸 것은 배포 단위의 변화이지
// 이 경계의 변화가 아니다.
package main

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"mir/internal/api"
	"mir/internal/fleet"
	"mir/internal/mtls"
	"mir/internal/registry"
)

var version = "0.1.0"

func main() {
	// 이미지가 distroless 라 셸도 curl 도 없다. 컨테이너 healthcheck 는
	// 바이너리 자신이 자기 /healthz 를 두드리는 방식이어야 한다.
	if len(os.Args) > 1 && os.Args[1] == "-healthcheck" {
		os.Exit(healthcheck())
	}

	log := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{
		Level: parseLevel(env("MIR_LOG_LEVEL", "info")),
	}))
	slog.SetDefault(log)

	var (
		fleetPath = env("MIR_FLEET_CONFIG", "/etc/mir/fleet.yaml")
		listen    = env("MIR_HTTP_LISTEN", ":8080")
	)

	fc, err := fleet.Load(fleetPath)
	if err != nil {
		// 설정 없이는 붙을 대상을 모른다. 빈 목록으로 조용히 뜨면 "다 죽었다"와
		// "설정이 없다"가 구분되지 않으므로, 여기서 멈추는 편이 낫다.
		log.Error("함대 설정 로드 실패", "path", fleetPath, "err", err)
		os.Exit(1)
	}

	// 함대가 장비 경계를 넘으면 제어 채널이 실제 망을 탄다. 평문은 명시적으로
	// 켜야만 쓰인다 — internal/mtls 패키지 주석 참조.
	tlsCfg := mtls.FromEnv()
	creds, err := tlsCfg.ClientCredentials()
	if err != nil {
		log.Error("TLS 설정 실패", "err", err)
		os.Exit(1)
	}
	if !tlsCfg.Enabled() {
		log.Warn("평문 제어 채널 — 신뢰할 수 있는 네트워크에서만 쓸 것 (MIR_INSECURE=1)")
	}

	instances := fc.Instances()
	log.Info("mir-control 기동",
		"version", version,
		"fleet", fleetPath,
		"machines", len(fc.Machines),
		"instances", len(instances),
		"tls", tlsCfg.Enabled())
	for _, in := range instances {
		log.Info("기대 인스턴스", "name", in.Name, "addr", in.Addr, "pf", in.PF)
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	reg := registry.New(fc, creds, version, log)
	go reg.Run(ctx)

	srv := &http.Server{
		Addr:              listen,
		Handler:           api.New(fc, reg, version, log).Routes(),
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

// healthcheck 는 자기 자신의 /healthz 를 두드려 종료 코드로 답한다.
//
// **liveness 만 본다.** /readyz 를 쓰면 데이터플레인이 아직 안 뜬 동안 제어부
// 컨테이너가 unhealthy 로 표시되는데, 그건 제어부의 문제가 아니고 그 상태야말로
// 제어부에 물어봐야 알 수 있는 것이다.
func healthcheck() int {
	listen := env("MIR_HTTP_LISTEN", ":8080")

	_, port, err := net.SplitHostPort(listen)
	if err != nil {
		fmt.Fprintf(os.Stderr, "MIR_HTTP_LISTEN 파싱 실패 (%s): %v\n", listen, err)
		return 1
	}

	client := &http.Client{Timeout: 2 * time.Second}
	url := "http://127.0.0.1:" + port + "/healthz"

	resp, err := client.Get(url)
	if err != nil {
		fmt.Fprintf(os.Stderr, "healthz 요청 실패: %v\n", err)
		return 1
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		fmt.Fprintf(os.Stderr, "healthz 상태 코드 %d\n", resp.StatusCode)
		return 1
	}
	return 0
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
