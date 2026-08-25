// Package fleet — 제어부가 관리하는 장비와 데이터플레인 인스턴스의 목록.
//
// 오케스트레이터가 없으므로 "어디에 무엇이 있어야 하는가"를 알려 주는 주체가
// 없다. 그 자리를 이 설정 파일이 대신한다. 그리고 이 구조가 오히려 더 정확하다 —
// **기대(설정)와 실측(HelloResponse)을 따로 들고 대조**할 수 있기 때문이다.
//
//	기대 O, 실측 X  → 컨테이너가 죽었거나 도달 불가
//	기대 O, 실측 O, PF 불일치 → 잘못된 BDF 를 잡았다
//	기대 X, 실측 O  → 설정에 없는 인스턴스가 떠 있다
//
// 노드 allocatable 을 합산하는 방식으로는 "몇 개가 Ready 인가"밖에 알 수 없어
// 이 셋이 구분되지 않는다.
package fleet

import (
	"bytes"
	"context"
	"fmt"
	"net"
	"os"
	"strconv"

	yaml "go.yaml.in/yaml/v3"
)

// Instance 는 데이터플레인 하나 = PF 하나다.
type Instance struct {
	ID   int    `yaml:"id"`
	Port int    `yaml:"port"`
	PF   string `yaml:"pf"`

	// 아래는 Load 가 채운다 (설정 파일에 쓰지 않는다).
	Machine string `yaml:"-"`
	Addr    string `yaml:"-"`
	Name    string `yaml:"-"`
}

type Machine struct {
	Name      string     `yaml:"name"`
	Address   string     `yaml:"address"`
	Instances []Instance `yaml:"instances"`
}

type Config struct {
	Machines []Machine `yaml:"machines"`
}

// InstanceName 은 인스턴스의 정본 이름이다.
//
// compose 의 `HOSTNAME` 을 **반드시 이 값과 같게** 둬야 한다. 그 값이 EAL
// --file-prefix 와 텔레메트리 node_id 로 그대로 쓰이므로, 어긋나면 설정상의
// 인스턴스와 보고해 오는 인스턴스를 이어 붙일 수 없다.
func InstanceName(machine string, id int) string {
	return fmt.Sprintf("%s-dp%d", machine, id)
}

// Load 는 설정을 읽고 검증한 뒤 파생 필드를 채운다.
func Load(path string) (*Config, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("함대 설정을 읽지 못했다 (%s): %w", path, err)
	}

	var cfg Config
	dec := yaml.NewDecoder(bytes.NewReader(raw))
	dec.KnownFields(true) // 오타 난 키를 조용히 무시하지 않는다
	if err := dec.Decode(&cfg); err != nil {
		return nil, fmt.Errorf("함대 설정 파싱 실패 (%s): %w", path, err)
	}

	if err := cfg.normalize(); err != nil {
		return nil, fmt.Errorf("함대 설정이 올바르지 않다 (%s): %w", path, err)
	}
	return &cfg, nil
}

func (c *Config) normalize() error {
	if len(c.Machines) == 0 {
		return fmt.Errorf("machines 가 비어 있다")
	}

	seenMachine := map[string]bool{}

	for mi := range c.Machines {
		m := &c.Machines[mi]

		if m.Name == "" {
			return fmt.Errorf("machines[%d].name 이 비어 있다", mi)
		}
		if seenMachine[m.Name] {
			return fmt.Errorf("장비 이름이 중복됐다: %q", m.Name)
		}
		seenMachine[m.Name] = true

		if m.Address == "" {
			return fmt.Errorf("장비 %q 의 address 가 비어 있다", m.Name)
		}
		if len(m.Instances) == 0 {
			return fmt.Errorf("장비 %q 에 instances 가 없다", m.Name)
		}

		// PF 는 장비 안에서만 유일하면 된다 — 같은 BDF 가 다른 장비에
		// 존재하는 것은 정상이고 오히려 흔하다(동일 사양 장비 2대).
		seenID := map[int]bool{}
		seenPort := map[int]bool{}
		seenPF := map[string]bool{}

		for ii := range m.Instances {
			in := &m.Instances[ii]

			if in.ID < 0 {
				return fmt.Errorf("장비 %q instances[%d].id 가 음수다", m.Name, ii)
			}
			if seenID[in.ID] {
				return fmt.Errorf("장비 %q 에서 id %d 가 중복됐다", m.Name, in.ID)
			}
			seenID[in.ID] = true

			if in.Port < 1 || in.Port > 65535 {
				return fmt.Errorf("장비 %q id %d 의 port 가 범위를 벗어났다: %d",
					m.Name, in.ID, in.Port)
			}
			if seenPort[in.Port] {
				return fmt.Errorf("장비 %q 에서 port %d 가 중복됐다", m.Name, in.Port)
			}
			seenPort[in.Port] = true

			if in.PF == "" {
				return fmt.Errorf("장비 %q id %d 의 pf 가 비어 있다", m.Name, in.ID)
			}
			if seenPF[in.PF] {
				return fmt.Errorf("장비 %q 에서 pf %q 가 중복됐다 — 한 PF 를 두 인스턴스가 쓸 수 없다",
					m.Name, in.PF)
			}
			seenPF[in.PF] = true

			in.Machine = m.Name
			in.Name = InstanceName(m.Name, in.ID)
			// JoinHostPort 가 IPv6 주소의 대괄호까지 처리한다.
			in.Addr = net.JoinHostPort(m.Address, strconv.Itoa(in.Port))
		}
	}
	return nil
}

// Instances 는 전 장비의 인스턴스를 평탄화해 돌려준다.
func (c *Config) Instances() []Instance {
	out := make([]Instance, 0, len(c.Machines)*4)
	for mi := range c.Machines {
		out = append(out, c.Machines[mi].Instances...)
	}
	return out
}

// ByName 은 이름으로 색인한 기대 인벤토리다 (실측과 대조할 때 쓴다).
func (c *Config) ByName() map[string]Instance {
	out := map[string]Instance{}
	for _, in := range c.Instances() {
		out[in.Name] = in
	}
	return out
}

// Resolve 는 registry.Resolver 를 만족한다. 주소 → 인스턴스 이름.
//
// 설정은 정적이라 오류가 날 일이 없고 ctx 도 쓰지 않는다. 시그니처가
// 인터페이스에 맞춰져 있는 것은 나중에 동적 출처(예: 파일 변경 감시,
// 디스커버리 서비스)를 끼울 자리를 남겨 두기 위해서다.
func (c *Config) Resolve(context.Context) (map[string]string, error) {
	out := map[string]string{}
	for _, in := range c.Instances() {
		out[in.Addr] = in.Name
	}
	return out, nil
}
