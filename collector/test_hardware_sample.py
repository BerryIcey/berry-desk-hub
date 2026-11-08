import unittest
from hardware_sample import normalize
class HardwareTests(unittest.TestCase):
 def test_separate_values_and_missing_gpu_never_become_zero(self):
  out=normalize(cpu=27,cpu_temp=55,gpu=None,gpu_temp=70,used=4,total=8,boot=1700000000,now='2026-10-30T00:00:00Z')
  self.assertEqual(out['cpu']['usage_percent'],27);self.assertIsNone(out['gpu']['usage_percent']);self.assertEqual(out['gpu']['temperature_c'],70);self.assertEqual(out['memory']['usage_percent'],50)
 def test_rejects_out_of_range_values(self):
  out=normalize(cpu=101,cpu_temp=-10,gpu=-1,gpu_temp=999,used=9,total=8,boot=None,now='2026-10-30T00:00:00Z')
  self.assertIsNone(out['cpu']['usage_percent']);self.assertIsNone(out['gpu']['temperature_c']);self.assertIsNone(out['memory']['usage_percent'])
