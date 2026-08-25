package fleet

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func write(t *testing.T, body string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "fleet.yaml")
	if err := os.WriteFile(path, []byte(body), 0o600); err != nil {
		t.Fatalf("임시 설정 작성 실패: %v", err)
	}
	return path
}

const twoMachines = `
machines:
  - name: gen-1
    address: 10.10.40.121
    instances:
      - { id: 0, port: 9100, pf: "0000:43:00.0" }
      - { id: 1, port: 9101, pf: "0000:43:00.1" }
  - name: gen-2
    address: 10.10.40.122
    instances:
      - { id: 0, port: 9100, pf: "0000:43:00.0" }
`

func TestLoadDerivesNameAndAddr(t *testing.T) {
	cfg, err := Load(write(t, twoMachines))
	if err != nil {
		t.Fatalf("Load 실패: %v", err)
	}

	got := cfg.ByName()
	if len(got) != 3 {
		t.Fatalf("인스턴스 3개를 기대했으나 %d개", len(got))
	}

	in, ok := got["gen-1-dp1"]
	if !ok {
		t.Fatalf("gen-1-dp1 이 없다: %v", got)
	}
	if in.Addr != "10.10.40.121:9101" {
		t.Errorf("Addr = %q, 기대 10.10.40.121:9101", in.Addr)
	}
	if in.Machine != "gen-1" {
		t.Errorf("Machine = %q, 기대 gen-1", in.Machine)
	}
}

// 같은 BDF 가 다른 장비에 있는 것은 정상이다 — 동일 사양 장비 2대면 흔하다.
func TestSamePFOnDifferentMachinesIsAllowed(t *testing.T) {
	if _, err := Load(write(t, twoMachines)); err != nil {
		t.Fatalf("장비 간 PF 중복이 거부됐다: %v", err)
	}
}

func TestResolveMapsAddrToName(t *testing.T) {
	cfg, err := Load(write(t, twoMachines))
	if err != nil {
		t.Fatalf("Load 실패: %v", err)
	}

	got, err := cfg.Resolve(context.Background())
	if err != nil {
		t.Fatalf("Resolve 실패: %v", err)
	}
	if got["10.10.40.122:9100"] != "gen-2-dp0" {
		t.Errorf("Resolve[10.10.40.122:9100] = %q, 기대 gen-2-dp0", got["10.10.40.122:9100"])
	}
	if len(got) != 3 {
		t.Errorf("주소 3개를 기대했으나 %d개", len(got))
	}
}

func TestIPv6AddressIsBracketed(t *testing.T) {
	cfg, err := Load(write(t, `
machines:
  - name: gen-1
    address: "fd00::1"
    instances:
      - { id: 0, port: 9100, pf: "0000:43:00.0" }
`))
	if err != nil {
		t.Fatalf("Load 실패: %v", err)
	}
	if got := cfg.Instances()[0].Addr; got != "[fd00::1]:9100" {
		t.Errorf("Addr = %q, 기대 [fd00::1]:9100", got)
	}
}

func TestRejects(t *testing.T) {
	cases := map[string]struct{ body, want string }{
		"PF 중복": {`
machines:
  - name: gen-1
    address: 10.0.0.1
    instances:
      - { id: 0, port: 9100, pf: "0000:43:00.0" }
      - { id: 1, port: 9101, pf: "0000:43:00.0" }
`, "pf"},
		"포트 중복": {`
machines:
  - name: gen-1
    address: 10.0.0.1
    instances:
      - { id: 0, port: 9100, pf: "0000:43:00.0" }
      - { id: 1, port: 9100, pf: "0000:43:00.1" }
`, "port"},
		"장비 이름 중복": {`
machines:
  - name: gen-1
    address: 10.0.0.1
    instances:
      - { id: 0, port: 9100, pf: "0000:43:00.0" }
  - name: gen-1
    address: 10.0.0.2
    instances:
      - { id: 0, port: 9100, pf: "0000:43:00.0" }
`, "중복"},
		"인스턴스 없음": {`
machines:
  - name: gen-1
    address: 10.0.0.1
    instances: []
`, "instances"},
		"오타 난 키": {`
machines:
  - name: gen-1
    addres: 10.0.0.1
    instances:
      - { id: 0, port: 9100, pf: "0000:43:00.0" }
`, "field"},
	}

	for label, tc := range cases {
		t.Run(label, func(t *testing.T) {
			_, err := Load(write(t, tc.body))
			if err == nil {
				t.Fatalf("거부돼야 하는데 통과했다")
			}
			if !strings.Contains(err.Error(), tc.want) {
				t.Errorf("오류 메시지에 %q 가 없다: %v", tc.want, err)
			}
		})
	}
}

// 저장소에 넣어 둔 예제가 실제로 파싱되는지 확인한다. 예제가 깨지면
// 처음 세팅하는 사람이 가장 먼저 걸려 넘어진다.
func TestShippedExampleParses(t *testing.T) {
	path := filepath.Join("..", "..", "..", "deploy", "compose", "fleet.example.yaml")
	if _, err := os.Stat(path); err != nil {
		t.Skipf("예제 파일이 없다: %v", err)
	}

	cfg, err := Load(path)
	if err != nil {
		t.Fatalf("예제 함대 설정이 파싱되지 않는다: %v", err)
	}
	if len(cfg.Instances()) == 0 {
		t.Fatal("예제에 인스턴스가 없다")
	}
	for _, in := range cfg.Instances() {
		if in.Name == "" || in.Addr == "" || in.PF == "" {
			t.Errorf("파생 필드가 비었다: %+v", in)
		}
	}
}
