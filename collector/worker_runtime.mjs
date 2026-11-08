import {execFile} from 'node:child_process';
import {mkdir,readFile,rename,writeFile} from 'node:fs/promises';
import path from 'node:path';
import {promisify} from 'node:util';
import {createWorker} from './worker.mjs';
import {fileURLToPath} from 'node:url';

const execute=promisify(execFile);
export async function replaceState(path,state,{write=writeFile,rename:move=rename,wait=ms=>new Promise(resolve=>setTimeout(resolve,ms))}={}) {
  const temporary=path+'.tmp';await write(temporary,JSON.stringify(state),{mode:0o600});
  for(let attempt=0;attempt<5;attempt++) {
    try {await move(temporary,path);return;}
    catch(e) {if(!['EPERM','EBUSY'].includes(e?.code) || attempt===4)throw e;await wait(100*(attempt+1));}
  }
}
export async function createRuntime({root,now=Date.now,run}={}) {
  if(!root || !path.isAbsolute(root)) throw new Error('absolute root required');
  const directory=path.join(root,'.local','berry-hub-live');
  const statePath=path.join(directory,'worker-state.json');
  await mkdir(directory,{recursive:true});
  const runFile=run ?? (async(file,args)=>{
    try {await execute(file,args,{cwd:root,windowsHide:true,timeout:180000,maxBuffer:65536,env:{...process.env,PYTHONPATH:root}});return true;}
    catch {return false;}
  });
  return createWorker({now,
    readState:async()=>{try{return JSON.parse(await readFile(statePath,'utf8'));}catch{return {};}},
    writeState:async state=>replaceState(statePath,state),
    collect:()=>runFile(process.execPath,[path.join(directory,'collect.mjs')]),
    fastGpu:()=>runFile(path.join(root,'.local','venv','Scripts','python.exe'),[path.join(directory,'gpu_fast.py')]),
    fastTask:()=>runFile(path.join(root,'.local','venv','Scripts','python.exe'),[path.join(directory,'task_fast.py')]),
    transfer:()=>runFile(path.join(root,'.local','venv','Scripts','python.exe'),[path.join(directory,'serial_bridge.py')])
  });
}

// Explicit invocation only; no task scheduler or startup registration is made.
if(process.argv[1] && path.resolve(process.argv[1])===fileURLToPath(import.meta.url) && process.argv[2]==='--run') {
  if(process.env.BERRY_WORKER_LOCKED!=='1') throw new Error('use the single-instance worker runner');
  if(!process.env.BERRY_CODEX_EXECUTABLE) throw new Error('Codex executable not configured');
  const worker=await createRuntime({root:process.cwd()});
  let stopped=false;
  process.on('SIGINT',()=>{stopped=true;});process.on('SIGTERM',()=>{stopped=true;});
  while(!stopped){try{await worker.step();}catch{if(!stopped)await new Promise(resolve=>setTimeout(resolve,5000));}if(!stopped)await new Promise(resolve=>setTimeout(resolve,1000));}
}
