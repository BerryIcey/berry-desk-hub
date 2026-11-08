import importlib.util
import unittest
from pathlib import Path

spec=importlib.util.spec_from_file_location('bridge',Path(__file__).with_name('serial_bridge.py'))
bridge=importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)

class SerialBridgeTests(unittest.TestCase):
    def test_serial_control_lines_are_disabled_before_open(self):
        class SerialModule:
            class Serial:
                def __init__(self,**kwargs):
                    self.port=kwargs.get('port');self.dtr=True;self.rts=True;self.opened=False
                def open(self):
                    self.opened=True
                    if self.dtr or self.rts: raise AssertionError('reset lines asserted')
        port=bridge.open_device(SerialModule,'COM3')
        self.assertTrue(port.opened)
        self.assertEqual(port.port,'COM3')
    def test_no_payload_before_valid_handshake(self):
        class Port:
            def __init__(self): self.sent=[]
            def write(self, v): self.sent.append(v)
            def readline(self): return b'not our firmware\n'
        port=Port()
        self.assertFalse(bridge.transfer(port,{'status':'unknown'},nonce='a'*32))
        self.assertEqual(len(port.sent),1)

    def test_matching_handshake_and_ack(self):
        class Port:
            def __init__(self): self.sent=[]
            def write(self, v): self.sent.append(v)
            def readline(self):
                return (b'BERRY-HUB/1 '+b'a'*32+b'\n') if len(self.sent)==1 else (b'ACK '+self.sent[-1].split(b' ',2)[1]+b'\n')
        port=Port()
        self.assertTrue(bridge.transfer(port,{'status':'unknown'},nonce='a'*32))
        self.assertEqual(len(port.sent),2)

if __name__ == '__main__': unittest.main()
