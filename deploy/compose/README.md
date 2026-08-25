# deploy/compose

장비마다 데이터플레인 인스턴스를 띄우고, 제어부 한 대가 함대 전체를 관측한다.

| 파일 | 어디서 쓰나 |
|---|---|
| `docker-compose.dataplane.yml` | **생성 장비마다** — PF 하나당 (dataplane + agent) 쌍 하나 |
| `dataplane.env.example` | → `.env` 로 복사해 장비별 값을 채운다 |
| `docker-compose.control.yml` | **제어 장비 한 대에서만** |
| `fleet.example.yaml` | → `fleet.yaml` 로 복사. 제어부가 읽는 기대 인벤토리 |

전체 절차와 검증은 [docs/DEPLOYMENT.md](../../docs/DEPLOYMENT.md).

## 반드시 맞춰야 하는 세 쌍

이 셋이 어긋나는 것이 가장 흔한 실패다.

| `.env` (생성 장비) | `fleet.yaml` (제어부) | 어긋나면 |
|---|---|---|
| `MIR_MACHINE` | `machines[].name` | 인스턴스 이름(`<machine>-dp<id>`)이 달라져 **전부 `unreachable`** |
| `MIR_DP<i>_PORT` | 해당 인스턴스의 `port` | 그 인스턴스만 `unreachable` |
| `MIR_DP<i>_PF` | 해당 인스턴스의 `pf` | `pf-mismatch` 로 잡힌다 |

`MIR_MACHINE` 은 compose 가 `HOSTNAME` 으로 넣어 주고, 그 값이 EAL
`--file-prefix` 와 텔레메트리 `node_id` 로 그대로 쓰인다. 그래서 이름이 곧
인스턴스의 정체성이다.

## `.env` 값을 어디서 얻나

| 값 | 출처 |
|---|---|
| `MIR_DP*_PF`, `MIR_DP*_VFIO_GROUP` | `sudo ../host/20-bind-vfio.sh` 출력 (`<bdf> → /dev/vfio/<group>`) |
| `MIR_DP*_CPUSET` | 커널 cmdline 의 `isolcpus` 안에서 나눈다 → [../host/10-kernel-cmdline.md](../host/10-kernel-cmdline.md) |
| `MIR_MEM_MB` | `이 값 × 인스턴스 수 ≤ 호스트 hugepage 총량` |
| `MIR_TLS_DIR` | `../host/50-gen-certs.sh` 출력물을 배치한 디렉터리 |

## 첫 기동

인스턴스 하나부터 확인한다.

```bash
cp dataplane.env.example .env && $EDITOR .env
COMPOSE_PROFILES= docker compose -f docker-compose.dataplane.yml up -d
docker logs -f "$(grep ^MIR_MACHINE .env | cut -d= -f2)-dp0"
```

기동 로그의 `lcores` 가 `.env` 의 `MIR_DP0_CPUSET` 과 **정확히 일치**하는지
확인한다. 다르면 cpuset 이 안 먹은 것이고, 그 상태로는 배타 코어 배치가
통째로 무의미하다.

확인되면 `.env` 의 `COMPOSE_PROFILES` 로 늘린다.

## 개수·리소스를 바꾸려면

제어부에 그런 API 는 없다. 이 디렉터리의 파일이 소유한다.

```bash
$EDITOR .env
docker compose -f docker-compose.dataplane.yml up -d   # 바뀐 것만 재생성
```

DPDK 가 런타임 lcore 변경을 지원하지 않으므로 어떤 방식이든 재시작을 수반한다.
그래서 선언적 파일이 맞는 자리다.

## 커밋하지 말 것

`.env` · `fleet.yaml` · `certs/` 는 장비별 설정과 비밀이다. 예제 파일만
저장소에 둔다.
