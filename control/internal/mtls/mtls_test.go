package mtls

import (
	"strings"
	"testing"
)

func TestValidate(t *testing.T) {
	cases := []struct {
		label   string
		cfg     Config
		wantErr string // 비어 있으면 통과해야 한다
	}{
		{"완전한 TLS 설정", Config{CertFile: "c", KeyFile: "k", CAFile: "a"}, ""},
		{"명시적 평문", Config{Insecure: true}, ""},

		// 가장 나쁜 결과는 "보안을 켰다고 믿는데 평문이 나가는" 것이다.
		{"평문과 인증서 동시 지정", Config{Insecure: true, CertFile: "c"}, "둘 중 하나만"},

		{"인증서 누락", Config{KeyFile: "k", CAFile: "a"}, "모두 필요하다"},
		{"키 누락", Config{CertFile: "c", CAFile: "a"}, "모두 필요하다"},
		{"CA 누락", Config{CertFile: "c", KeyFile: "k"}, "모두 필요하다"},

		// 아무것도 설정하지 않은 채로 조용히 평문이 되면 안 된다.
		{"전부 비었음", Config{}, "모두 필요하다"},
	}

	for _, tc := range cases {
		t.Run(tc.label, func(t *testing.T) {
			err := tc.cfg.Validate()
			if tc.wantErr == "" {
				if err != nil {
					t.Fatalf("통과해야 하는데 실패했다: %v", err)
				}
				return
			}
			if err == nil {
				t.Fatalf("거부돼야 하는데 통과했다")
			}
			if !strings.Contains(err.Error(), tc.wantErr) {
				t.Errorf("오류에 %q 가 없다: %v", tc.wantErr, err)
			}
		})
	}
}

// 설정이 잘못됐으면 credentials 를 만들지 못하고 오류로 끝나야 한다.
// 조용히 insecure 로 폴백하면 안 된다.
func TestCredentialsRejectBadConfig(t *testing.T) {
	bad := Config{CertFile: "c"} // 키·CA 없음

	if _, err := bad.ServerCredentials(); err == nil {
		t.Error("ServerCredentials 가 잘못된 설정을 받아들였다")
	}
	if _, err := bad.ClientCredentials(); err == nil {
		t.Error("ClientCredentials 가 잘못된 설정을 받아들였다")
	}
}

func TestInsecureYieldsCredentials(t *testing.T) {
	c := Config{Insecure: true}

	if c.Enabled() {
		t.Error("Insecure 인데 Enabled() 가 참이다")
	}
	if _, err := c.ServerCredentials(); err != nil {
		t.Errorf("ServerCredentials 실패: %v", err)
	}
	if _, err := c.ClientCredentials(); err != nil {
		t.Errorf("ClientCredentials 실패: %v", err)
	}
}

// 파일이 없으면 명확히 실패해야 한다 (경로 오타를 조용히 넘기지 않는다).
func TestMissingFilesFail(t *testing.T) {
	c := Config{CertFile: "/nope/c.pem", KeyFile: "/nope/k.pem", CAFile: "/nope/ca.pem"}
	if _, err := c.ServerCredentials(); err == nil {
		t.Error("없는 파일 경로가 통과했다")
	}
}
