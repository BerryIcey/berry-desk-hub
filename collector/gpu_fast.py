"""Update only the discrete GPU load, preserving all other source timestamps."""
import json
import os
import math
import sys
from datetime import datetime,timezone
from pathlib import Path

def update(path,read,now=None):
    now=datetime.now(timezone.utc) if now is None else now
    try:
        value=read()
        if type(value) not in (int,float) or not math.isfinite(value) or not 0<=value<=100:return False
        data=json.loads(Path(path).read_text(encoding='utf8'))
        if data.get('schema_version') not in (None,'1.0') or not isinstance(data.get('dorm_pc'),dict):return False
        gpu=data['dorm_pc'].get('gpu')
        if not isinstance(gpu,dict):return False
        gpu['usage_percent']=value;gpu['usage_updated_at']=now.isoformat();data['generated_at']=now.isoformat()
        temp=Path(path).with_name('snapshot-gpu-fast.tmp')
        temp.write_text(json.dumps(data,ensure_ascii=False,indent=2),encoding='utf8');os.replace(temp,path)
        return True
    except (OSError,ValueError,TypeError):return False

if __name__=='__main__':
    from gpu_probe import probe
    sys.exit(0 if update(Path(__file__).with_name('snapshot.json'),probe) else 1)
