"""Read the Ryzen Tctl/Tdie sensor using the locally verified LHM library."""
import json
import math
import subprocess
from pathlib import Path

def valid_temperature(data):
    value=data.get('temperature_c') if isinstance(data,dict) else None
    return round(value) if type(value) in (int,float) and math.isfinite(value) and 0<=value<=120 else None

def probe():
    root=Path(__file__).resolve().parent
    library=root/'hardware-reference-0.9.6/extracted/LibreHardwareMonitorLib.dll'
    script=root/'cpu_temperature.ps1'
    try:
        output=subprocess.run(['powershell.exe','-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',str(script),'-Library',str(library)],capture_output=True,text=True,timeout=12,check=True,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        return valid_temperature(json.loads(output.stdout))
    except (OSError,ValueError,subprocess.SubprocessError):return None

if __name__=='__main__': print(json.dumps({'temperature_c':probe()}))
