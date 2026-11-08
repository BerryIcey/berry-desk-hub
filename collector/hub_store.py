"""Isolated in-memory prototype for independently aged, filtered sources."""
from copy import deepcopy
import json
import hashlib
import sqlite3
from contextlib import closing
from datetime import datetime, timezone, timedelta
from threading import Lock
from serial_protocol import device_payload


def stamp(value):
    try:
        parsed=datetime.fromisoformat(value.replace('Z','+00:00'))
        return parsed.timestamp() if parsed.tzinfo else None
    except (AttributeError,ValueError,OverflowError): return None


class HubStore:
    SOURCES={'codex':('codex',600),'dorm_pc':('dorm_pc',600),'zhiqing':('zhiqing',600),'aliyun':('aliyun',600),'daily':('weather',7200),'schedule':('schedule',600)}
    def __init__(self,path=None):
        self._values={};self._lock=Lock();self._path=path
        if path is not None:
            with closing(sqlite3.connect(path,timeout=2)) as db,db:
                db.execute('CREATE TABLE IF NOT EXISTS snapshots(source TEXT PRIMARY KEY,observed REAL NOT NULL,updated_at TEXT NOT NULL,data TEXT NOT NULL,processed REAL NOT NULL)')
                columns={r[1] for r in db.execute('PRAGMA table_info(snapshots)')}
                if 'fingerprint' not in columns:db.execute('ALTER TABLE snapshots ADD COLUMN fingerprint TEXT')
                for source,at,updated,data,processed,fingerprint in db.execute('SELECT source,observed,updated_at,data,processed,fingerprint FROM snapshots'):
                    if source in self.SOURCES:
                        value=json.loads(data)
                        if not isinstance(value,dict):raise ValueError('invalid stored snapshot')
                        self._values[source]=(at,updated,value,processed,fingerprint)
    def _save(self,source,value):
        if self._path is None:return True
        try:
            with closing(sqlite3.connect(self._path,timeout=2)) as db,db:
                db.execute('INSERT OR REPLACE INTO snapshots(source,observed,updated_at,data,processed,fingerprint) VALUES(?,?,?,?,?,?)',(source,value[0],value[1],json.dumps(value[2],ensure_ascii=False,allow_nan=False),value[3],value[4]))
            return True
        except (OSError,sqlite3.Error,ValueError,TypeError):return False
    def update(self,source,snapshot,*,now):
        if source not in self.SOURCES or not isinstance(snapshot,dict):return False
        section=snapshot.get(source)
        if not isinstance(section,dict):return False
        try:fingerprint=hashlib.sha256(json.dumps(section,sort_keys=True,separators=(',',':'),ensure_ascii=False,allow_nan=False).encode('utf8')).hexdigest()
        except (TypeError,ValueError,UnicodeError,OverflowError):return False
        observed=section.get('updated_at');at=stamp(observed);current=stamp(now)
        if current is None:return False
        tasks_time=section.get('tasks_updated_at');tasks_at=stamp(tasks_time)
        task_only=source=='codex' and (section.get('quota_task_only') is True or at is None or not 0<=current-at<=600)
        if task_only:
            if tasks_at is None or not 0<=current-tasks_at<=120 or section.get('tasks_status')!='available':return False
            at=tasks_at;observed=tasks_time
        if at is None or not 0<=current-at<=self.SOURCES[source][1]:return False
        try:clean=device_payload(snapshot,now=now)
        except (AttributeError,TypeError,ValueError,OverflowError,KeyError):return False
        if clean.get('status')!='fresh':return False
        key=self.SOURCES[source][0];value=clean.get(key)
        if not isinstance(value,dict):return False
        if source=='codex':
            task_time=section.get('tasks_updated_at');task_at=stamp(task_time)
            if task_at is not None and 0<=current-task_at<=120:value['tasks_updated_at']=task_time
            if task_only:value['quota_windows']=[];value['quota_task_only']=True
        if source=='daily' and isinstance(value.get('air_quality'),dict):
            weather=section.get('weather') or {};original=weather.get('air_quality') if isinstance(weather,dict) else None
            air_at=original.get('updated_at') if isinstance(original,dict) else None
            if stamp(air_at) is not None:value['air_quality']['updated_at']=air_at
            else:value['air_quality']=None
        if source=='dorm_pc':
            gpu=section.get('gpu');out=value.get('gpu')
            if isinstance(gpu,dict) and isinstance(out,dict):
                updated=gpu.get('usage_updated_at');gpu_at=stamp(updated)
                if gpu_at is not None and 0<=current-gpu_at<=600:out['usage_updated_at']=updated
        if source=='zhiqing':
            observation=section.get('last_chat_observation')
            if isinstance(observation,dict):
                finished=observation.get('finished_at');outcome=observation.get('outcome');category=observation.get('category')
                if type(finished) in (int,float) and 946684800<=finished<=current and outcome in ('success','failed','cancelled','deferred'):
                    value['last_chat_observation']={'finished_at':finished,'outcome':outcome,'category':category if category in ('none','rate_limit','capacity','quota','auth','network','timeout','local','unknown','deferred') else 'unknown'}
            ledger=section.get('token_ledger')
            def count(v):return v if type(v) is int and 0<=v<=1000000000000000 else None
            if isinstance(ledger,dict):
                started=ledger.get('started_at');recorded=count(ledger.get('recorded_requests'))
                if type(started) in (int,float) and 946684800<=started<=current and recorded is not None:
                    value['token_ledger']={'started_at':started,'recorded_requests':recorded,
                        'recorded_total':count(ledger.get('recorded_total')) if recorded>0 else None,
                        'missing_requests':count(ledger.get('missing_requests')),
                        'observed_requests':count(ledger.get('observed_requests')),
                        'observed_total':count(ledger.get('observed_total')) if count(ledger.get('observed_requests')) else None,
                        'observed_missing':count(ledger.get('observed_missing')),
                        'round_tokens':count(ledger.get('round_tokens')),'chat_requests':count(ledger.get('chat_requests')),
                        'chat_average':count(ledger.get('chat_average')) if count(ledger.get('chat_requests')) else None,
                        'latest_chat_tokens':count(ledger.get('latest_chat_tokens')),
                        'latest_chat_finished_at':ledger.get('latest_chat_finished_at') if type(ledger.get('latest_chat_finished_at')) in (int,float) and 946684800<=ledger['latest_chat_finished_at']<=current else None,
                        'gap':ledger.get('gap') if type(ledger.get('gap')) is bool else None,
                        'coverage':'reported_successes_only'}
        with self._lock:
            existing=self._values.get(source)
            if existing and at==existing[0] and fingerprint==existing[4]:return True
            quota_returned=existing and source=='codex' and existing[2].get('quota_task_only') is True and not task_only
            if quota_returned:
                previous_task=stamp(existing[2].get('tasks_updated_at'));incoming_task=stamp(value.get('tasks_updated_at'))
                if previous_task is not None and (incoming_task is None or incoming_task<previous_task):
                    for key in ('recent_tasks','tasks_status','tasks_updated_at'):value[key]=deepcopy(existing[2].get(key))
            if existing and at<=existing[0] and not quota_returned:
                if source=='codex' and at==existing[0]:
                    old_at=stamp(existing[2].get('tasks_updated_at'));new_at=stamp(value.get('tasks_updated_at'))
                    if new_at is None or (old_at is not None and new_at<=old_at):return False
                    merged=deepcopy(existing[2])
                    for key in ('recent_tasks','tasks_status','tasks_updated_at'):merged[key]=value.get(key)
                    replacement=(at,observed,merged,current,fingerprint)
                    if not self._save(source,replacement):return False
                    self._values[source]=replacement
                    return True
                if source=='daily' and at==existing[0]:
                    old_air=existing[2].get('air_quality') or {};new_air=value.get('air_quality') or {}
                    old_at=stamp(old_air.get('updated_at'));new_at=stamp(new_air.get('updated_at'))
                    if new_at is None or (old_at is not None and new_at<=old_at):return False
                    merged=deepcopy(existing[2]);merged['air_quality']=deepcopy(new_air)
                    replacement=(at,observed,merged,current,fingerprint)
                    if not self._save(source,replacement):return False
                    self._values[source]=replacement;return True
                if source!='dorm_pc' or at!=existing[0]:return False
                old_gpu=existing[2].get('gpu') or {};new_gpu=value.get('gpu') or {}
                old_at=stamp(old_gpu.get('usage_updated_at'));new_at=stamp(new_gpu.get('usage_updated_at'))
                if old_at is not None and new_at==old_at:
                    before=deepcopy(existing[2]);after=deepcopy(value)
                    before.pop('uptime_seconds',None);after.pop('uptime_seconds',None)
                    return before==after
                if new_at is None or (old_at is not None and new_at<=old_at):return False
                merged=deepcopy(existing[2]);merged_gpu=merged.get('gpu') or {}
                merged_gpu.update(usage_percent=new_gpu.get('usage_percent'),usage_updated_at=new_gpu['usage_updated_at'])
                merged['gpu']=merged_gpu;replacement=(at,observed,merged,current,fingerprint)
                if not self._save(source,replacement):return False
                self._values[source]=replacement
                return True
            replacement=(at,observed,deepcopy(value),current,fingerprint)
            if not self._save(source,replacement):return False
            self._values[source]=replacement
        return True
    def read(self,source,*,now):
        current=stamp(now)
        if source not in self.SOURCES or current is None:return None
        with self._lock:
            value=deepcopy(self._values.get(source))
        if value is None:return {'status':'unknown','updated_at':None,'data':None}
        at,observed,data,processed,_fingerprint=value
        max_age=120 if source=='codex' and data.get('quota_task_only') is True else self.SOURCES[source][1]
        state='fresh' if 0<=current-at<=max_age else 'stale' if current>=at else 'unknown' 
        if source=='codex' and 'tasks_updated_at' in data:
            task_at=stamp(data.get('tasks_updated_at'))
            if task_at is None or not 0<=current-task_at<=120:
                data['tasks_status']='unknown'
                for task in data.get('recent_tasks',[]):
                    task['status']='unknown';task.pop('notice_key',None);task.pop('notice_state',None)
        if source=='daily' and isinstance(data.get('air_quality'),dict):
            air_at=stamp(data['air_quality'].get('updated_at'))
            if air_at is None or not 0<=current-air_at<=86400:data['air_quality']=None
            elif current-air_at>10800:data['air_quality']['stale']=True
            else:data['air_quality'].pop('stale',None)
        if source=='aliyun' and type(data.get('uptime_seconds')) is int:
            data['uptime_seconds']=int(data['uptime_seconds']+current-at) if state=='fresh' else None
        if source=='schedule' and data.get('status')=='available':
            local=datetime.fromtimestamp(current,timezone(timedelta(hours=8))).date();day=local.isoweekday()
            monday=local-timedelta(days=day-1)
            data['day']=day;data['week_dates']=[(monday+timedelta(days=i)).day for i in range(7)]
        if source=='dorm_pc' and isinstance(data.get('gpu'),dict):
            gpu=data['gpu'];gpu_at=stamp(gpu.get('usage_updated_at'))
            if gpu_at is not None and not 0<=current-gpu_at<=600:gpu['usage_percent']=None
        if source=='zhiqing':
            if not 0<=current-at<=120:
                for key in ('process','qq_connection','model_api','message_processing'):
                    if isinstance(data.get('components'),dict):data['components'][key]='unknown'
            age=data.get('last_reply_seconds_ago')
            if type(age) is int:
                data['last_reply_seconds_ago']=int(age+current-processed) if current>=processed and age+current-processed<=86400 else None
            zone=timezone(timedelta(hours=8))
            if datetime.fromtimestamp(at,zone).date()!=datetime.fromtimestamp(current,zone).date():data['today_confirmed_rounds']=None
        return {'status':state,'updated_at':observed,'data':data}


