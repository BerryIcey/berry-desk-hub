import {test} from 'node:test';
import assert from 'node:assert/strict';
import {createWorker} from './worker.mjs';

test('collection is rate-limited and transfer does not begin before collection',async()=>{
 let now=1000,state={},collected=0,sent=0;
 const worker=await createWorker({now:()=>now,readState:async()=>state,writeState:async s=>{state=structuredClone(s)},collect:async()=>{collected++;return true},transfer:async()=>{sent++;return true}});
 await worker.step();assert.equal(collected,1);assert.equal(sent,1);
 now+=30000;await worker.step();assert.equal(collected,1);assert.equal(sent,2);
 const restarted=await createWorker({now:()=>now,readState:async()=>state,writeState:async s=>{state=structuredClone(s)},collect:async()=>{collected++;return true},transfer:async()=>{sent++;return true}});
 await restarted.step();assert.equal(collected,1);assert.equal(sent,2);
});

test('failed initial collection never transfers unverified or old snapshots',async()=>{
 let sent=0,state={};
 const worker=await createWorker({now:()=>1000,readState:async()=>state,writeState:async s=>{state=structuredClone(s)},collect:async()=>{throw Error('failed')},transfer:async()=>{sent++;return true}});
 await worker.step();assert.equal(sent,0);
});

test('fast GPU reads are independently limited and do not trigger extra full collections',async()=>{
 let now=0,state={},full=0,gpu=0;
 const worker=await createWorker({now:()=>now,readState:async()=>state,writeState:async s=>{state=structuredClone(s)},collect:async()=>{full++;return true},fastGpu:async()=>{gpu++;return true},transfer:async()=>true});
 await worker.step();now=15000;await worker.step();assert.equal(full,1);assert.equal(gpu,2);
 now=20000;await worker.step();assert.equal(gpu,2);
});

test('desktop tasks refresh independently and do not rerun quota collection',async()=>{
 let now=0,state={},full=0,tasks=0;
 const worker=await createWorker({now:()=>now,readState:async()=>state,writeState:async s=>{state=structuredClone(s)},collect:async()=>{full++;return true},fastTask:async()=>{tasks++;return true},transfer:async()=>true});
 await worker.step();now=30000;await worker.step();assert.equal(full,1);assert.equal(tasks,2);
});
