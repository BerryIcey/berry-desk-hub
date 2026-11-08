"""Independent source jobs and a single serialized snapshot writer."""
import asyncio,json,os,shutil,sys,time
from copy import deepcopy
from datetime import datetime,timezone
from pathlib import Path
from async_scheduler import Scheduler
from coordinator import merge_sample,timestamp
from worker_lock import instance_lock
from conversation_watch import marker
from start_context import live_pipe

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]

def utc():return datetime.now(timezone.utc).isoformat()

class Worker:
    def __init__(self,snapshot,write,run):self.snapshot=snapshot;self.write=write;self.run=run;self.lock=asyncio.Lock()
    async def sample(self,kind,command,convert=lambda x:x):
        raw=await self.run(command)
        try:sample=convert(json.loads(raw))
        except (TypeError,ValueError,KeyError):return False
        async with self.lock:
            if kind=='weather' and sample.get('updated_at') and sample.get('updated_at')==(self.snapshot.get('daily') or {}).get('updated_at'):
                old=(self.snapshot['daily'].get('weather') or {}).get('air_quality') or {}
                new=(sample.get('weather') or {}).get('air_quality') or {}
                old_at=timestamp(old.get('updated_at'));new_at=timestamp(new.get('updated_at'))
                if new_at is not None and (old_at is None or new_at>old_at):
                    candidate=deepcopy(self.snapshot);candidate['daily']['weather']['air_quality']=deepcopy(new);candidate['generated_at']=utc()
                    await self.write(candidate);self.snapshot.clear();self.snapshot.update(candidate)
                return True
            candidate=deepcopy(self.snapshot)
            if not merge_sample(candidate,kind,sample,utc()):return False
            await self.write(candidate)
            self.snapshot.clear();self.snapshot.update(candidate)
        return True

async def run_process(args,timeout=20):
    env={**os.environ,'PYTHONUTF8':'1','PYTHONPATH':str(ROOT)}
    if str(args[-1]).endswith('app_task_probe.py'):
        pipe=await asyncio.to_thread(live_pipe)
        if pipe:env['CODEX_APP_TOOLS_PIPE_PATH']=pipe
        else:env.pop('CODEX_APP_TOOLS_PIPE_PATH',None)
    process=await asyncio.create_subprocess_exec(*args,cwd=ROOT,env=env,stdout=asyncio.subprocess.PIPE,stderr=asyncio.subprocess.DEVNULL,creationflags=getattr(__import__('subprocess'),'CREATE_NO_WINDOW',0))
    try:out,_=await asyncio.wait_for(process.communicate(),timeout)
    except asyncio.TimeoutError:
        process.kill();await process.communicate();raise
    if process.returncode or len(out)>262144:raise ValueError('probe failed')
    return out.decode('utf8')

def cloud_section(data):
    if data.get('status')!='available':raise ValueError('cloud unavailable')
    def pct(v):return round(v) if type(v) in (int,float) and 0<=v<=100 else None
    def mem(total,free):
        if type(total) not in (int,float) or type(free) not in (int,float) or total<=0 or not 0<=free<=total:return {'usage_percent':None,'used_bytes':None,'total_bytes':None}
        return {'usage_percent':round((total-free)*100/total),'used_bytes':total-free,'total_bytes':total}
    disk=data.get('disk_total');used=data.get('disk_used')
    out={'status':'available' if data.get('status')=='available' else 'unknown','source':'pinned-ssh','updated_at':utc(),'relay_status':data.get('relay_status','unknown'),'cpu':{'usage_percent':pct(data.get('cpu_percent'))},'memory':mem(data.get('memory_total'),data.get('memory_available')),'storage':mem(disk,disk-used if type(disk) in (int,float) and type(used) in (int,float) else None)}
    for key in ('uptime_seconds','latency_ms','download_bytes_per_second','upload_bytes_per_second'):
        v=data.get(key);out[key]=v if type(v) is int and 0<=v<10**16 else None
    return out

def zhiqing_section(data):
    components={k:data.get(k) if data.get(k) in ('healthy','offline','unknown') else 'unknown' for k in ('process','qq_connection','model_api','message_processing')}
    return {'status':'unknown','updated_at':utc(),'source':'local-read-only-probe','components':components,'model_name':data.get('model_name'),'last_reply_seconds_ago':data.get('last_reply_seconds_ago'),'today_confirmed_rounds':data.get('today_confirmed_rounds'),'token_ledger':data.get('token_ledger'),'last_chat_observation':data.get('last_chat_observation')}

def weather_section(data):
    if data.get('status')!='available':raise ValueError('weather unavailable')
    return {'status':'available','updated_at':data['updated_at'],'source':'moji-xiangan','weather':data}

def tasks_section(data):
    if not isinstance(data,list):raise ValueError('tasks unavailable')
    return {'tasks_updated_at':utc(),'tasks_status':'available','recent_tasks':data}

async def main():
    python=sys.executable;node=shutil.which('node')
    if not node or not os.environ.get('BERRY_CODEX_EXECUTABLE'):return 1
    snapshot=json.loads((HERE/'snapshot.json').read_text(encoding='utf8'))
    async def write(data):
        path=HERE/'snapshot.json';temp=HERE/'snapshot-parallel.tmp'
        temp.write_text(json.dumps(data,ensure_ascii=False,indent=2),encoding='utf8')
        for attempt in range(5):
            try:os.replace(temp,path);return
            except PermissionError:
                if attempt==4:raise
                await asyncio.sleep(.1*(attempt+1))
    worker=Worker(snapshot,write,run_process)
    def job(kind,file,convert=lambda x:x):
        async def invoke():
            if not await worker.sample(kind,[node if file.endswith('.mjs') else python,str(HERE/file)],convert):raise ValueError('sample unavailable')
        return invoke
    async def transfer():await run_process([python,str(HERE/'serial_bridge.py')],timeout=10)
    jobs={'hardware':(10,job('hardware','hardware_sample.py')),'tasks':(10,job('tasks','app_task_probe.py',tasks_section)),
          'quota':(60,job('quota','quota_probe.mjs')),'cloud':(60,job('cloud','cloud_probe.py',cloud_section)),
          'zhiqing':(60,job('zhiqing','zhiqing_probe.py',zhiqing_section)),'weather':(3600,job('weather','weather_probe.py',weather_section)),
          'schedule':(300,job('schedule','schedule_sample.py')),'transfer':(10,transfer)}
    scheduler=Scheduler(jobs)
    async def watch_conversations():
        previous=await asyncio.to_thread(marker,ROOT);pending=False
        while True:
            current=await asyncio.to_thread(marker,ROOT)
            if current is not None and previous is not None and current!=previous:pending=True
            if current is not None:previous=current
            state=scheduler.jobs['zhiqing']
            if pending and state['task'] is None:
                state['next']=0;pending=False
            await asyncio.sleep(2)
    asyncio.create_task(watch_conversations())
    while True:
        await scheduler.step(time.monotonic());await asyncio.sleep(.25)

if __name__=='__main__':
    with instance_lock(HERE/'worker.lock') as acquired:
        if not acquired:sys.exit(1)
        sys.exit(asyncio.run(main()))
