// Package mtls — 제어 채널(④ gRPC)의 상호 인증.
//
// 한 장비 안에서만 돌 때는 평문이어도 넘어갔다. 장비 경계를 넘는 순간 얘기가
// 달라진다 — **:9100 에 닿을 수 있는 누구나 라인레이트 패킷 제너레이터를
// 조종할 수 있다.** 관리 전용 VLAN 을 전제로 하더라도 그것 하나에 기대지 않는다.
//
// 그래서 기본값이 안전한 쪽이다. 평문은 MIR_INSECURE=1 을 **명시적으로** 줘야
// 켜지고, 켜지면 양쪽 모두 경고를 남긴다.
package mtls

import (
	"crypto/tls"
	"crypto/x509"
	"fmt"
	"os"

	"google.golang.org/grpc/credentials"
	"google.golang.org/grpc/credentials/insecure"
)

type Config struct {
	CertFile string
	KeyFile  string
	CAFile   string

	// Insecure 가 참이면 TLS 를 아예 쓰지 않는다. 개발용 통로다.
	Insecure bool
}

// FromEnv 는 환경변수에서 설정을 읽는다.
//
//	MIR_TLS_CERT / MIR_TLS_KEY / MIR_TLS_CA
//	MIR_INSECURE=1
func FromEnv() Config {
	return Config{
		CertFile: os.Getenv("MIR_TLS_CERT"),
		KeyFile:  os.Getenv("MIR_TLS_KEY"),
		CAFile:   os.Getenv("MIR_TLS_CA"),
		Insecure: os.Getenv("MIR_INSECURE") == "1",
	}
}

// Enabled 는 TLS 를 실제로 쓸지 알려 준다 (로그·진단용).
func (c Config) Enabled() bool { return !c.Insecure }

// Validate 는 설정이 앞뒤가 맞는지 본다.
//
// 인증서 경로를 줬는데 MIR_INSECURE 도 켜 둔 경우를 조용히 넘기지 않는다 —
// 보안을 켰다고 믿는 채로 평문이 나가는 게 가장 나쁜 결과다.
func (c Config) Validate() error {
	if c.Insecure {
		if c.CertFile != "" || c.KeyFile != "" || c.CAFile != "" {
			return fmt.Errorf("MIR_INSECURE=1 과 MIR_TLS_* 가 함께 설정됐다 — 둘 중 하나만 쓸 것")
		}
		return nil
	}
	if c.CertFile == "" || c.KeyFile == "" || c.CAFile == "" {
		return fmt.Errorf("MIR_TLS_CERT / MIR_TLS_KEY / MIR_TLS_CA 가 모두 필요하다 " +
			"(개발 중이라면 MIR_INSECURE=1 을 명시할 것)")
	}
	return nil
}

func (c Config) load() (tls.Certificate, *x509.CertPool, error) {
	cert, err := tls.LoadX509KeyPair(c.CertFile, c.KeyFile)
	if err != nil {
		return tls.Certificate{}, nil, fmt.Errorf("인증서/키 로드 실패: %w", err)
	}

	pem, err := os.ReadFile(c.CAFile)
	if err != nil {
		return tls.Certificate{}, nil, fmt.Errorf("CA 읽기 실패: %w", err)
	}

	pool := x509.NewCertPool()
	if !pool.AppendCertsFromPEM(pem) {
		return tls.Certificate{}, nil, fmt.Errorf("CA 파싱 실패 (%s): PEM 인증서가 없다", c.CAFile)
	}
	return cert, pool, nil
}

// ServerCredentials — 사이드카(gRPC 서버)용. 클라이언트 인증서를 요구한다.
func (c Config) ServerCredentials() (credentials.TransportCredentials, error) {
	if err := c.Validate(); err != nil {
		return nil, err
	}
	if c.Insecure {
		return insecure.NewCredentials(), nil
	}

	cert, pool, err := c.load()
	if err != nil {
		return nil, err
	}

	return credentials.NewTLS(&tls.Config{
		Certificates: []tls.Certificate{cert},
		// 제어부만 붙을 수 있어야 한다. VerifyClientCertIfGiven 은
		// 인증서를 아예 안 내면 통과시키므로 쓰면 안 된다.
		ClientAuth: tls.RequireAndVerifyClientCert,
		ClientCAs:  pool,
		MinVersion: tls.VersionTLS13,
	}), nil
}

// ClientCredentials — 제어부(gRPC 클라이언트)용. 자기 인증서를 제시하고
// 서버도 같은 CA 로 검증한다. 서버 이름은 다이얼 주소에서 나오므로,
// 사이드카 인증서에 그 장비의 IP/호스트명이 SAN 으로 들어 있어야 한다.
func (c Config) ClientCredentials() (credentials.TransportCredentials, error) {
	if err := c.Validate(); err != nil {
		return nil, err
	}
	if c.Insecure {
		return insecure.NewCredentials(), nil
	}

	cert, pool, err := c.load()
	if err != nil {
		return nil, err
	}

	return credentials.NewTLS(&tls.Config{
		Certificates: []tls.Certificate{cert},
		RootCAs:      pool,
		MinVersion:   tls.VersionTLS13,
	}), nil
}
