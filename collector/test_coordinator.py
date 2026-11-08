import unittest
from coordinator import merge_sample

class CoordinatorTests(unittest.TestCase):
 def test_old_whole_section_does_not_override_newer_tasks(self):
  s={'generated_at':'2026-10-30T00:00:20Z','codex':{'updated_at':'2026-10-30T00:00:00Z','tasks_updated_at':'2026-10-30T00:00:20Z','recent_tasks':[{'title':'new','status':'running'}]}}
  ok=merge_sample(s,'quota',{'updated_at':'2026-10-30T00:00:10Z','quota_windows':[{'remaining_percent':40}]},'2026-10-30T00:00:25Z')
  self.assertTrue(ok);self.assertEqual(s['codex']['recent_tasks'][0]['title'],'new');self.assertEqual(s['codex']['quota_windows'][0]['remaining_percent'],40)
 def test_old_sample_and_future_sample_are_rejected(self):
  s={'codex':{'updated_at':'2026-10-30T00:00:10Z','quota_windows':[]}}
  for at in ('2026-10-30T00:00:09Z','2026-10-30T00:00:30Z'):
   self.assertFalse(merge_sample(s,'quota',{'updated_at':at,'quota_windows':[]},'2026-10-30T00:00:20Z'))
 def test_tasks_and_quota_keep_distinct_clocks(self):
  s={'codex':{'updated_at':'2026-10-30T00:00:00Z'}}
  self.assertTrue(merge_sample(s,'tasks',{'tasks_updated_at':'2026-10-30T00:00:10Z','recent_tasks':[],'tasks_status':'available'},'2026-10-30T00:00:11Z'))
  self.assertEqual(s['codex']['updated_at'],'2026-10-30T00:00:00Z')
