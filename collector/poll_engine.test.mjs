import { test } from 'node:test';
import assert from 'node:assert/strict';
import { PollEngine } from './poll_engine.mjs';

test('restart respects a persisted retry deadline and preserves source timestamps',async()=>{
 let now=1000, saved={}, calls=0;
 const opts={intervals:{codex:300000},sources:{codex:async()=>{calls++;return {status:'available'}}},readState:async()=>saved,writeState:async s=>{saved=structuredClone(s)},now:()=>now};
 const first=new PollEngine(opts);await first.init();assert.equal(await first.poll('codex'),true);
 const stamp=saved.values.codex.updated_at;
 const second=new PollEngine(opts);await second.init();now=2000;
 assert.equal(await second.poll('codex'),false);assert.equal(calls,1);assert.equal(second.values.codex.updated_at,stamp);
 now=301000;assert.equal(await second.poll('codex'),true);assert.equal(calls,2);
});

test('failure retains old timestamp and uses bounded backoff',async()=>{
 let now=0,saved={},fail=false;
 const opts={intervals:{cloud:100},sources:{cloud:async()=>{if(fail)throw Error();return 12}},readState:async()=>saved,writeState:async s=>{saved=structuredClone(s)},now:()=>now};
 const engine=new PollEngine(opts);await engine.init();await engine.poll('cloud');
 now=100;fail=true;assert.equal(await engine.poll('cloud'),false);
 assert.equal(saved.values.cloud.updated_at,'1970-01-01T00:00:00.000Z');
 assert.equal(engine.schedule.due('cloud',299),false);assert.equal(engine.schedule.due('cloud',300),true);
});
