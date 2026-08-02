import { useEffect, useRef, useState } from 'react'

const POLL_INTERVAL_MS = 2000
const NODES_POLL_INTERVAL_MS = 3000
const TRIGGER_STATUS_RESET_MS = 2000

function formatBytes(bytes) {
  if (bytes === undefined || bytes === null) return '—'
  if (bytes < 1024) return `${bytes} B`
  const kb = bytes / 1024
  if (kb < 1024) return `${kb.toFixed(1)} KB`
  return `${(kb / 1024).toFixed(2)} MB`
}

function formatUptime(ms) {
  if (ms === undefined || ms === null) return '—'
  let s = Math.floor(ms / 1000)
  const days = Math.floor(s / 86400)
  s -= days * 86400
  const hours = Math.floor(s / 3600)
  s -= hours * 3600
  const minutes = Math.floor(s / 60)
  s -= minutes * 60
  const parts = []
  if (days) parts.push(`${days}d`)
  if (days || hours) parts.push(`${hours}h`)
  if (days || hours || minutes) parts.push(`${minutes}m`)
  parts.push(`${s}s`)
  return parts.join(' ')
}

function taskStateClass(state) {
  switch (state) {
    case 'Running':
    case 'Ready':
      return 'state-running'
    case 'Blocked':
      return 'state-blocked'
    case 'Suspended':
    case 'Deleted':
      return 'state-suspended'
    default:
      return ''
  }
}

function useStatus(intervalMs) {
  const [status, setStatus] = useState(null)
  const [error, setError] = useState(null)
  const timerRef = useRef(null)

  useEffect(() => {
    let cancelled = false

    async function poll() {
      try {
        const res = await fetch('/api/status')
        if (!res.ok) throw new Error(`HTTP ${res.status}`)
        const data = await res.json()
        if (!cancelled) {
          setStatus(data)
          setError(null)
        }
      } catch (err) {
        if (!cancelled) setError(err.message)
      } finally {
        if (!cancelled) timerRef.current = setTimeout(poll, intervalMs)
      }
    }

    poll()
    return () => {
      cancelled = true
      clearTimeout(timerRef.current)
    }
  }, [intervalMs])

  return { status, error }
}

function useNodes(intervalMs) {
  const [nodes, setNodes] = useState([])
  const timerRef = useRef(null)

  useEffect(() => {
    let cancelled = false

    async function poll() {
      try {
        const res = await fetch('/api/nodes')
        if (!res.ok) throw new Error(`HTTP ${res.status}`)
        const data = await res.json()
        if (!cancelled) setNodes(data.nodes ?? [])
      } catch {
        // Hub is unreachable; the status banner up top already surfaces this.
      } finally {
        if (!cancelled) timerRef.current = setTimeout(poll, intervalMs)
      }
    }

    poll()
    return () => {
      cancelled = true
      clearTimeout(timerRef.current)
    }
  }, [intervalMs])

  return nodes
}

function PropsPanel({ nodes }) {
  const [triggerStatus, setTriggerStatus] = useState({})

  async function trigger(node, effect) {
    const key = `${node.id}:${effect}`
    setTriggerStatus((prev) => ({ ...prev, [key]: 'sending' }))
    try {
      const res = await fetch(`http://${node.ip}/trigger`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ effect }),
      })
      if (!res.ok) throw new Error(`HTTP ${res.status}`)
      setTriggerStatus((prev) => ({ ...prev, [key]: 'sent' }))
    } catch {
      setTriggerStatus((prev) => ({ ...prev, [key]: 'failed' }))
    } finally {
      setTimeout(() => {
        setTriggerStatus((prev) => ({ ...prev, [key]: undefined }))
      }, TRIGGER_STATUS_RESET_MS)
    }
  }

  return (
    <div className="card">
      <h2>Props</h2>
      {nodes.length === 0 && <p className="subtitle">No prop nodes registered yet.</p>}
      {nodes.map((node) => (
        <div className="prop-row" key={node.id}>
          <span className={`status-dot ${node.online ? 'ok' : 'err'}`} />
          <span className="prop-name">{node.name}</span>
          <span className="prop-ip">{node.ip}</span>
          <span className="prop-actions">
            {node.effects.map((effect) => {
              const status = triggerStatus[`${node.id}:${effect}`]
              return (
                <button
                  key={effect}
                  type="button"
                  disabled={!node.online || status === 'sending'}
                  onClick={() => trigger(node, effect)}
                >
                  {status === 'sending' ? 'Sending…' : status === 'sent' ? 'Triggered ✓' : status === 'failed' ? 'Failed ✗' : effect}
                </button>
              )
            })}
          </span>
        </div>
      ))}
    </div>
  )
}

