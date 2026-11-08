"""Transmit one snapshot only after a matching device handshake."""
import json
import secrets
import sys
from pathlib import Path

sys.path.insert(0,str(Path(__file__).resolve().parent))
from serial_protocol import device_payload, make_device_frame


def open_device(serial_module, name):
    # DTR/RTS can reset the ESP32; set both before opening the port.
    port=serial_module.Serial(port=None,baudrate=115200,timeout=3,write_timeout=3)
    port.dtr=False
    port.rts=False
    port.port=name
    port.open()
    return port


def transfer(port, payload, *, nonce=None):
    nonce = nonce or secrets.token_hex(16)
    if len(nonce)!=32 or any(c not in '0123456789abcdef' for c in nonce):
        return False
    port.write(b'HELLO '+nonce.encode('ascii')+b'\n')
    if port.readline().strip() != b'BERRY-HUB/1 '+nonce.encode('ascii'):
        return False
    frame = make_device_frame(payload)
    port.write(frame)
    return port.readline().strip() == b'ACK '+frame.split(b' ',2)[1]


def main():
    if Path(__file__).with_name("usb-data-paused.flag").is_file():
        print("usb_data_paused")
        return 0
    sys.path.insert(0,str(Path(__file__).resolve().parent.parent/'berry-hub-tools'))
    import serial
    from serial.tools import list_ports
    ports=[p.device for p in list_ports.comports() if p.vid == 0x303A and p.pid == 0x1001]
    if len(ports) != 1:
        print('device_unavailable')
        return 1
    file=Path(__file__).with_name('snapshot.json')
    try:
        snapshot=json.loads(file.read_text(encoding='utf8'))
        with open_device(serial,ports[0]) as port:
            return 0 if transfer(port,device_payload(snapshot)) else 1
    except (OSError, ValueError, serial.SerialException):
        print('transfer_unavailable')
        return 1


if __name__ == '__main__': raise SystemExit(main())
