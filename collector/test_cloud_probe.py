import importlib.util
import unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('cloud_probe',Path(__file__).with_name('cloud_probe.py'))
cloud=importlib.util.module_from_spec(spec);spec.loader.exec_module(cloud)

class CloudLatencyTests(unittest.TestCase):
    def test_tcp_latency_uses_successful_connection_only(self):
        class Socket:
            def close(self): pass
        ticks=iter((1.0,1.025))
        self.assertEqual(cloud.tcp_latency('example',connect=lambda *a,**k:Socket(),clock=lambda:next(ticks)),25)
        def fail(*args,**kwargs): raise OSError('offline')
        self.assertIsNone(cloud.tcp_latency('example',connect=fail,clock=lambda:1.0))
