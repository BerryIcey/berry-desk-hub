"""Merge independent observations without resetting other source clocks."""
from copy import deepcopy
from datetime import datetime

def timestamp(value):
    try:
        t=datetime.fromisoformat(value.replace('Z','+00:00'))
        return t.timestamp() if t.tzinfo else None
    except (AttributeError,ValueError,OverflowError):return None

def merge_sample(snapshot,kind,sample,now):
    if not isinstance(snapshot,dict) or not isinstance(sample,dict):return False
    definitions={'quota':('codex','updated_at',('updated_at','quota_windows','status','source')),
                 'tasks':('codex','tasks_updated_at',('tasks_updated_at','recent_tasks','tasks_status')),
                 'hardware':('dorm_pc','updated_at',None),'zhiqing':('zhiqing','updated_at',None),
                 'cloud':('aliyun','updated_at',None),'weather':('daily','updated_at',None),
                 'schedule':('schedule','updated_at',None)}
    if kind not in definitions:return False
    section,field,keys=definitions[kind];current=timestamp(now);observed=timestamp(sample.get(field))
    if current is None or observed is None or observed>current:return False
    existing=snapshot.get(section);existing=existing if isinstance(existing,dict) else {}
    prior=timestamp(existing.get(field))
    if prior is not None and observed<=prior:return False
    if keys is None:snapshot[section]=deepcopy(sample)
    else:
        out=deepcopy(existing)
        for key in keys:
            if key in sample:out[key]=deepcopy(sample[key])
        snapshot[section]=out
    snapshot['generated_at']=now
    return True
