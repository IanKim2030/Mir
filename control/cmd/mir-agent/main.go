// mir-agent — 데이터플레인 인스턴스의 사이드카 컨테이너.
//
// C 데이터플레인과 unix socket(③)으로 붙고, 제어부에는 gRPC(④)로 노출한다.
// 이 프로세스는 공유 풀 코어에서 돌며, 데이터플레인의 배타 코어를 절대
// 건드리지 않는다 — 별도 컨테이너이므로 cpuset 이 물리적으로 분리되어 있다.
package main

import (
	"context"
	"log/slog"
	"net"
	"os"
	"os/signal"
	"syscall"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/health"
	healthpb "google.golang.org/grpc/health/grpc_health_v1"
	"google.golang.org/grpc/reflection"

	"mir/internal/agent"
	"mir/internal/mtls"
	"mir/internal/pb"
)

func main() {
	log := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{
		Level: parseLevel(env("MIR_LOG_LEVEL", "info")),
	}))
	slog.SetDefault(log)

	var (
		sockPath = env("MIR_IPC_SOCKET", "/var/run/mir/dp.sock")
		listen   = env("MIR_GRPC_LISTEN", ":9100")
		nodeID   = env("HOSTNAME", "unknown")
	)

	// 이 포트는 장비 밖으로 열린다(제어부가 다른 장비에 있을 수 있다).
	// 인증이 없으면 여기 닿는 누구나 라인레이트 송신을 시킬 수 있으므로,
	// TLS 를 끄려면 MIR_INSECURE=1 을 명시해야 한다.
	tlsCfg := mtls.FromEnv()
	creds, err := tlsCfg.ServerCredentials()
	if err != nil {
		log.Error("TLS 설정 실패", "err", err)
		os.Exit(1)
	}

	log.Info("mir-agent 기동",
		"sock", sockPath, "listen", listen, "node", nodeID, "tls", tlsCfg.Enabled())
	if !tlsCfg.Enabled() {
		log.Warn("평문 제어 채널 — 신뢰할 수 있는 네트워크에서만 쓸 것 (MIR_INSECURE=1)")
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	healthSrv := health.NewServer()
	// 데이터플레인이 붙기 전까지는 NOT_SERVING 이다. 포트를 실제로 쥐었는지까지
	// 확인한 뒤에야 SERVING 으로 올라간다 — internal/agent 의 probeReady.
	healthSrv.SetServingStatus(agent.ServiceName, healthpb.HealthCheckResponse_NOT_SERVING)
	healthSrv.SetServingStatus("", healthpb.HealthCheckResponse_NOT_SERVING)

	a := agent.New(sockPath, log, healthSrv)
	go a.Run(ctx)

	srv := grpc.NewServer(grpc.Creds(creds))
	pb.RegisterDataPlaneServer(srv, a)
	healthpb.RegisterHealthServer(srv, healthSrv)

	// grpcurl 로 붙어 스키마를 조회할 수 있게 한다. 로컬 디버깅에 결정적으로
	// 유용하고, TLS 를 켜 두면 클라이언트 인증서 없이는 조회되지 않는다.
	reflection.Register(srv)

	lis, err := net.Listen("tcp", listen)
	if err != nil {
		log.Error("리슨 실패", "addr", listen, "err", err)
		os.Exit(1)
	}

	errCh := make(chan error, 1)
	go func() { errCh <- srv.Serve(lis) }()

	select {
	case <-ctx.Done():
		log.Info("종료 신호 수신")
	case err := <-errCh:
		if err != nil {
			log.Error("gRPC 서버 종료", "err", err)
			os.Exit(1)
		}
	}

	// GracefulStop 이 오래 걸리면 강제 종료한다. 데이터플레인이 이미
	// 사라진 상태에서 스트림이 남아 있을 수 있다.
	done := make(chan struct{})
	go func() { srv.GracefulStop(); close(done) }()

	select {
	case <-done:
	case <-time.After(5 * time.Second):
		log.Warn("graceful stop 지연 — 강제 종료")
		srv.Stop()
	}
	log.Info("mir-agent 종료")
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
