import { useCallback, useEffect, useRef, useState } from 'react'
import {
  api,
  CapacityResponse,
  EventView,
  InstanceView,
  InstanceState,
  Snapshot,
} from './api'
import { TxChart } from './TxChart'
import { Builder } from './Builder'

const HISTORY = 120 // 차트에 남길 표본 수 (0.5s × 120 = 60s)

const STATE_LABEL: Record<InstanceState, string> = {
  ok: '정상',
  unreachable: '연결안됨',
  connecting: '연결중',
  'no-ports': '포트없음',
  'pf-mismatch': 'PF불일치',
}

function fmtRate(pps: number): string {
  if (pps >= 1e6) return `${(pps / 1e6).toFixed(2)} Mpps`
  if (pps >= 1e3) return `${(pps / 1e3).toFixed(1)} kpps`
  return `${Math.round(pps)} pps`
}
function fmtGbps(bps: number): string {
  if (bps >= 1e9) return `${(bps / 1e9).toFixed(2)} Gbps`
  if (bps >= 1e6) return `${(bps / 1e6).toFixed(1)} Mbps`
  return `${(bps / 1e3).toFixed(0)} kbps`
}

const noop = () => {}

interface Prev {
  t: number
  tx: Record<string, number> // name -> txPkts
  rx: Record<string, number>
  txb: Record<string, number>
}

interface Rate {
  txPps: number
  rxPps: number
  txBps: number
}

