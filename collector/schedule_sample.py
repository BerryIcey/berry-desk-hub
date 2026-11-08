import json
from datetime import datetime,timezone
from pathlib import Path

def normalize(rows,now):
    if not isinstance(rows,list) or not all(isinstance(row,list) and len(row)==4 and type(row[0]) is int and 1<=row[0]<=7 and all(isinstance(v,str) for v in row[1:]) for row in rows):raise ValueError('invalid schedule')
    return {'status':'available','updated_at':now,'source':'confirmed-repeating-preview','courses':rows}

if __name__=='__main__':
    rows=json.loads(Path(__file__).with_name('schedule.json').read_text(encoding='utf8'))
    print(json.dumps(normalize(rows,datetime.now(timezone.utc).isoformat()),ensure_ascii=False))
