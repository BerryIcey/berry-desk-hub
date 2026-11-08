import importlib.util,json,tempfile,unittest
from pathlib import Path
from datetime import datetime,timezone
spec=importlib.util.spec_from_file_location('task_fast',Path(__file__).with_name('task_fast.py'));module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
class TaskFastTests(unittest.TestCase):
 def test_update_only_tasks_preserves_quota_and_source_time(self):
  with tempfile.TemporaryDirectory() as d:
   p=Path(d)/'s.json';p.write_text(json.dumps({'generated_at':'2026-10-30T01:00:00Z','codex':{'updated_at':'2026-10-30T01:00:00Z','quota_windows':[{'remaining_percent':50}]}},ensure_ascii=False),encoding='utf8')
   self.assertTrue(module.update(p,[{'title':'a','status':'running'}],datetime(2026,10,30,1,1,tzinfo=timezone.utc)))
   data=json.loads(p.read_text(encoding='utf8'));self.assertEqual(data['codex']['updated_at'],'2026-10-30T01:00:00Z');self.assertEqual(data['codex']['quota_windows'][0]['remaining_percent'],50);self.assertEqual(data['codex']['recent_tasks'][0]['status'],'running')
 def test_failed_or_malformed_probe_does_not_erase_current_data(self):
  with tempfile.TemporaryDirectory() as d:
   p=Path(d)/'s.json';p.write_text('{"codex":{}}',encoding='utf8');before=p.read_bytes()
   self.assertFalse(module.update(p,None));self.assertFalse(module.update(p,[{'status':'bogus'}]));self.assertEqual(p.read_bytes(),before)
if __name__=='__main__':unittest.main()