export default function App() {
  const [instances, setInstances] = useState<InstanceView[]>([])
  const [capacity, setCapacity] = useState<CapacityResponse | null>(null)
  const [events, setEvents] = useState<EventView[]>([])
  const [rates, setRates] = useState<Record<string, Rate>>({})
  const [connErr, setConnErr] = useState<string | null>(null)

  // 차트 롤링 버퍼
  const [xs, setXs] = useState<number[]>([])
  const [txHist, setTxHist] = useState<number[]>([])
  const [rxHist, setRxHist] = useState<number[]>([])
  const [bwHist, setBwHist] = useState<number[]>([])
  const [tab, setTab] = useState<'dashboard' | 'builder'>('dashboard')

  const prevRef = useRef<Prev | null>(null)

  // SSE 한 프레임을 처리한다 — 인스턴스·용량·이벤트 갱신 + pps/대역 델타 + 차트.
  const processSnapshot = useCallback((snap: Snapshot) => {
    const dps = snap.instances
    setInstances(dps)
    setCapacity(snap.capacity)
    setEvents(snap.events.events)

    {
      // pps 델타
      const now = Date.now() / 1000
      const curTx: Record<string, number> = {}
      const curRx: Record<string, number> = {}
      const curTxb: Record<string, number> = {}
      for (const d of dps) {
        const s = d.ports?.[0]?.stats
        if (s) {
          curTx[d.name] = s.txPkts
          curRx[d.name] = s.rxPkts
          curTxb[d.name] = s.txBytes
        }
      }
      const prev = prevRef.current
      const nextRates: Record<string, Rate> = {}
      let aggTx = 0
      let aggRx = 0
      let aggTxBps = 0
      if (prev) {
        const dt = now - prev.t
        if (dt > 0) {
          for (const name of Object.keys(curTx)) {
            const txPps = Math.max(0, (curTx[name] - (prev.tx[name] ?? curTx[name])) / dt)
            const rxPps = Math.max(0, (curRx[name] - (prev.rx[name] ?? curRx[name])) / dt)
            const txBps = Math.max(0, ((curTxb[name] - (prev.txb[name] ?? curTxb[name])) * 8) / dt)
            nextRates[name] = { txPps, rxPps, txBps }
            aggTx += txPps
            aggRx += rxPps
            aggTxBps += txBps
          }
        }
      }
      prevRef.current = { t: now, tx: curTx, rx: curRx, txb: curTxb }
      setRates(nextRates)

      if (prev) {
        setXs((a) => [...a, now].slice(-HISTORY))
        setTxHist((a) => [...a, aggTx / 1e6].slice(-HISTORY))
        setRxHist((a) => [...a, aggRx / 1e6].slice(-HISTORY))
        setBwHist((a) => [...a, aggTxBps / 1e9].slice(-HISTORY))
      }
    }
  }, [])

  // SSE 구독 — 폴링을 대체한다. EventSource 는 끊기면 표준으로 자동 재연결한다.
  useEffect(() => {
    const es = new EventSource('/api/stream')
    es.onopen = () => setConnErr(null)
    es.onmessage = (e) => {
      try {
        processSnapshot(JSON.parse(e.data) as Snapshot)
        setConnErr(null)
      } catch (err) {
        setConnErr(String(err))
      }
    }
    es.onerror = () => setConnErr('스트림 재연결 중…')
    return () => es.close()
  }, [processSnapshot])

  const active = instances.find((d) => d.handshake && d.handshake.sessions > 0)?.handshake

  return (
    <div className="app">
      <header className="topbar">
        <div className="brand">
          <span className="logo">◆</span> Mir
          <span className="tagline">패킷 제너레이터</span>
        </div>
        <nav className="tabs">
          <button className={tab === 'dashboard' ? 'tab on' : 'tab'} onClick={() => setTab('dashboard')}>
            대시보드
          </button>
          <button className={tab === 'builder' ? 'tab on' : 'tab'} onClick={() => setTab('builder')}>
            시나리오 빌더
          </button>
        </nav>
        <div className="fleet">
          {capacity && (
            <span className={capacity.ready === capacity.expected ? 'pill ok' : 'pill warn'}>
              인스턴스 {capacity.ready}/{capacity.expected}
            </span>
          )}
          <span className="pill">장비 {capacity?.machines.length ?? 0}</span>
          <span className={connErr ? 'pill err' : 'pill ok'}>
            {connErr ? '제어부 끊김' : '실시간'}
          </span>
        </div>
      </header>

      {tab === 'builder' ? (
        <Builder instances={instances} onStarted={noop} />
      ) : (
        <>
      <ScenarioBar instances={instances} onChange={noop} />

      <div className="grid">
        {/* 인스턴스 인벤토리 */}
        <section className="card span2">
          <h2>인스턴스</h2>
          <table className="tbl">
            <thead>
              <tr>
                <th>이름</th><th>상태</th><th>PF</th><th>TX</th><th>RX</th>
                <th>대역(TX)</th><th>시나리오</th>
              </tr>
            </thead>
            <tbody>
              {instances.map((d) => {
                const r = rates[d.name]
                return (
                  <tr key={d.name}>
                    <td className="mono">{d.name}</td>
                    <td><span className={`badge ${d.state}`}>{STATE_LABEL[d.state]}</span></td>
                    <td className="mono dim">{d.expectedPf}</td>
                    <td className="num">{r ? fmtRate(r.txPps) : '—'}</td>
                    <td className="num">{r ? fmtRate(r.rxPps) : '—'}</td>
                    <td className="num">{r ? fmtGbps(r.txBps) : '—'}</td>
                    <td className="mono dim">{d.activeScenario || '—'}</td>
                  </tr>
                )
              })}
              {instances.length === 0 && (
                <tr><td colSpan={7} className="dim center">인스턴스 없음</td></tr>
              )}
            </tbody>
          </table>
        </section>

        {/* 영역 1 — 송신 카운터 */}
        <section className="card">
          <h2>송신 카운터 — 처리량 (Mpps)</h2>
          <TxChart
            xs={xs}
            title="함대 합산 pps"
            unit="Mpps"
            series={[
              { label: 'TX', stroke: '#4fd1c5', values: txHist },
              { label: 'RX', stroke: '#f6ad55', values: rxHist },
            ]}
          />
        </section>

        {/* 영역 1b — 대역 */}
        <section className="card">
          <h2>송신 대역 (Gbps)</h2>
          <TxChart
            xs={xs}
            title="함대 합산 대역 (on-wire 아님, L2)"
            unit="Gbps"
            series={[{ label: 'TX', stroke: '#63b3ed', values: bwHist }]}
          />
        </section>

        {/* 영역 2 — 실시간 패킷 로그 */}
        <section className="card">
          <h2>실시간 판정 로그</h2>
          <div className="log">
            {events.length === 0 && <div className="dim center">이벤트 없음</div>}
            {events.slice().reverse().map((e, i) => (
              <div className="logrow" key={`${e.tsNs}-${i}`}>
                <span className="logtime">{new Date(e.time).toLocaleTimeString()}</span>
                <span className={`logkind ${e.kind}`}>{e.kind.replace('KIND_', '')}</span>
                <span className="mono dim">{e.instance}</span>
                <span className="logflow mono">{e.flowKey}</span>
                {e.rttUs ? <span className="dim">{e.rttUs}µs</span> : null}
                <span className="logdetail">{e.detail}</span>
              </div>
            ))}
          </div>
        </section>

        {/* 영역 3 — 세션 판정 */}
        <section className="card">
          <h2>세션 판정 (Mode B)</h2>
          {active ? (
            <div className="verdict">
              <Stat label="세션" v={active.sessions} />
              <Stat label="SYN-ACK" v={active.synAck} />
              <Stat label="established" v={active.established} />
              <Stat label="응답" v={active.responded} />
              <Stat label="HTTP 2xx" v={active.http2xx} good />
              <Stat label="closed" v={active.closed} />
              <Stat label="TLS ok" v={active.tlsOk} good />
              <Stat label="TLS 실패" v={active.tlsFailed} bad={active.tlsFailed > 0} />
              <Stat label="refused" v={active.refused} bad={active.refused > 0} />
              <Stat label="timeout" v={active.timedOut} bad={active.timedOut > 0} />
              <Stat label="RTT avg" v={`${active.rttAvgUs}µs`} />
              <Stat label="수신" v={`${(active.bytesRx / 1024).toFixed(1)}KB`} />
            </div>
          ) : (
            <div className="dim center">진행 중인 세션 시나리오 없음</div>
          )}
        </section>
      </div>
        </>
      )}
    </div>
  )
}

