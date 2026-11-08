import asyncio,json,unittest
from parallel_worker import Worker,cloud_section,tasks_section,weather_section

class WorkerTests(unittest.IsolatedAsyncioTestCase):
 async def test_weather_unchanged_source_time_is_a_successful_refresh(self):
  observed='2026-10-30T00:00:00Z';snapshot={'daily':{'updated_at':observed,'status':'available','source':'moji-xiangan','weather':{'status':'available','updated_at':observed}}};writes=[]
  async def run(command):return json.dumps({'status':'available','updated_at':observed})
  async def write(value):writes.append(value)
  worker=Worker(snapshot,write,run)
  self.assertTrue(await worker.sample('weather',['weather'],weather_section));self.assertEqual(writes,[])
 async def test_air_can_update_without_refreshing_weather_time(self):
  observed='2026-10-30T00:00:00Z';snapshot={'daily':{'updated_at':observed,'weather':{'status':'available','air_quality':None}}};writes=[]
  async def run(command):return json.dumps({'status':'available','updated_at':observed,'air_quality':{'aqi':33,'description':'优','updated_at':'2026-10-29T21:00:00Z'}})
  async def write(value):writes.append(value)
  self.assertTrue(await Worker(snapshot,write,run).sample('weather',['weather'],weather_section))
  self.assertEqual(snapshot['daily']['weather']['air_quality']['aqi'],33);self.assertEqual(snapshot['daily']['updated_at'],observed);self.assertEqual(len(writes),1)
 async def test_failed_write_does_not_commit_sample_and_can_retry(self):
  snapshot={'codex':{'updated_at':'2026-01-01T00:00:00Z'}};attempts=0
  async def run(command):return json.dumps({'updated_at':'2026-10-30T00:00:00Z','quota_windows':[]})
  async def write(value):
   nonlocal attempts
   attempts+=1
   if attempts==1:raise OSError('busy')
  worker=Worker(snapshot,write,run)
  with self.assertRaises(OSError):await worker.sample('quota',['quota'])
  self.assertEqual(snapshot['codex']['updated_at'],'2026-01-01T00:00:00Z')
  self.assertTrue(await worker.sample('quota',['quota']))
 async def test_slow_quota_and_fast_tasks_do_not_overwrite_one_another(self):
  snapshot={'codex':{'updated_at':'2026-01-01T00:00:00Z'}};writes=[];gate=asyncio.Event()
  async def run(command):
   if command[0]=='quota':
    await gate.wait();return json.dumps({'updated_at':'2026-10-30T00:00:00Z','quota_windows':[{'remaining_percent':55}]})
   return json.dumps([{'title':'task','status':'running'}])
  async def write(value):writes.append(json.loads(json.dumps(value)))
  worker=Worker(snapshot,write,run);slow=asyncio.create_task(worker.sample('quota',['quota']));await asyncio.sleep(0)
  self.assertTrue(await worker.sample('tasks',['task'],tasks_section));self.assertFalse(slow.done());gate.set();self.assertTrue(await slow)
  self.assertEqual(snapshot['codex']['recent_tasks'][0]['title'],'task');self.assertEqual(snapshot['codex']['quota_windows'][0]['remaining_percent'],55);self.assertEqual(len(writes),2)
 def test_bad_weather_is_not_promoted(self):
  with self.assertRaises(ValueError):weather_section({'status':'unknown'})
 def test_cloud_rejects_bad_numbers(self):
  value=cloud_section({'status':'available','cpu_percent':999,'latency_ms':-1})
  self.assertIsNone(value['cpu']['usage_percent']);self.assertIsNone(value['latency_ms'])
 def test_cloud_failure_does_not_replace_last_good_sample(self):
  with self.assertRaises(ValueError):cloud_section({'status':'unknown'})
