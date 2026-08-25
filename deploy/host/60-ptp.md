# 60 — 장비 간 시계 동기 (PTP)

> **장비가 한 대뿐이면 이 문서는 건너뛴다.** 두 대 이상을 한 제어부가 묶는
> 순간부터 전제 조건이 된다.

## 왜 필요한가

데이터플레인이 올리는 `TelemetrySnapshot.ts_ns` 와 hello 프레임의 타임스탬프는
`CLOCK_REALTIME` 을 쓴다. 장비마다 이 시계가 제각각이면 다음이 전부 무의미해진다.

- **장비 간 텔레메트리 상관** — gen-1 의 10:00:00.000 과 gen-2 의 10:00:00.000 이
  같은 순간이 아니면 두 장비의 송신량을 같은 시간축에 놓을 수 없다
- **지연 측정** — 한 장비에서 쏘고 다른 장비에서 받는 구성에서 시계 오차가
  그대로 측정 오차가 된다. NTP 의 밀리초급 오차는 마이크로초를 다투는
  계측기에서 쓸 수 없다
- **조율된 시작** — `StartScenarioRequest.start_at_ns` 가 공통 시각을 전제한다
  (`proto/dataplane.proto`)

**어떤 오케스트레이터도 이걸 대신 해 주지 않는다.** 호스트 커널과 NIC 하드웨어
계층의 문제이고, k8s 를 쓰든 Compose 를 쓰든 똑같이 별도로 풀어야 한다.

## 하드웨어 지원

검증기 X710(i40e)과 목표기 E810(ice) 모두 **하드웨어 타임스탬핑(PHC)** 을
지원한다. 소프트웨어 타임스탬핑으로 떨어지면 정밀도가 크게 나빠지므로 반드시
확인한다.

```bash
sudo apt-get install -y linuxptp ethtool

# PTP Hardware Clock 존재 확인
ls /dev/ptp*

# NIC 이 하드웨어 타임스탬핑을 광고하는지 (관리 NIC 말고 생성용 NIC 으로)
ethtool -T enp67s0f0
```

출력에 다음이 있어야 한다.

```
PTP Hardware Clock: 0
Hardware Transmit Timestamp Modes: ... on ...
Hardware Receive Filter Modes:     ... all ...
```

> ⚠️ **vfio-pci 에 바인딩한 뒤에는 `ethtool` 이 그 포트를 보지 못한다.**
> 커널이 더 이상 소유하지 않기 때문이다. 위 확인은 `20-bind-vfio.sh` **이전**에
> 하거나, 동기화 전용으로 남겨 둔 별도 포트에서 한다.

## 구성 — 동기 전용 포트를 따로 둔다

가장 깔끔한 배치는 **PTP 를 관리망 포트에서 돌리고, 생성용 PF 는 전부
vfio 로 넘기는** 것이다. 생성용 포트를 커널에 남겨 두면 vfio 로 못 넘긴다.

```
[관리 NIC / I210]  ← ptp4l + phc2sys, 커널이 소유
[생성 NIC / X710 4포트] ← 전부 vfio-pci, DPDK 가 소유
```

시스템 시계(`CLOCK_REALTIME`)만 맞으면 되므로 이 배치로 충분하다 —
데이터플레인이 읽는 것은 PHC 가 아니라 시스템 시계다.

### 마스터 (기준 장비 한 대)

```bash
sudo tee /etc/linuxptp/ptp4l.conf >/dev/null <<'EOF'
[global]
clientOnly              0
time_stamping           hardware
tx_timestamp_timeout    10
[eth0]
EOF

sudo systemctl enable --now ptp4l@eth0
sudo systemctl enable --now phc2sys@eth0
```

### 슬레이브 (나머지 장비)

`clientOnly 1` 로 두는 것 외에는 동일하다.

`phc2sys` 가 **PHC → 시스템 시계** 방향으로 흘려주는 역할이라 빠뜨리면 안 된다.
`ptp4l` 만 돌리면 NIC 시계만 맞고 `CLOCK_REALTIME` 은 그대로다.

### NTP 와 충돌시키지 말 것

`chronyd`/`systemd-timesyncd` 가 동시에 시스템 시계를 건드리면 두 소스가
서로를 밀어내며 진동한다. 하나만 남긴다.

```bash
sudo systemctl disable --now chronyd systemd-timesyncd 2>/dev/null || true
```

## 확인

```bash
# 오프셋이 수십~수백 ns 수준으로 수렴해야 한다
journalctl -u ptp4l@eth0 -f | grep -E 'master offset'

# 시스템 시계까지 반영됐는지
journalctl -u phc2sys@eth0 -f
```

`40-verify-node.sh` 가 `MIR_REQUIRE_PTP=1` 일 때 이 상태를 점검한다.
장비 한 대 구성에서는 기본적으로 검사하지 않는다.

## 남은 이슈

- **허용 오차를 아직 정하지 않았다.** "수백 ns" 는 관례적인 목표치일 뿐이고,
  판정 규칙이 확정돼야(REQUIREMENTS 7절) 실제 요구 정밀도가 나온다.
- 스위치를 경유하면 **boundary clock 또는 transparent clock** 지원 여부가
  정밀도를 좌우한다. 직결이 아니면 스위치 사양을 먼저 확인할 것.
