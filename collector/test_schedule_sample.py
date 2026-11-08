import unittest
from schedule_sample import normalize

class ScheduleTests(unittest.TestCase):
 def test_valid_rows_are_kept(self):
  value=normalize([[1,'10:10','课程','教室']],'2026-10-30T00:00:00Z')
  self.assertEqual(value['courses'][0][2],'课程');self.assertEqual(value['status'],'available')
 def test_invalid_rows_are_rejected(self):
  for value in (None,[[8,'10:10','课程','教室']],[[1,'10:10','课程',None]]):
   with self.assertRaises(ValueError):normalize(value,'2026-10-30T00:00:00Z')
