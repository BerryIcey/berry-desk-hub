import { AppServerClient } from '../berry-hub-reference-codex-meter/src/app-server-client.mjs';
import { quotaSnapshot, memorySnapshot, taskSnapshot, percent, cloudSnapshot, freshness, keepFreshSection, keepFreshTasks } from './snapshot.mjs';
import { execFileSync } from 'node:child_process';
import { readFile, writeFile, rename } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const executable = process.env.BERRY_CODEX_EXECUTABLE;
if (!executable) throw new Error('Codex executable not configured');
let quota = quotaSnapshot();
let tasks = null;
const client = new AppServerClient({command: executable});
try {
  await client.start();
  const result = await client.request('account/rateLimits/read');
  quota = quotaSnapshot(result.rateLimits ?? {});
  try { tasks = taskSnapshot((await client.request('thread/list', {limit: 8, sortKey: 'updated_at'})).data); } catch { /* Task source unavailable. */ }
} catch { /* Unavailable is distinct from zero. */ }
finally { await client.stop(); }
let memory = memorySnapshot();
let cpu = null;
let cpuTemperature = null;
let gpu = null;
let gpuTemperature = null;
let boot = null;
try {
  const raw = execFileSync('powershell.exe', ['-NoProfile','-NonInteractive','-Command', '$o=Get-CimInstance Win32_OperatingSystem; $p=Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -Filter "Name=\'_Total\'" -ErrorAction SilentlyContinue; @{total=[double]$o.TotalVisibleMemorySize*1024;free=[double]$o.FreePhysicalMemory*1024;cpu=$p.PercentProcessorTime;boot=$o.LastBootUpTime.ToUniversalTime().ToString("o")}|ConvertTo-Json -Compress'], {encoding:'utf8',windowsHide:true,timeout:10000});
  const v = JSON.parse(raw);
  memory = memorySnapshot(v.total,v.free);
  cpu = percent(v.cpu);
  boot = typeof v.boot === 'string' && Number.isFinite(Date.parse(v.boot)) ? v.boot : null;
} catch { /* Leave unknown. */ }
try {
  const root=process.cwd();
  const raw=execFileSync(path.join(root,'.local','venv','Scripts','python.exe'),[path.join(root,'.local','berry-hub-live','cpu_temperature.py')],{encoding:'utf8',windowsHide:true,timeout:16000});
  const value=JSON.parse(raw).temperature_c;
  if(Number.isInteger(value) && value>=0 && value<=120) cpuTemperature=value;
} catch { /* A missing sensor is unknown. */ }
try {
  const root=process.cwd();
  const raw=execFileSync(path.join(root,'.local','venv','Scripts','python.exe'),[path.join(root,'.local','berry-hub-live','gpu_probe.py')],{encoding:'utf8',windowsHide:true,timeout:16000});
  const value=JSON.parse(raw).usage_percent;
  if(typeof value==='number' && Number.isFinite(value) && value>=0 && value<=100) gpu=value;
} catch { /* An unavailable GPU counter is not zero. */ }
try {
  const root=process.cwd();
  const raw=execFileSync(path.join(root,'.local','venv','Scripts','python.exe'),[path.join(root,'.local','berry-hub-live','gpu_temperature.py')],{encoding:'utf8',windowsHide:true,timeout:10000});
  const value=JSON.parse(raw).temperature_c;
  if(Number.isInteger(value) && value>=0 && value<=120) gpuTemperature=value;
} catch { /* A missing sensor is unknown. */ }
const now = new Date().toISOString();
let components = {process:'unknown',qq_connection:'unknown',model_api:'unknown',message_processing:'unknown'};
let modelName=null, lastReply=null, todayRounds=null, tokenLedger=null, lastChatObservation=null;
try {
  const root = process.cwd();
  const python = path.join(root,'.local','venv','Scripts','python.exe');
  const script = path.join(root,'.local','berry-hub-live','zhiqing_probe.py');
  const raw = execFileSync(python,[script],{encoding:'utf8',windowsHide:true,timeout:30000,env:{...process.env,PYTHONPATH:root}});
  const data = JSON.parse(raw);
  if(data.token_ledger && typeof data.token_ledger==='object') tokenLedger=data.token_ledger;
  if(data.last_chat_observation && typeof data.last_chat_observation==='object') lastChatObservation=data.last_chat_observation;
  for (const key of Object.keys(components)) if (['healthy','offline','unknown'].includes(data[key])) components[key]=data[key];
  if(typeof data.model_name==='string' && /^[A-Za-z0-9._-]{1,40}$/.test(data.model_name)) modelName=data.model_name;
  if(Number.isInteger(data.last_reply_seconds_ago) && data.last_reply_seconds_ago>=0 && data.last_reply_seconds_ago<=86400) lastReply=data.last_reply_seconds_ago;
  if(Number.isInteger(data.today_confirmed_rounds) && data.today_confirmed_rounds>0 && data.today_confirmed_rounds<=100000) todayRounds=data.today_confirmed_rounds;
} catch { /* A failed check does not imply a specific fault. */ }
let cloud=cloudSnapshot();
try {
  const root=process.cwd();
  const raw=execFileSync(path.join(root,'.local','venv','Scripts','python.exe'),[path.join(root,'.local','berry-hub-live','cloud_probe.py')],{encoding:'utf8',windowsHide:true,timeout:20000,env:{...process.env,PYTHONPATH:root}});
  cloud=cloudSnapshot(JSON.parse(raw));
} catch { /* Remote failure does not establish that the server is offline. */ }
let schedule={status:'unknown'};
try {
  const rows=JSON.parse(await readFile(path.join(process.cwd(),'.local','berry-hub-live','schedule.json'),'utf8'));
  if(Array.isArray(rows) && rows.every(r=>Array.isArray(r) && r.length===4 && Number.isInteger(r[0]) && r[0]>=1 && r[0]<=7 && r.slice(1).every(v=>typeof v==='string'))) schedule={status:'available',updated_at:now,source:'confirmed-repeating-preview',courses:rows};
} catch { /* A missing schedule is not an empty schedule. */ }
let weather={status:'unknown',location:'翔安'};
const weatherCache=path.join(process.cwd(),'.local','berry-hub-live','weather-moji-cache.json');
try { const cached=JSON.parse(await readFile(weatherCache,'utf8')); if(cached.schema_version===2 && freshness(cached.updated_at,Date.now(),30*60*1000)==='fresh')weather=cached; } catch { /* No usable cache. */ }
if(weather.status!=='available') {
  try {
    const root=process.cwd();
    const raw=execFileSync(path.join(root,'.local','venv','Scripts','python.exe'),[path.join(root,'.local','berry-hub-live','weather_probe.py')],{encoding:'utf8',windowsHide:true,timeout:15000});
    const candidate=JSON.parse(raw);
    if(candidate.status==='available') { weather=candidate; await writeFile(weatherCache,JSON.stringify(weather),{mode:0o600}); }
  } catch { /* Keep unknown on failure. */ }
}
const snapshot = {schema_version:'1.0',generated_at:now,timezone:'Asia/Shanghai',codex:{status:quota.some(q=>q.remaining_percent!==null)?'available':'unknown',updated_at:now,source:'codex-app-server',quota_windows:quota,recent_tasks:tasks,tasks_status:tasks===null?'unknown':'available'},dorm_pc:{status:'unknown',updated_at:now,source:'windows-cim',cpu:{usage_percent:cpu,temperature_c:cpuTemperature},gpu:{usage_percent:gpu,temperature_c:gpuTemperature},memory,boot_at:boot},zhiqing:{status:'unknown',updated_at:now,source:'local-read-only-probe',components,model_name:modelName,last_reply_seconds_ago:lastReply,today_confirmed_rounds:todayRounds,token_ledger:tokenLedger,last_chat_observation:lastChatObservation},aliyun:{...cloud,updated_at:now,source:'pinned-ssh'},daily:{status:weather.status,updated_at:weather.updated_at ?? now,source:'moji-xiangan',weather},schedule};
const output = path.join(path.dirname(fileURLToPath(import.meta.url)),'snapshot.json');
// A transient failed read must not erase a still-fresh successful observation.
// Its original source timestamp remains unchanged, so it expires normally.
try {
  const previous=JSON.parse(await readFile(output,'utf8'));
  snapshot.codex=keepFreshSection(snapshot.codex,previous.codex,snapshot.codex.status==='available',Date.now());
  snapshot.codex=keepFreshTasks(snapshot.codex,previous.codex,Date.now());
  snapshot.aliyun=keepFreshSection(snapshot.aliyun,previous.aliyun,snapshot.aliyun.status==='available',Date.now());
} catch { /* No usable prior sample. */ }
await writeFile(output+'.tmp',JSON.stringify(snapshot,null,2),{mode:0o600});
await rename(output+'.tmp',output);
console.log(JSON.stringify({codex:snapshot.codex.status,memory:memory.usage_percent===null?'unknown':'available',qq:components.qq_connection,cloud:cloud.status}));
