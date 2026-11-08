import importlib.util
import json
import tempfile
import unittest
from datetime import datetime,timezone
from pathlib import Path

spec=importlib.util.spec_from_file_location('gpu_fast',Path(__file__).with_name('gpu_fast.py'))
fast=importlib.util.module_from_spec(spec);spec.loader.exec_module(fast)

class GpuFastTest(unittest.TestCase):
    def test_only_gpu_usage_and_its_timestamp_are_refreshed(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'snapshot.json'
            p.write_text(json.dumps({'generated_at':'2026-10-29T11:00:00Z','dorm_pc':{'updated_at':'2026-10-29T11:00:00Z','cpu':{'usage_percent':20},'gpu':{'usage_percent':0,'temperature_c':55}}}),encoding='utf8')
            now=datetime(2026,10,29,12,0,tzinfo=timezone.utc)
            self.assertTrue(fast.update(p,lambda:42,now))
            result=json.loads(p.read_text(encoding='utf8'))
            self.assertEqual(result['dorm_pc']['gpu']['usage_percent'],42)
            self.assertEqual(result['dorm_pc']['updated_at'],'2026-10-29T11:00:00Z')
            self.assertEqual(result['dorm_pc']['gpu']['temperature_c'],55)
            self.assertEqual(result['dorm_pc']['gpu']['usage_updated_at'],now.isoformat())
            self.assertFalse(fast.update(p,lambda:None,now))
            self.assertTrue(fast.update(p,lambda:0.16,now))
            self.assertEqual(json.loads(p.read_text(encoding='utf8'))['dorm_pc']['gpu']['usage_percent'],0.16)
