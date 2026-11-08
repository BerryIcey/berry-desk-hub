import { PollEngine } from './poll_engine.mjs';

// Explicitly called by a host process; importing this module never opens USB.
export async function createWorker({now=Date.now, readState, writeState, collect, transfer, fastGpu, fastTask}) {
  let ready=false;
  const engine=new PollEngine({intervals:{collect:300000,transfer:30000,...(fastGpu?{fast_gpu:15000}:{}),...(fastTask?{fast_task:30000}:{})},now,readState,writeState,sources:{
    collect:async()=>{if(await collect()!==true)throw new Error('collection unavailable');ready=true;return true;},
    transfer:async()=>{if(await transfer()!==true)throw new Error('transfer unavailable');return true;},
    ...(fastGpu?{fast_gpu:async()=>{if(await fastGpu()!==true)throw new Error('GPU unavailable');return true;}}:{}),
    ...(fastTask?{fast_task:async()=>{if(await fastTask()!==true)throw new Error('task status unavailable');return true;}}:{})
  }});
  await engine.init();
  return {async step(){
    if(engine.schedule.due('collect',now())) await engine.poll('collect');
    if(ready && fastGpu && engine.schedule.due('fast_gpu',now())) await engine.poll('fast_gpu');
    if(ready && fastTask && engine.schedule.due('fast_task',now())) await engine.poll('fast_task');
    if(ready && engine.schedule.due('transfer',now())) await engine.poll('transfer');
  }};
}
