import asyncio,unittest
from async_scheduler import Scheduler

class SchedulerTests(unittest.IsolatedAsyncioTestCase):
 async def test_slow_source_does_not_block_fast_source_or_overlap_itself(self):
  release=asyncio.Event();started=[]
  async def slow():started.append("slow");await release.wait()
  async def fast():started.append("fast")
  scheduler=Scheduler({"slow":(60,slow),"fast":(10,fast)})
  await scheduler.step(0);await asyncio.sleep(0)
  await scheduler.step(10);await asyncio.sleep(0)
  await scheduler.step(60);await asyncio.sleep(0)
  self.assertEqual(started.count("slow"),1);self.assertEqual(started.count("fast"),3)
  release.set();await scheduler.close()
 async def test_failures_do_not_stop_other_jobs_and_retry_with_backoff(self):
  calls=[]
  async def fail():calls.append("bad");raise RuntimeError()
  async def good():calls.append("good")
  s=Scheduler({"bad":(10,fail),"good":(10,good)})
  await s.step(0);await asyncio.sleep(0);await s.step(10);await asyncio.sleep(0)
  self.assertEqual(calls.count("bad"),1);self.assertEqual(calls.count("good"),2)
  await s.step(20);await asyncio.sleep(0);self.assertEqual(calls.count("bad"),2);await s.close()

 async def test_real_slow_job_does_not_delay_three_fast_ticks(self):
  from time import monotonic
  start=monotonic();fast=[]
  async def slow():await asyncio.sleep(0.45)
  async def send():fast.append(monotonic()-start)
  s=Scheduler({"slow":(2,slow),"send":(0.1,send)})
  while monotonic()-start<0.36:
   await s.step(monotonic()-start);await asyncio.sleep(0.005)
  await s.close()
  self.assertGreaterEqual(len(fast),3);self.assertLess(max(b-a for a,b in zip(fast,fast[1:])),0.16)
