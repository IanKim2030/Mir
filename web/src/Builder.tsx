import { useCallback, useMemo, useState } from 'react'
import {
  ReactFlow,
  ReactFlowProvider,
  Background,
  Controls,
  Handle,
  Position,
  addEdge,
  useNodesState,
  useEdgesState,
  type Node,
  type Edge,
  type Connection,
  type NodeProps,
} from '@xyflow/react'
import '@xyflow/react/dist/style.css'
import { api, InstanceView } from './api'

// ── 레이어 정의 ────────────────────────────────────────────────
// 각 레이어의 편집 필드. type=num 이면 숫자 입력.
type LayerKind = 'eth' | 'ipv4' | 'ipv6' | 'tcp' | 'udp' | 'payload'

interface FieldDef { key: string; label: string; num?: boolean; ph?: string }

const LAYERS: Record<LayerKind, { title: string; color: string; l: string; fields: FieldDef[] }> = {
  eth: {
    title: 'Ethernet', color: '#4fd1c5', l: 'L2',
    fields: [
      { key: 'dstMac', label: 'dst MAC', ph: 'ff:ff:ff:ff:ff:ff' },
      { key: 'srcMac', label: 'src MAC', ph: '(포트 MAC)' },
      { key: 'vlanId', label: 'VLAN', num: true, ph: '0' },
    ],
  },
  ipv4: {
    title: 'IPv4', color: '#63b3ed', l: 'L3',
    fields: [
      { key: 'src', label: 'src', ph: '10.0.0.1' },
      { key: 'dst', label: 'dst', ph: '10.0.0.2' },
      { key: 'srcCount', label: 'src 순환', num: true, ph: '0' },
      { key: 'dstCount', label: 'dst 순환', num: true, ph: '0' },
      { key: 'ttl', label: 'TTL', num: true, ph: '64' },
    ],
  },
  ipv6: {
    title: 'IPv6', color: '#7f9cf5', l: 'L3',
    fields: [
      { key: 'src', label: 'src', ph: 'fd00::1' },
      { key: 'dst', label: 'dst', ph: 'fd00::2' },
    ],
  },
  tcp: {
    title: 'TCP', color: '#f6ad55', l: 'L4',
    fields: [
      { key: 'srcPort', label: 'src port', num: true, ph: '1000' },
      { key: 'dstPort', label: 'dst port', num: true, ph: '80' },
      { key: 'srcPortCount', label: 'src 순환', num: true, ph: '0' },
      { key: 'dstPortCount', label: 'dst 순환', num: true, ph: '0' },
      { key: 'flags', label: 'flags(비트합)', num: true, ph: '0=SYN' },
      { key: 'window', label: 'window', num: true, ph: '65535' },
    ],
  },
  udp: {
    title: 'UDP', color: '#fc8181', l: 'L4',
    fields: [
      { key: 'srcPort', label: 'src port', num: true, ph: '1000' },
      { key: 'dstPort', label: 'dst port', num: true, ph: '53' },
    ],
  },
  payload: {
    title: 'Payload', color: '#a0aec0', l: 'L7',
    fields: [{ key: 'text', label: '텍스트', ph: '남는 자리를 채움' }],
  },
}

interface LayerData extends Record<string, unknown> {
  kind: LayerKind
  fields: Record<string, string>
  onField: (id: string, key: string, val: string) => void
}
type LayerNode = Node<LayerData, 'layer'>

// ── 커스텀 노드 ────────────────────────────────────────────────
function LayerNodeView({ id, data }: NodeProps<LayerNode>) {
  const def = LAYERS[data.kind]
  return (
    <div className="lnode" style={{ borderColor: def.color }}>
      <Handle type="target" position={Position.Top} />
      <div className="lnode-h" style={{ color: def.color }}>
        <span className="lnode-l">{def.l}</span> {def.title}
      </div>
      <div className="lnode-b">
        {def.fields.map((f) => (
          <label key={f.key} className="lnode-f">
            <span>{f.label}</span>
            <input
              value={data.fields[f.key] ?? ''}
              placeholder={f.ph}
              inputMode={f.num ? 'numeric' : 'text'}
              onChange={(e) => data.onField(id, f.key, e.target.value)}
            />
          </label>
        ))}
      </div>
      <Handle type="source" position={Position.Bottom} />
    </div>
  )
}

