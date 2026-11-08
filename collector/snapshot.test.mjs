import { test } from 'node:test';
import assert from 'node:assert/strict';
import { quotaSnapshot, memorySnapshot, taskSnapshot, percent, cloudSnapshot, freshness, keepFreshSection, keepFreshTasks } from './snapshot.mjs';

test('failed reads retain the last successful sample only until its original expiry',()=>{
  const now=Date.parse('2026-10-29T12:06:00Z');
  const last={source:'codex-app-server',status:'available',updated_at:'2026-10-29T12:00:00Z',quota_windows:[{remaining_percent:60}]};
  const unavailable={source:'codex-app-server',status:'unknown',updated_at:'2026-10-29T12:06:00Z'};
  assert.equal(keepFreshSection(unavailable,last,false,now),last);
  assert.equal(keepFreshSection(unavailable,last,false,now+300000),unavailable);
  assert.equal(keepFreshSection(unavailable,last,true,now),unavailable);
});

test('quota reports remaining and rejects missing or invalid fields', () => {
  assert.deepEqual(quotaSnapshot({primary:{usedPercent:30,windowDurationMins:300,resetsAt:1000},secondary:{usedPercent:69,windowDurationMins:10080,resetsAt:2000}}), [{label:'5 小时额度',remaining_percent:70,resets_at:'1970-01-01T00:16:40.000Z'},{label:'周额度',remaining_percent:31,resets_at:'1970-01-01T00:33:20.000Z'}]);
  assert.equal(quotaSnapshot({primary:{usedPercent:null},secondary:{usedPercent:101}})[0].remaining_percent,null);
  assert.equal(quotaSnapshot({primary:{usedPercent:null},secondary:{usedPercent:101}})[1].remaining_percent,null);
});
test('only explicit titles and reliable task states are exported',()=>{
  assert.deepEqual(taskSnapshot([{name:'任务 A',preview:'secret',status:{type:'active',activeFlags:['waitingOnApproval']}},{name:'任务 B',status:{type:'idle'}},{preview:'secret'}]), [{title:'任务 A',status:'waiting_approval',status_text:'等待批准'},{title:'任务 B',status:'unknown',status_text:'状态未知'}]);
});
test('sensitive-looking task titles are hidden before local snapshot storage',()=>{
  assert.equal(taskSnapshot([{name:'查看 API key',status:{type:'idle'}}])[0].title,'标题已隐藏');
});
test('invalid percentages remain unknown',()=>{
  assert.equal(percent(101),null);
  assert.equal(percent(null),null);
  assert.equal(percent(32),32);
});
test('missing, future, or expired timestamps cannot be live',()=>{
  const now=Date.parse('2026-01-01T00:05:00Z');
  assert.equal(freshness('2026-01-01T00:04:00Z',now,120000),'fresh');
  assert.equal(freshness('2026-01-01T00:02:00Z',now,120000),'stale');
  assert.equal(freshness('2026-01-02T00:00:00Z',now,120000),'unknown');
  assert.equal(freshness(null,now,120000),'unknown');
});
test('cloud values are validated and unknown measurements stay unknown',()=>{
  const s=cloudSnapshot({status:'available',cpu_percent:12,memory_total:100,memory_available:40,disk_total:200,disk_used:50,uptime_seconds:10,download_bytes_per_second:123,upload_bytes_per_second:45});
  assert.equal(s.cpu.usage_percent,12); assert.equal(s.memory.usage_percent,60); assert.equal(s.storage.usage_percent,25); assert.equal(s.uptime_seconds,10);
  assert.equal(s.download_bytes_per_second,123); assert.equal(s.upload_bytes_per_second,45);
  const bad=cloudSnapshot({status:'available',cpu_percent:120,memory_total:100,memory_available:200,disk_total:0,disk_used:-1,uptime_seconds:-1});
  assert.equal(bad.cpu.usage_percent,null); assert.equal(bad.memory.usage_percent,null); assert.equal(bad.storage.usage_percent,null); assert.equal(bad.uptime_seconds,null);
});
test('memory uses real sizes and rejects missing or impossible values',()=>{
  assert.deepEqual(memorySnapshot(100,25),{usage_percent:75,used_bytes:75,total_bytes:100});
  assert.deepEqual(memorySnapshot(100,101),{usage_percent:null,used_bytes:null,total_bytes:null});
});

test('desktop task states survive full collection only while their own timestamp is fresh',()=>{
 const now=Date.parse('2026-10-30T01:02:00Z');
 const candidate={updated_at:'2026-10-30T01:02:00Z',recent_tasks:[{status:'unknown'}]};
 const previous={tasks_updated_at:'2026-10-30T01:01:30Z',tasks_status:'available',recent_tasks:[{status:'running'}]};
 assert.equal(keepFreshTasks(candidate,previous,now).recent_tasks[0].status,'running');
 assert.equal(keepFreshTasks(candidate,previous,now+180000).recent_tasks[0].status,'unknown');
});
