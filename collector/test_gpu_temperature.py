import importlib.util
import unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('gpu_temperature',Path(__file__).with_name('gpu_temperature.py'))
gpu=importlib.util.module_from_spec(spec);spec.loader.exec_module(gpu)

class GpuTemperatureTest(unittest.TestCase):
    def test_only_the_named_discrete_adapter_is_used(self):
        samples=[('AMD Radeon RX 6750 GRE 10GB',42000),('AMD Radeon(TM) Graphics',35000)]
        self.assertEqual(gpu.select_temperature(samples,'AMD Radeon RX 6750 GRE 10GB'),42)
        self.assertIsNone(gpu.select_temperature(samples,'AMD Unknown'))
        self.assertIsNone(gpu.select_temperature([('GPU',-1)],'GPU'))
        self.assertIsNone(gpu.select_temperature([('GPU',30000),('GPU',60000)],'GPU'))
        self.assertEqual(gpu.select_temperature([('GPU',52000),('GPU',53000)],'GPU'),53)
