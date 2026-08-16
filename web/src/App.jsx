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

function shortId(id) {
  // id is a MAC address like "AA:BB:CC:DD:EE:FF" - show the last two octets
  // so props with duplicate PROP_NAMEs are still distinguishable.
  const parts = id.split(':')
  return parts.slice(-2).join(':')
}

function PropsPanel({ nodes, onOpenLogs }) {
  const [triggerStatus, setTriggerStatus] = useState({})
  const [renamingId, setRenamingId] = useState(null)
  const [renameValue, setRenameValue] = useState('')
  const [renameStatus, setRenameStatus] = useState({})
  const [renamingTrigger, setRenamingTrigger] = useState(null) // { nodeId, triggerId } | null
  const [triggerRenameValue, setTriggerRenameValue] = useState('')
  const [triggerRenameStatus, setTriggerRenameStatus] = useState({})

  // id is the canonical trigger name sent in the request body; label is
  // what's shown on the button (possibly renamed - see triggerEventButtons).
  async function trigger(node, id) {
    const key = `${node.id}:${id}`
    setTriggerStatus((prev) => ({ ...prev, [key]: 'sending' }))
    try {
      const res = await fetch(`http://${node.ip}/trigger`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ effect: id }),
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

  // Renders one trigger button. TriggerEvents and EventConfigs both fire the
  // same POST /trigger {"effect": id} request (see PropCore.h) - the only
  // difference here is which group they're shown in and how they're styled,
  // so operators can tell "do something" apart from "change how it behaves"
  // at a glance. label is what's displayed; id is always what's sent.
  function triggerButton(node, id, label, className) {
    const status = triggerStatus[`${node.id}:${id}`]
    return (
      <button
        key={id}
        type="button"
        className={className}
        disabled={!node.online || status === 'sending'}
        onClick={() => trigger(node, id)}
      >
        {status === 'sending' ? 'Sending…' : status === 'sent' ? 'Triggered ✓' : status === 'failed' ? 'Failed ✗' : label}
      </button>
    )
  }

  function startTriggerRename(node, triggerEvent) {
    setRenamingTrigger({ nodeId: node.id, triggerId: triggerEvent.id })
    setTriggerRenameValue(triggerEvent.label)
  }

  // POSTs directly to the prop (same direct-to-prop pattern as
  // submitRename() below) so the new label is persisted on-device and
  // survives reboots/reflashes - see PropCore.h's
  // POST /trigger-events/rename. A blank label reverts to the canonical id.
  async function submitTriggerRename(node, triggerEvent) {
    const newLabel = triggerRenameValue.trim()
    setRenamingTrigger(null)
    if (newLabel === triggerEvent.label) return

    const key = `${node.id}:${triggerEvent.id}`
    setTriggerRenameStatus((prev) => ({ ...prev, [key]: 'saving' }))
    try {
      const res = await fetch(`http://${node.ip}/trigger-events/rename`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ id: triggerEvent.id, label: newLabel }),
      })
      if (!res.ok) throw new Error(`HTTP ${res.status}`)
      setTriggerRenameStatus((prev) => ({ ...prev, [key]: 'saved' }))
    } catch {
      setTriggerRenameStatus((prev) => ({ ...prev, [key]: 'failed' }))
    } finally {
      setTimeout(() => {
        setTriggerRenameStatus((prev) => ({ ...prev, [key]: undefined }))
      }, TRIGGER_STATUS_RESET_MS)
    }
  }

  // TriggerEvents get a small rename pencil next to the trigger button
  // itself (clicking the button fires it, so renaming needs its own
  // affordance) - EventConfigs aren't renameable, so they skip all of this
  // and just render a plain triggerButton in the caller below.
  function triggerEventButtons(node) {
    return node.triggerEvents.map((triggerEvent) => {
      const isRenaming =
        renamingTrigger?.nodeId === node.id && renamingTrigger?.triggerId === triggerEvent.id
      if (isRenaming) {
        return (
          <input
            key={triggerEvent.id}
            className="prop-name-input trigger-rename-input"
            value={triggerRenameValue}
            autoFocus
            onChange={(e) => setTriggerRenameValue(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === 'Enter') submitTriggerRename(node, triggerEvent)
              if (e.key === 'Escape') setRenamingTrigger(null)
            }}
            onBlur={() => submitTriggerRename(node, triggerEvent)}
          />
        )
      }

      const renameState = triggerRenameStatus[`${node.id}:${triggerEvent.id}`]
      return (
        <span className="trigger-event-wrap" key={triggerEvent.id}>
          {triggerButton(node, triggerEvent.id, triggerEvent.label, 'trigger-event-btn')}
          <button
            type="button"
            className="trigger-rename-btn"
            title={`Rename "${triggerEvent.label}"`}
            disabled={!node.online}
            onClick={() => startTriggerRename(node, triggerEvent)}
          >
            {renameState === 'saving' ? '…' : renameState === 'failed' ? '✗' : '✎'}
          </button>
        </span>
      )
    })
  }

  function startRename(node) {
    setRenamingId(node.id)
    setRenameValue(node.name)
  }

  async function submitRename(node) {
    const newName = renameValue.trim()
    setRenamingId(null)
    if (!newName || newName === node.name) return

    setRenameStatus((prev) => ({ ...prev, [node.id]: 'saving' }))
    try {
      const res = await fetch(`http://${node.ip}/rename`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name: newName }),
      })
      if (!res.ok) throw new Error(`HTTP ${res.status}`)
      setRenameStatus((prev) => ({ ...prev, [node.id]: 'saved' }))
    } catch {
      setRenameStatus((prev) => ({ ...prev, [node.id]: 'failed' }))
    } finally {
      setTimeout(() => {
        setRenameStatus((prev) => ({ ...prev, [node.id]: undefined }))
      }, TRIGGER_STATUS_RESET_MS)
    }
  }

  return (
    <div className="card props-panel">
      {nodes.length === 0 && <p className="subtitle">No prop nodes registered yet.</p>}
      {nodes.map((node) => (
        <div className="prop-row" key={node.id}>
          <span className={`status-dot ${node.online ? 'ok' : 'err'}`} />
          {renamingId === node.id ? (
            <input
              className="prop-name-input"
              value={renameValue}
              autoFocus
              onChange={(e) => setRenameValue(e.target.value)}
              onKeyDown={(e) => {
                if (e.key === 'Enter') submitRename(node)
                if (e.key === 'Escape') setRenamingId(null)
              }}
              onBlur={() => submitRename(node)}
            />
          ) : (
            <button
              type="button"
              className="prop-name prop-name-button"
              disabled={!node.online}
              onClick={() => startRename(node)}
              title="Click to rename"
            >
              {node.name}
              {renameStatus[node.id] === 'saving' && ' …'}
              {renameStatus[node.id] === 'failed' && ' (rename failed)'}
            </button>
          )}
          <span className="prop-id">#{shortId(node.id)}</span>
          <span className="prop-ip">{node.ip}</span>
          <button type="button" className="nav-link prop-logs-link" onClick={() => onOpenLogs(node)}>
            Logs
          </button>
          <span className="prop-actions">{triggerEventButtons(node)}</span>
          {node.eventConfigs.length > 0 && (
            <span className="prop-actions prop-actions-config">
              {node.eventConfigs.map((name) => triggerButton(node, name, name, 'event-config-btn'))}
            </span>
          )}
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
