"""Refresh desktop task metadata without refreshing quota timestamps."""
import asyncio,json,os,sys
from datetime import datetime,timezone
from pathlib import Path

STATES={'running','waiting_approval','blocked','error','unknown','idle','completed','interrupted'}
def update(path,tasks,now=None):
    now=datetime.now(timezone.utc) if now is None else now
    if not isinstance(tasks,list) or len(tasks)>4 or any(not isinstance(t,dict) or not isinstance(t.get('title'),str) or t.get('status') not in STATES for t in tasks):return False
    try:
        path=Path(path);data=json.loads(path.read_text(encoding='utf8'));codex=data.get('codex')
        if not isinstance(codex,dict):return False
        codex['recent_tasks']=[{'title':t['title'][:100],'status':t['status'],**{k:t[k] for k in ('notice_key','notice_state') if k in t}} for t in tasks]
        codex['tasks_status']='available';codex['tasks_updated_at']=now.isoformat();data['generated_at']=now.isoformat()
        temp=path.with_name('snapshot-task-fast.tmp');temp.write_text(json.dumps(data,ensure_ascii=False,indent=2),encoding='utf8');os.replace(temp,path)
        return True
    except (OSError,TypeError,ValueError):return False

if __name__=='__main__':
    from app_task_probe import probe
    try:tasks=asyncio.run(asyncio.wait_for(probe(),20))
    except (TimeoutError,KeyboardInterrupt):tasks=None
    sys.exit(0 if update(Path(__file__).with_name('snapshot.json'),tasks) else 1)
