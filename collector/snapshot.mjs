export function quotaSnapshot(limits = {}) {
  const windows = [['5 小时额度',300],['周额度',10080]].map(([label,duration]) => {
    const window = [limits.primary,limits.secondary].find(w => w?.windowDurationMins === duration &&
      Number.isFinite(w.usedPercent) && w.usedPercent >= 0 && w.usedPercent <= 100);
    const reset = window && Number.isFinite(window.resetsAt) && window.resetsAt > 0 ? new Date(window.resetsAt * 1000) : null;
    return {label, remaining_percent:window ? 100-window.usedPercent : null, resets_at:reset && Number.isFinite(reset.getTime()) ? reset.toISOString() : null};
  });
  if (windows[0].remaining_percent === null && windows[1].remaining_percent !== null &&
      ![limits.primary,limits.secondary].some(w => w?.windowDurationMins === 300)) {
    windows[0].remaining_percent = 100;
  }
  return windows;
}
export function memorySnapshot(total,free) {
  if (![total,free].every(Number.isFinite) || total <= 0 || free < 0 || free > total) return {usage_percent:null,used_bytes:null,total_bytes:null};
  return {usage_percent:Math.round((total-free)/total*100),used_bytes:total-free,total_bytes:total};
}
export function percent(value) {
  return typeof value === 'number' && Number.isFinite(value) && value >= 0 && value <= 100 ? Math.round(value) : null;
}
export function taskSnapshot(threads) {
  if (!Array.isArray(threads)) return [];
  return threads.filter(t => typeof t?.name === 'string' && t.name.trim()).slice(0,4).map(t => {
    const flags = t.status?.activeFlags ?? [];
    const state = t.status?.type === 'active' ? (flags.includes('waitingOnApproval') ? 'waiting_approval' : flags.includes('waitingOnUserInput') ? 'blocked' : 'running') : t.status?.type === 'systemError' ? 'error' : 'unknown';
    const title = /api[ _-]*key|password|secret|token|credential|密码|密钥|口令|凭据|[A-Za-z0-9_-]{32,}/i.test(t.name) ? '标题已隐藏' : Array.from(t.name.trim()).slice(0,100).join('');
    return {title, status:state, status_text:{waiting_approval:'等待批准',blocked:'等待输入',running:'运行中',error:'遇到问题',unknown:'状态未知'}[state]};
  });
}
export function cloudSnapshot(data = {}) {
  const memory = memorySnapshot(data.memory_total, data.memory_available);
  const storage = memorySnapshot(data.disk_total, typeof data.disk_total === 'number' && typeof data.disk_used === 'number' ? data.disk_total-data.disk_used : null);
  const rate = n => Number.isSafeInteger(n) && n >= 0 ? n : null;
  return {status:data.status === 'available' ? 'available' : 'unknown',relay_status:data.relay_status==='healthy'?'healthy':'unknown',cpu:{usage_percent:percent(data.cpu_percent)},memory,storage,uptime_seconds:Number.isInteger(data.uptime_seconds) && data.uptime_seconds >= 0 ? data.uptime_seconds : null,latency_ms:rate(data.latency_ms),download_bytes_per_second:rate(data.download_bytes_per_second),upload_bytes_per_second:rate(data.upload_bytes_per_second)};
}
export function freshness(timestamp, now, maxAge) {
  if (typeof timestamp !== 'string' || !Number.isFinite(now) || !Number.isFinite(maxAge) || maxAge <= 0) return 'unknown';
  const time = Date.parse(timestamp);
  if (!Number.isFinite(time) || time > now) return 'unknown';
  return now-time <= maxAge ? 'fresh' : 'stale';
}
export function keepFreshSection(candidate,previous,successful,now,maxAge=600000) {
  return !successful && previous?.status==='available' && previous.source===candidate.source && freshness(previous.updated_at,now,maxAge)==='fresh' ? previous : candidate;
}

export function keepFreshTasks(candidate,previous,now) {
  if(previous?.tasks_status==='available' && Array.isArray(previous.recent_tasks) && freshness(previous.tasks_updated_at,now,120000)==='fresh')
    return {...candidate,recent_tasks:previous.recent_tasks,tasks_status:'available',tasks_updated_at:previous.tasks_updated_at};
  return candidate;
}
