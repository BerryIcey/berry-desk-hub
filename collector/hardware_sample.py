"""Collect the existing desktop sensors concurrently in a separate process."""
import json,math
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime,timezone
import psutil
import cpu_temperature,gpu_probe,gpu_temperature

def normalize(*,cpu,cpu_temp,gpu,gpu_temp,used,total,boot,now):
    def val(v,maximum):return round(v) if type(v) in (int,float) and math.isfinite(v) and 0<=v<=maximum else None
    valid=type(used) is int and type(total) is int and total>0 and 0<=used<=total
    try:boot_at=datetime.fromtimestamp(boot,timezone.utc).isoformat() if type(boot) in (int,float) and math.isfinite(boot) and boot>0 else None
    except (ValueError,OverflowError,OSError):boot_at=None
    return {'status':'available','source':'windows-sensors','updated_at':now,'cpu':{'usage_percent':val(cpu,100),'temperature_c':val(cpu_temp,120)},'gpu':{'usage_percent':val(gpu,100),'temperature_c':val(gpu_temp,120),'usage_updated_at':now},'memory':{'usage_percent':round(100*used/total) if valid else None,'used_bytes':used if valid else None,'total_bytes':total if valid else None},'boot_at':boot_at}

def probe():
    with ThreadPoolExecutor(max_workers=4) as pool:
        jobs=[pool.submit(fn) for fn in (lambda:psutil.cpu_percent(interval=1),cpu_temperature.probe,gpu_probe.probe,gpu_temperature.probe)]
        values=[]
        for job in jobs:
            try:values.append(job.result())
            except Exception:values.append(None)
    memory=psutil.virtual_memory();now=datetime.now(timezone.utc).isoformat()
    return normalize(cpu=values[0],cpu_temp=values[1],gpu=values[2],gpu_temp=values[3],used=memory.total-memory.available,total=memory.total,boot=psutil.boot_time(),now=now)

if __name__=='__main__':print(json.dumps(probe()))
