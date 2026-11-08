"""Isolated independent scheduler; jobs must use non-blocking I/O."""
import asyncio

class Scheduler:
    def __init__(self,jobs):
        self.jobs={name:{"interval":interval,"call":call,"next":0,"failures":0,"task":None} for name,(interval,call) in jobs.items()}
        if any(j["interval"]<=0 for j in self.jobs.values()):raise ValueError("invalid interval")
    async def step(self,now):
        for job in self.jobs.values():
            task=job["task"]
            if task is not None and task.done():
                try:task.result();job["failures"]=0
                except Exception:job["failures"]=min(job["failures"]+1,4)
                delay=job["interval"] if job["failures"]==0 else min(300,job["interval"]*(2**job["failures"]))
                job["next"]=job["start"]+delay
                job["task"]=None
            if job["task"] is None and now>=job["next"]:
                job["start"]=now
                job["task"]=asyncio.create_task(job["call"]())
    async def close(self):
        tasks=[j["task"] for j in self.jobs.values() if j["task"] is not None]
        if tasks:await asyncio.gather(*tasks,return_exceptions=True)
