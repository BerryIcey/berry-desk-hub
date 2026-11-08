import importlib.util
import unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('gpu_probe',Path(__file__).with_name('gpu_probe.py'))
gpu=importlib.util.module_from_spec(spec)
spec.loader.exec_module(gpu)

class GPUProbeTests(unittest.TestCase):
    def test_uses_only_the_selected_physical_adapter_and_busiest_engine(self):
        adapters=[{'luid':'0x00000000_0x00000001','vram':100,'software':False},{'luid':'0x00000000_0x00000002','vram':1000,'software':False}]
        counters=[{'Name':'pid_1_luid_0x00000000_0x00000001_phys_0_eng_0_engtype_3D','UtilizationPercentage':90},
                  {'Name':'pid_1_luid_0x00000000_0x00000002_phys_0_eng_0_engtype_3D','UtilizationPercentage':20},
                  {'Name':'pid_2_luid_0x00000000_0x00000002_phys_0_eng_0_engtype_3D','UtilizationPercentage':15},
                  {'Name':'pid_3_luid_0x00000000_0x00000002_phys_0_eng_1_engtype_Copy','UtilizationPercentage':10}]
        self.assertEqual(gpu.measure(adapters,counters),35)
    def test_missing_or_ambiguous_adapter_stays_unknown(self):
        self.assertIsNone(gpu.measure([],[]))
        self.assertIsNone(gpu.measure([{'luid':'bad','vram':100,'software':False}],[]))
    def test_duplicate_adapter_without_counters_is_excluded(self):
        adapters=[{'luid':'0x00000000_0x00000001','vram':1000,'software':False},{'luid':'0x00000000_0x00000002','vram':1000,'software':False}]
        counters=[{'Name':'pid_1_luid_0x00000000_0x00000001_phys_0_eng_0_engtype_3D','UtilizationPercentage':12}]
        self.assertEqual(gpu.measure(adapters,counters),12)
    def test_small_nonzero_utilization_is_not_rounded_to_zero(self):
        adapters=[{'luid':'0x00000000_0x00000001','vram':1000,'software':False}]
        counters=[{'Name':'pid_1_luid_0x00000000_0x00000001_phys_0_eng_0_engtype_3D','UtilizationPercentage':0.2}]
        self.assertEqual(gpu.measure(adapters,counters),0.2)