export default function App() {
  const { status, error } = useStatus(POLL_INTERVAL_MS)
  const nodes = useNodes(NODES_POLL_INTERVAL_MS)
  const device = status?.device
  const memory = status?.memory
  const network = status?.network
  const tasks = status?.tasks ?? []

  return (
    <>
      <h1>SoundBox</h1>
      <p className="subtitle">
        <span className={`status-dot ${error ? 'err' : 'ok'}`} />
        {error ? `Disconnected (${error})` : status ? 'Connected' : 'Loading…'}
      </p>

      {error && (
        <div className="error-banner">
          Unable to reach the device: {error}. Retrying every {POLL_INTERVAL_MS / 1000}s.
        </div>
      )}

      <div className="grid">
        <div className="card">
          <h2>Device</h2>
          <ul className="stat-list">
            <li><span>Chip</span><span>{device?.chipModel ?? '—'} rev {device?.chipRevision ?? '—'}</span></li>
            <li><span>Cores</span><span>{device?.cores ?? '—'}</span></li>
            <li><span>CPU Freq</span><span>{device?.cpuFreqMHz ? `${device.cpuFreqMHz} MHz` : '—'}</span></li>
            <li><span>Flash</span><span>{formatBytes(device?.flashSizeBytes)}</span></li>
            <li><span>SDK</span><span>{device?.sdkVersion ?? '—'}</span></li>
            <li><span>MAC</span><span>{device?.macAddress ?? '—'}</span></li>
            <li><span>Uptime</span><span>{formatUptime(status?.uptimeMs)}</span></li>
          </ul>
        </div>

        <div className="card">
          <h2>Memory</h2>
          <ul className="stat-list">
            <li><span>Free Heap</span><span>{formatBytes(memory?.heapFreeBytes)}</span></li>
            <li><span>Total Heap</span><span>{formatBytes(memory?.heapTotalBytes)}</span></li>
            <li><span>Min Free Heap</span><span>{formatBytes(memory?.heapMinFreeBytes)}</span></li>
            {memory?.psramTotalBytes > 0 && (
              <li><span>Free PSRAM</span><span>{formatBytes(memory?.psramFreeBytes)}</span></li>
            )}
          </ul>
        </div>

        <div className="card">
          <h2>Network</h2>
          <ul className="stat-list">
            <li><span>Mode</span><span>Access Point</span></li>
            <li><span>SSID</span><span>{network?.ssid ?? '—'}</span></li>
            <li><span>IP Address</span><span>{network?.ipAddress ?? '—'}</span></li>
            <li><span>Connected Clients</span><span>{network?.connectedClients ?? '—'}</span></li>
          </ul>
        </div>
      </div>

      <PropsPanel nodes={nodes} />

      <div className="card">
        <h2>Running Processes</h2>
        <table>
          <thead>
            <tr>
              <th>Name</th>
              <th>State</th>
              <th>Priority</th>
              <th>Core</th>
              <th>Stack Free</th>
            </tr>
          </thead>
          <tbody>
            {tasks.length === 0 && (
              <tr>
                <td colSpan={5}>{status ? 'No task data' : 'Loading…'}</td>
              </tr>
            )}
            {tasks.map((task) => (
              <tr key={task.name}>
                <td>{task.name}</td>
                <td className={taskStateClass(task.state)}>{task.state}</td>
                <td>{task.priority}</td>
                <td>{task.coreId === 2147483647 || task.coreId === -1 ? 'ANY' : task.coreId}</td>
                <td>{formatBytes(task.stackHighWaterMark)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>

      <p className="footer-note">Polling /api/status every {POLL_INTERVAL_MS / 1000}s</p>
    </>
  )
}
