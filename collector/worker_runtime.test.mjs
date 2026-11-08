import {test} from 'node:test';
import assert from 'node:assert/strict';
import {mkdtemp,readFile,rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import path from 'node:path';
import {createRuntime,replaceState} from './worker_runtime.mjs';

test('runtime invokes collection before transfer and persists only operational state',async()=>{
 const dir=await mkdtemp(path.join(tmpdir(),'berry-worker-'));
 try {
  const calls=[];const w=await createRuntime({root:dir,now:()=>1000,run:async(file,args)=>{calls.push(args.at(-1));return true}});
  await w.step();assert.equal(calls.length,4);assert.match(calls[0],/collect\.mjs$/);assert.match(calls[1],/gpu_fast\.py$/);assert.match(calls[2],/task_fast\.py$/);assert.match(calls[3],/serial_bridge\.py$/);
  const state=JSON.parse(await readFile(path.join(dir,'.local','berry-hub-live','worker-state.json'),'utf8'));
  assert.equal(state.values.collect.value,true);assert.equal(state.values.transfer.value,true);
 } finally {await rm(dir,{recursive:true,force:true});}
});

test('temporary Windows rename conflicts retry without losing the state',async()=>{
 let writes=0,renames=0;
 await replaceState('state.json',{a:1},{write:async()=>{writes++},rename:async()=>{renames++;if(renames<3){const e=new Error('busy');e.code='EPERM';throw e}},wait:async()=>{}});
 assert.equal(writes,1);assert.equal(renames,3);
});
test('persistent or unexpected rename errors are not treated as success',async()=>{
 let calls=0;await assert.rejects(replaceState('state.json',{}, {write:async()=>{},rename:async()=>{calls++;const e=new Error('bad');e.code='EACCES';throw e},wait:async()=>{}}));assert.equal(calls,1);
});
