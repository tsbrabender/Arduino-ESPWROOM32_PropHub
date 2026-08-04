import { useEffect, useRef, useState } from 'react'
import NetworkPage from './NetworkPage.jsx'
import PropLogPage from './PropLogPage.jsx'

const POLL_INTERVAL_MS = 2000
const NODES_POLL_INTERVAL_MS = 3000
const TRIGGER_STATUS_RESET_MS = 2000

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

function PropsPanel({ nodes, onOpenLogs }) {
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
    <div className="card props-panel">
      {nodes.length === 0 && <p className="subtitle">No prop nodes registered yet.</p>}
      {nodes.map((node) => (
        <div className="prop-row" key={node.id}>
          <span className={`status-dot ${node.online ? 'ok' : 'err'}`} />
          <span className="prop-name">{node.name}</span>
          <span className="prop-ip">{node.ip}</span>
          <button type="button" className="nav-link prop-logs-link" onClick={() => onOpenLogs(node)}>
            Logs
          </button>
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
  const tasks = status?.tasks ?? []
  const [view, setView] = useState({ page: 'props' })

  return (
    <>
      <h1>PropHub</h1>
      <p className="subtitle">
        <span className={`status-dot ${error ? 'err' : 'ok'}`} />
        {error ? `Disconnected (${error})` : status ? 'Connected' : 'Loading…'}
      </p>

      {error && (
        <div className="error-banner">
          Unable to reach the device: {error}. Retrying every {POLL_INTERVAL_MS / 1000}s.
        </div>
      )}

      {view.page === 'network' && (
        <NetworkPage status={status} tasks={tasks} onBack={() => setView({ page: 'props' })} />
      )}
      {view.page === 'logs' && (
        <PropLogPage
          nodeId={view.nodeId}
          nodeName={view.nodeName}
          onBack={() => setView({ page: 'props' })}
        />
      )}
      {view.page === 'props' && (
        <>
          <h2>Props</h2>
          <PropsPanel nodes={nodes} onOpenLogs={(node) => setView({ page: 'logs', nodeId: node.id, nodeName: node.name })} />
          <button type="button" className="nav-link" onClick={() => setView({ page: 'network' })}>
            Network &amp; device info →
          </button>
        </>
      )}
    </>
  )
}
