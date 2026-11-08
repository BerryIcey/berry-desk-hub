import unittest
from hub_device import to_device

class DeviceProjectionTests(unittest.TestCase):
 def test_fresh_sources_are_mapped_and_stale_sources_are_not_replayed(self):
  now="2026-10-30T01:00:00Z"
  sources={"codex":{"status":"fresh","data":{"recent_tasks":[{"title":"","status":"running"}]}},"dorm_pc":{"status":"stale","data":{"cpu":{"usage_percent":99}}},"daily":{"status":"fresh","data":{"temperature_c":26}},"zhiqing":{"status":"unknown","data":None},"aliyun":{"status":"fresh","data":{"cpu":{"usage_percent":12}}},"schedule":{"status":"fresh","data":{"courses":[]}}}
  out=to_device(sources,generated_at=now)
  self.assertEqual(out["codex"]["recent_tasks"][0]["status"],"running")
  self.assertNotIn("cpu",out["dorm_pc"])
  self.assertEqual(out["weather"]["temperature_c"],26)
  self.assertEqual(out["aliyun"]["cpu"]["usage_percent"],12)
  self.assertEqual(out["generated_at"],now)
 def test_monitor_sample_time_does_not_follow_frame_generation(self):
  sample="2026-10-30T01:00:00Z"
  sources={key:{"status":"fresh","updated_at":sample,"data":{"cpu":{"usage_percent":12}}} for key in ("dorm_pc","aliyun")}
  out=to_device(sources,generated_at="2026-10-30T01:05:00Z")
  self.assertEqual(out["dorm_pc"]["updated_at"],sample)
  self.assertEqual(out["aliyun"]["updated_at"],sample)
 def test_invalid_envelopes_are_rejected(self):
  with self.assertRaises(ValueError):to_device([],generated_at="2026-10-30T01:00:00Z")
  with self.assertRaises(ValueError):to_device({},generated_at="not-a-time")
