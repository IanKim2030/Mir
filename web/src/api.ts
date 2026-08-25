// api.ts — mir-control REST 의 타입과 페처.
// 형태는 control/internal/api 의 JSON 태그와 반드시 일치한다.

export interface PortStats {
  txPkts: number; txBytes: number; txDrop: number; txErr: number;
  rxPkts: number; rxBytes: number; rxDrop: number; rxErr: number;
}

export interface PortView {
  portId: number;
  driver: string;
  mac: string;
  numaNode: number;
  deviceSpec: string;
  stats?: PortStats;
}

export interface RxClass {
  tcpSyn: number; tcpSynAck: number; tcpRst: number; tcpFin: number;
  tcpAck: number; tcpOther: number; udp: number; nonIp: number;
}

export interface HandshakeStats {
  sessions: number; sent: number; synAck: number; completed: number;
  refused: number; timedOut: number;
  rttMinUs: number; rttAvgUs: number; rttMaxUs: number;
  // Phase 5a
  established: number; reqSent: number; responded: number; closed: number;
  bytesRx: number; http2xx: number;
  // Phase 5b
  tlsOk: number; tlsFailed: number;
}

export type InstanceState =
  | 'ok' | 'unreachable' | 'connecting' | 'no-ports' | 'pf-mismatch';

export interface InstanceView {
  name: string;
  machine: string;
  addr: string;
  expectedPf: string;
  state: InstanceState;
  message?: string;
  connected: boolean;
  lastSeen?: string;
  ports?: PortView[];
  lcores?: number[];
  mainLcore?: number;
  dataplaneVersion?: string;
  eventDrop?: number;
  activeScenario?: string;
  txLcores?: number;
  txDrop?: number;
  rx?: RxClass;
  handshake?: HandshakeStats;
}

export interface MachineView {
  name: string; address: string; expected: number; ready: number;
}
export interface CapacityResponse {
  machines: MachineView[]; expected: number; ready: number;
}

export interface EventView {
  instance: string;
  tsNs: number;
  time: string;
  kind: string;
  portId: number;
  flowKey?: string;
  rttUs?: number;
  detail?: string;
}
export interface EventsResponse { seen: number; events: EventView[]; }

// SSE /api/stream 이 한 프레임에 밀어 주는 스냅샷.
export interface Snapshot {
  instances: InstanceView[];
  capacity: CapacityResponse;
  events: EventsResponse;
}

async function getJSON<T>(path: string): Promise<T> {
  const r = await fetch(path, { headers: { Accept: 'application/json' } });
  if (!r.ok) throw new Error(`${path} → ${r.status}`);
  return r.json() as Promise<T>;
}

export const api = {
  dataplanes: () => getJSON<InstanceView[]>('/api/dataplanes'),
  capacity: () => getJSON<CapacityResponse>('/api/capacity'),
  events: () => getJSON<EventsResponse>('/api/events'),
  startScenario: (body: unknown) =>
    fetch('/api/scenarios/start', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    }).then((r) => r.json()),
  stopScenario: (body: unknown) =>
    fetch('/api/scenarios/stop', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    }).then((r) => r.json()),
};