// ── PacketSpec 조립 ────────────────────────────────────────────
function buildPacket(nodes: LayerNode[], frameSize: number) {
  const by = (k: LayerKind) => nodes.find((n) => n.data.kind === k)?.data.fields
  const numOr = (v: string | undefined, d?: number) =>
    v !== undefined && v !== '' ? Number(v) : d

  const eth = by('eth')
  const ipv4 = by('ipv4')
  const ipv6 = by('ipv6')
  const tcp = by('tcp')
  const udp = by('udp')
  const payload = by('payload')

  const errs: string[] = []
  if (!eth) errs.push('Ethernet 레이어가 필요하다')
  if (!eth?.dstMac) errs.push('eth.dstMac 이 필요하다')
  if (ipv4 && ipv6) errs.push('L3 는 IPv4/IPv6 중 하나만')
  if (!ipv4 && !ipv6) errs.push('L3(IPv4 또는 IPv6)가 필요하다')
  if (tcp && udp) errs.push('L4 는 TCP/UDP 중 하나만')
  if (!tcp && !udp) errs.push('L4(TCP 또는 UDP)가 필요하다')

  const packet: Record<string, unknown> = { frameSize }
  if (eth) {
    packet.eth = {
      dstMac: eth.dstMac,
      ...(eth.srcMac ? { srcMac: eth.srcMac } : {}),
      ...(numOr(eth.vlanId) ? { vlanId: numOr(eth.vlanId) } : {}),
    }
  }
  if (ipv4) {
    packet.ipv4 = {
      src: ipv4.src || '10.0.0.1', dst: ipv4.dst || '10.0.0.2',
      ...(numOr(ipv4.srcCount) ? { srcCount: numOr(ipv4.srcCount) } : {}),
      ...(numOr(ipv4.dstCount) ? { dstCount: numOr(ipv4.dstCount) } : {}),
      ...(numOr(ipv4.ttl) ? { ttl: numOr(ipv4.ttl) } : {}),
    }
  } else if (ipv6) {
    packet.ipv6 = { src: ipv6.src || 'fd00::1', dst: ipv6.dst || 'fd00::2' }
  }
  if (tcp) {
    packet.tcp = {
      srcPort: numOr(tcp.srcPort, 1000), dstPort: numOr(tcp.dstPort, 80),
      ...(numOr(tcp.srcPortCount) ? { srcPortCount: numOr(tcp.srcPortCount) } : {}),
      ...(numOr(tcp.dstPortCount) ? { dstPortCount: numOr(tcp.dstPortCount) } : {}),
      ...(numOr(tcp.flags) ? { flags: numOr(tcp.flags) } : {}),
      ...(numOr(tcp.window) ? { window: numOr(tcp.window) } : {}),
    }
  } else if (udp) {
    packet.udp = { srcPort: numOr(udp.srcPort, 1000), dstPort: numOr(udp.dstPort, 53) }
  }
  if (payload?.text) packet.payload = btoa(payload.text)

  return { packet, errs }
}

let idSeq = 100
const nid = () => `n${idSeq++}`

function makeNode(kind: LayerKind, x: number, y: number, onField: LayerData['onField']): LayerNode {
  return { id: nid(), type: 'layer', position: { x, y }, data: { kind, fields: {}, onField } }
}

