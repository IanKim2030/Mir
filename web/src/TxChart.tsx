import { useEffect, useRef } from 'react'
import uPlot from 'uplot'

// 롤링 시계열 라인 차트 (uPlot). 부모가 넘긴 (t, 값) 시리즈를 그린다.
// uPlot 은 명령형이라 ref 로 인스턴스를 잡고 setData 로 갱신한다.
export interface Series {
  label: string
  stroke: string
  values: number[]
}

export function TxChart({
  xs,
  series,
  title,
  unit,
}: {
  xs: number[]
  series: Series[]
  title: string
  unit: string
}) {
  const elRef = useRef<HTMLDivElement>(null)
  const uRef = useRef<uPlot | null>(null)

  // 인스턴스 생성 (한 번). 시리즈 구성이 바뀌면 재생성.
  useEffect(() => {
    if (!elRef.current) return
    const opts: uPlot.Options = {
      title,
      width: elRef.current.clientWidth || 600,
      height: 200,
      cursor: { drag: { x: true, y: false } },
      scales: { x: { time: true } },
      axes: [
        { stroke: '#8a93a6', grid: { stroke: '#20242e' }, ticks: { stroke: '#20242e' } },
        {
          stroke: '#8a93a6',
          grid: { stroke: '#20242e' },
          ticks: { stroke: '#20242e' },
          size: 60,
          values: (_u, ticks) => ticks.map((v) => `${v} ${unit}`),
        },
      ],
      series: [
        {},
        ...series.map((s) => ({
          label: s.label,
          stroke: s.stroke,
          width: 2,
          points: { show: false },
        })),
      ],
      legend: { show: true },
    }
    const u = new uPlot(opts, [xs, ...series.map((s) => s.values)], elRef.current)
    uRef.current = u

    const onResize = () => u.setSize({ width: elRef.current!.clientWidth, height: 200 })
    window.addEventListener('resize', onResize)
    return () => {
      window.removeEventListener('resize', onResize)
      u.destroy()
      uRef.current = null
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [series.length, title, unit])

  // 데이터 갱신.
  useEffect(() => {
    uRef.current?.setData([xs, ...series.map((s) => s.values)])
  }, [xs, series])

  return <div className="chart" ref={elRef} />
}
