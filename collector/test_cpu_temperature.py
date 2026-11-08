import importlib.util
import unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('cpu_temperature',Path(__file__).with_name('cpu_temperature.py'))
cpu=importlib.util.module_from_spec(spec);spec.loader.exec_module(cpu)

class CpuTemperatureTest(unittest.TestCase):
    def test_validates_sensor_and_never_turns_missing_into_zero(self):
        self.assertEqual(cpu.valid_temperature({'temperature_c':51.3}),51)
        self.assertIsNone(cpu.valid_temperature({'temperature_c':None}))
        self.assertIsNone(cpu.valid_temperature({'temperature_c':200}))
        self.assertIsNone(cpu.valid_temperature({'temperature_c':True}))