// ── 빌더 ───────────────────────────────────────────────────────
function BuilderInner({ instances, onStarted }: { instances: InstanceView[]; onStarted: () => void }) {
  const onField = useCallback((id: string, key: string, val: string) => {
    setNodes((ns) =>
      ns.map((n) =>
        n.id === id ? { ...n, data: { ...n.data, fields: { ...n.data.fields, [key]: val } } } : n,
      ),
    )
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  const initial = useMemo<LayerNode[]>(
    () => [
      makeNode('eth', 40, 20, onField),
      makeNode('ipv4', 40, 200, onField),
      makeNode('tcp', 40, 430, onField),
    ],
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [],
  )
  const initialEdges: Edge[] = useMemo(
    () => [
      { id: 'e1', source: initial[0].id, target: initial[1].id },
      { id: 'e2', source: initial[1].id, target: initial[2].id },
    ],
    [initial],
  )

  const [nodes, setNodes, onNodesChange] = useNodesState<LayerNode>(initial)
  const [edges, setEdges, onEdgesChange] = useEdgesState<Edge>(initialEdges)
  const onConnect = useCallback((c: Connection) => setEdges((e) => addEdge(c, e)), [setEdges])

  const [target, setTarget] = useState('')
  const [rate, setRate] = useState(0)
  const [dur, setDur] = useState(10)
  const [frame, setFrame] = useState(64)
  const [msg, setMsg] = useState('')

  const nodeTypes = useMemo(() => ({ layer: LayerNodeView }), [])

  const addLayer = (kind: LayerKind) =>
    setNodes((ns) => [...ns, makeNode(kind, 320, 40 + ns.length * 30, onField)])

  const { packet, errs } = buildPacket(nodes, frame)
  const request = {
    scenarioId: `build-${Date.now()}`,
    targets: target ? [target] : [],
    ratePps: rate,
    durationS: dur,
    packet,
  }

  const start = async () => {
    if (errs.length) { setMsg('오류: ' + errs.join(' / ')); return }
    setMsg('전송 중…')
    try {
      const r = await api.startScenario(request)
      setMsg(r.ok ? '시작됨' : '실패: ' + JSON.stringify(r.results ?? r))
      onStarted()
    } catch (e) {
      setMsg(String(e))
    }
  }

  return (
    <div className="builder">
      <div className="canvas">
        <ReactFlow
          nodes={nodes}
          edges={edges}
          onNodesChange={onNodesChange}
          onEdgesChange={onEdgesChange}
          onConnect={onConnect}
          nodeTypes={nodeTypes}
          fitView
          proOptions={{ hideAttribution: true }}
        >
          <Background color="#232a36" gap={18} />
          <Controls />
        </ReactFlow>
        <div className="palette">
          <span>레이어 추가:</span>
          {(['ipv4', 'ipv6', 'tcp', 'udp', 'payload'] as LayerKind[]).map((k) => (
            <button key={k} onClick={() => addLayer(k)}>+ {LAYERS[k].title}</button>
          ))}
        </div>
      </div>

      <aside className="inspector">
        <h3>시나리오 (Mode A)</h3>
        <label className="ifield">대상
          <select value={target} onChange={(e) => setTarget(e.target.value)}>
            <option value="">전체</option>
            {instances.map((d) => <option key={d.name} value={d.name}>{d.name}</option>)}
          </select>
        </label>
        <label className="ifield">프레임(B)
          <input type="number" value={frame} min={64} onChange={(e) => setFrame(+e.target.value)} />
        </label>
        <label className="ifield">rate(pps, 0=최대)
          <input type="number" value={rate} min={0} onChange={(e) => setRate(+e.target.value)} />
        </label>
        <label className="ifield">기간(s, 0=무한)
          <input type="number" value={dur} min={0} onChange={(e) => setDur(+e.target.value)} />
        </label>

        <button className="btn go build-start" onClick={start}>빌드 & 시작</button>
        {msg && <div className="ins-msg">{msg}</div>}
        {errs.length > 0 && <div className="ins-err">{errs.map((e, i) => <div key={i}>· {e}</div>)}</div>}

        <h3>생성된 요청</h3>
        <pre className="json">{JSON.stringify(request, null, 2)}</pre>
        <div className="sbwarn">⚠ 본인 소유·승인 링크에서만 송신</div>
      </aside>
    </div>
  )
}

export function Builder(props: { instances: InstanceView[]; onStarted: () => void }) {
  return (
    <ReactFlowProvider>
      <BuilderInner {...props} />
    </ReactFlowProvider>
  )
}
