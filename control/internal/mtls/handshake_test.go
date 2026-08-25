package mtls

import (
	"context"
	"crypto/rand"
	"crypto/rsa"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"math/big"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/health"
	healthpb "google.golang.org/grpc/health/grpc_health_v1"
)

// certAuthority 는 테스트용 자체 CA 다. deploy/host/50-gen-certs.sh 가 하는 일을
// 프로세스 안에서 재현해, 발급 → 핸드셰이크 → 검증까지 한 번에 확인한다.
type certAuthority struct {
	cert *x509.Certificate
	key  *rsa.PrivateKey
	pem  []byte
}

func newCA(t *testing.T) *certAuthority {
	t.Helper()

	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatalf("CA 키 생성 실패: %v", err)
	}

	tmpl := &x509.Certificate{
		SerialNumber:          big.NewInt(1),
		Subject:               pkix.Name{CommonName: "mir-test-ca"},
		NotBefore:             time.Now().Add(-time.Hour),
		NotAfter:              time.Now().Add(time.Hour),
		IsCA:                  true,
		KeyUsage:              x509.KeyUsageCertSign | x509.KeyUsageDigitalSignature,
		BasicConstraintsValid: true,
	}

	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		t.Fatalf("CA 인증서 생성 실패: %v", err)
	}
	cert, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatalf("CA 인증서 파싱 실패: %v", err)
	}

	return &certAuthority{
		cert: cert,
		key:  key,
		pem:  pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der}),
	}
}

// issue 는 CN/IP SAN 을 가진 리프 인증서를 발급하고 파일 경로를 담은 Config 를 준다.
func (ca *certAuthority) issue(t *testing.T, dir, name string, ip net.IP) Config {
	t.Helper()

	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatalf("키 생성 실패: %v", err)
	}

	tmpl := &x509.Certificate{
		SerialNumber: big.NewInt(time.Now().UnixNano()),
		Subject:      pkix.Name{CommonName: name},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(time.Hour),
		KeyUsage:     x509.KeyUsageDigitalSignature | x509.KeyUsageKeyEncipherment,
		ExtKeyUsage: []x509.ExtKeyUsage{
			x509.ExtKeyUsageServerAuth, x509.ExtKeyUsageClientAuth,
		},
	}
	if ip != nil {
		tmpl.IPAddresses = []net.IP{ip}
	}

	der, err := x509.CreateCertificate(rand.Reader, tmpl, ca.cert, &key.PublicKey, ca.key)
	if err != nil {
		t.Fatalf("인증서 발급 실패: %v", err)
	}

	certPath := filepath.Join(dir, name+".crt")
	keyPath := filepath.Join(dir, name+".key")
	caPath := filepath.Join(dir, "ca.crt")

	writeFile(t, certPath, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der}))
	writeFile(t, keyPath, pem.EncodeToMemory(&pem.Block{
		Type: "RSA PRIVATE KEY", Bytes: x509.MarshalPKCS1PrivateKey(key),
	}))
	writeFile(t, caPath, ca.pem)

	return Config{CertFile: certPath, KeyFile: keyPath, CAFile: caPath}
}

func writeFile(t *testing.T, path string, body []byte) {
	t.Helper()
	if err := os.WriteFile(path, body, 0o600); err != nil {
		t.Fatalf("%s 작성 실패: %v", path, err)
	}
}

// serveTLS 는 mTLS 서버를 띄우고 주소를 돌려준다.
func serveTLS(t *testing.T, cfg Config) string {
	t.Helper()

	creds, err := cfg.ServerCredentials()
	if err != nil {
		t.Fatalf("ServerCredentials 실패: %v", err)
	}

	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("리슨 실패: %v", err)
	}

	srv := grpc.NewServer(grpc.Creds(creds))
	healthpb.RegisterHealthServer(srv, health.NewServer())

	go func() { _ = srv.Serve(lis) }()
	t.Cleanup(srv.Stop)

	return lis.Addr().String()
}

func checkHealth(t *testing.T, addr string, opt grpc.DialOption) error {
	t.Helper()

	cc, err := grpc.NewClient("passthrough:///"+addr, opt)
	if err != nil {
		return err
	}
	defer cc.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	_, err = healthpb.NewHealthClient(cc).Check(ctx, &healthpb.HealthCheckRequest{})
	return err
}

func TestMutualTLSHandshake(t *testing.T) {
	dir := t.TempDir()
	ca := newCA(t)

	serverCfg := ca.issue(t, dir, "agent", net.ParseIP("127.0.0.1"))
	clientCfg := ca.issue(t, dir, "control", nil)

	addr := serveTLS(t, serverCfg)

	t.Run("올바른 인증서로 붙는다", func(t *testing.T) {
		creds, err := clientCfg.ClientCredentials()
		if err != nil {
			t.Fatalf("ClientCredentials 실패: %v", err)
		}
		if err := checkHealth(t, addr, grpc.WithTransportCredentials(creds)); err != nil {
			t.Fatalf("정상 클라이언트가 거부됐다: %v", err)
		}
	})

	// 이게 이 테스트의 핵심이다. 인증서 없이도 붙는다면 TLS 를 켠 의미가 없다 —
	// :9100 에 닿는 누구나 라인레이트 송신을 시킬 수 있게 된다.
	t.Run("평문 클라이언트는 거부된다", func(t *testing.T) {
		if err := checkHealth(t, addr, grpc.WithTransportCredentials(insecure.NewCredentials())); err == nil {
			t.Fatal("평문 클라이언트가 통과했다 — 클라이언트 인증이 걸려 있지 않다")
		}
	})

	// 다른 CA 가 서명한 인증서도 거부돼야 한다.
	t.Run("다른 CA 의 인증서는 거부된다", func(t *testing.T) {
		otherDir := t.TempDir()
		otherCA := newCA(t)
		rogue := otherCA.issue(t, otherDir, "rogue", nil)

		// 서버 검증은 우리 CA 로 하되, 제시하는 클라이언트 인증서만 남의 것으로 둔다.
		rogue.CAFile = clientCfg.CAFile

		creds, err := rogue.ClientCredentials()
		if err != nil {
			t.Fatalf("ClientCredentials 실패: %v", err)
		}
		if err := checkHealth(t, addr, grpc.WithTransportCredentials(creds)); err == nil {
			t.Fatal("남의 CA 가 서명한 인증서가 통과했다")
		}
	})
}
