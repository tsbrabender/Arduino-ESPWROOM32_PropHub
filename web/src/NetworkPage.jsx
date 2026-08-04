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

export default function NetworkPage({ status, tasks, onBack }) {
  const device = status?.device
  const memory = status?.memory
  const network = status?.network

  return (
    <>
      <button type="button" className="nav-link" onClick={onBack}>← Back to Props</button>
      <h1>Network &amp; Device</h1>
      <p className="subtitle">Hub hardware, memory, network, and FreeRTOS task info.</p>

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
    </>
  )
}