def source_envelope(source,snapshot,*,now):
    """Filter on the sender before anything crosses a network boundary."""
    temporary=HubStore()
    if not temporary.update(source,snapshot,now=now):return None
    result=temporary.read(source,now=now)
    data=result['data'];data['updated_at']=result['updated_at']
    if source=='aliyun' and data.get('uptime_seconds') is not None:
        raw=snapshot['aliyun'].get('uptime_seconds')
        data['uptime_seconds']=raw if type(raw) is int and 0<=raw<10000000000000000 else None
    if source=='zhiqing' and data.get('last_reply_seconds_ago') is not None:
        raw=snapshot['zhiqing'].get('last_reply_seconds_ago')
        data['last_reply_seconds_ago']=raw if type(raw) is int and 0<=raw<=86400 else None
    if source=='daily':
        air=data.get('air_quality')
        original=((snapshot.get('daily') or {}).get('weather') or {}).get('air_quality')
        if isinstance(air,dict) and isinstance(original,dict):air['updated_at']=original.get('updated_at')
        data={'updated_at':result['updated_at'],'source':'moji-xiangan','weather':data}
    elif source=='schedule':
        data['courses']=[[row['day'],row['time'],row['title'],row['location']] for row in data.get('courses',[])]
    return {'generated_at':snapshot['generated_at'],source:data}