function Stat({
  label,
  v,
  good,
  bad,
}: {
  label: string
  v: number | string
  good?: boolean
  bad?: boolean
}) {
  return (
    <div className={`stat ${good ? 'good' : ''} ${bad ? 'bad' : ''}`}>
      <div className="statv">{v}</div>
      <div className="statl">{label}</div>
    </div>
  )
}

function ScenarioBar({
  instances,
  onChange,
}: {
  instances: InstanceView[]
  onChange: () => void
}) {
  const [target, setTarget] = useState('')
  const [frame, setFrame] = useState(64)
  const [rate, setRate] = useState(0)
  const [dur, setDur] = useState(10)
  const [busy, setBusy] = useState(false)
  const [msg, setMsg] = useState('')

  const names = instances.map((d) => d.name)
  const dstMac = 'ff:ff:ff:ff:ff:ff'

  const start = async () => {
    setBusy(true)
    setMsg('')
    try {
      const body = {
        scenarioId: `ui-${Date.now()}`,
        targets: target ? [target] : [],
        ratePps: rate,
        durationS: dur,
        packet: {
          eth: { dstMac },
          ipv4: { src: '10.0.0.1', dst: '10.0.0.2' },
          tcp: { srcPort: 1000, dstPort: 80 },
          frameSize: frame,
        },
      }
      const r = await api.startScenario(body)
      setMsg(r.ok ? '시작됨' : '실패: ' + JSON.stringify(r.results ?? r))
    } catch (e) {
      setMsg(String(e))
    } finally {
      setBusy(false)
      onChange()
    }
  }
  const stop = async () => {
    setBusy(true)
    try {
      await api.stopScenario({ scenarioId: 'ui', targets: target ? [target] : [] })
      setMsg('정지됨')
    } finally {
      setBusy(false)
      onChange()
    }
  }

  return (
    <div className="scenariobar">
      <span className="sblabel">Mode A 블라스트</span>
      <select value={target} onChange={(e) => setTarget(e.target.value)}>
        <option value="">전체 대상</option>
        {names.map((n) => (
          <option key={n} value={n}>{n}</option>
        ))}
      </select>
      <label>프레임<input type="number" value={frame} min={64} onChange={(e) => setFrame(+e.target.value)} />B</label>
      <label>rate<input type="number" value={rate} min={0} onChange={(e) => setRate(+e.target.value)} />pps(0=최대)</label>
      <label>기간<input type="number" value={dur} min={0} onChange={(e) => setDur(+e.target.value)} />s</label>
      <button className="btn go" disabled={busy} onClick={start}>시작</button>
      <button className="btn stop" disabled={busy} onClick={stop}>정지</button>
      {msg && <span className="sbmsg">{msg}</span>}
      <span className="sbwarn">⚠ 본인 소유·승인 링크에서만</span>
    </div>
  )
}
