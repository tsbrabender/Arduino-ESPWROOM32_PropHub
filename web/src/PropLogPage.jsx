import { useEffect, useRef, useState } from 'react'

const LOG_POLL_INTERVAL_MS = 2000

// Log timestamps are the hub's millis() at receipt - the hub has no RTC, so
// this is uptime, not wall-clock time.
function formatHubTime(ms) {
  const totalSeconds = Math.floor(ms / 1000)
  const hours = Math.floor(totalSeconds / 3600)
  const minutes = Math.floor((totalSeconds % 3600) / 60)
  const seconds = totalSeconds % 60
  const pad = (n) => String(n).padStart(2, '0')
  return `${pad(hours)}:${pad(minutes)}:${pad(seconds)}`
}

function useNodeLogs(nodeId, intervalMs) {
  const [logs, setLogs] = useState([])
  const timerRef = useRef(null)

  useEffect(() => {
    let cancelled = false

    async function poll() {
      try {
        const res = await fetch(`/api/nodes/logs?id=${encodeURIComponent(nodeId)}`)
        if (!res.ok) throw new Error(`HTTP ${res.status}`)
        const data = await res.json()
        if (!cancelled) setLogs(data.logs ?? [])
      } catch {
        // Hub or node unreachable; the status banner up top already surfaces this.
      } finally {
        if (!cancelled) timerRef.current = setTimeout(poll, intervalMs)
      }
    }

    poll()
    return () => {
      cancelled = true
      clearTimeout(timerRef.current)
    }
  }, [nodeId, intervalMs])

  return logs
}

export default function PropLogPage({ nodeId, nodeName, onBack }) {
  const logs = useNodeLogs(nodeId, LOG_POLL_INTERVAL_MS)

  return (
    <>
      <button type="button" className="nav-link" onClick={onBack}>← Back to Props</button>
      <h1>{nodeName}</h1>
      <p className="subtitle">
        Last {logs.length} status message{logs.length === 1 ? '' : 's'} (times are hub uptime, not wall-clock).
      </p>

      <div className="card log-panel">
        {logs.length === 0 && <p className="subtitle">No status messages yet.</p>}
        {logs.map((entry, i) => (
          <div className="log-line" key={i}>
            <span className="log-ts">{formatHubTime(entry.ts)}</span>
            <span className="log-message">{entry.message}</span>
          </div>
        ))}
      </div>
    </>
  )
}
