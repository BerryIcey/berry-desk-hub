"""Read-only local service checks; do not export credentials or chat content."""
import json
import socket
import sqlite3
import time
import re
from datetime import datetime, timezone, timedelta
from pathlib import Path
from contextlib import closing

def delivery_metrics(root,now=None):
    now=time.time() if now is None else now
    path=Path(root)/'.local/state-sync/deliveries.db'
    if not path.is_file(): return {}
    start=datetime.fromtimestamp(now,timezone(timedelta(hours=8))).replace(hour=0,minute=0,second=0,microsecond=0).timestamp()
    try:
        with closing(sqlite3.connect(path.resolve().as_uri()+'?mode=ro',uri=True,timeout=2)) as db:
            last=db.execute('SELECT MAX(at) FROM deliveries WHERE at<=? AND at>=?',(now,now-86400)).fetchone()[0]
            ids=db.execute("SELECT id FROM deliveries WHERE at>=? AND at<=?",(start,now)).fetchall()
            count=len({match.group(1) for (identifier,) in ids if (match:=re.fullmatch(r'(?!initiative:)([^:]+):(?:transport|voice):[^:]+',identifier or ''))})
        return {'last_reply_seconds_ago':int(now-last) if last is not None else None,'today_confirmed_rounds':count}
    except (OSError,sqlite3.Error,ValueError): return {}

def last_chat_observation(root):
    path=Path(root)/'.local/health/request-diagnostics.db'
    if not path.is_file(): return None
    try:
        with closing(sqlite3.connect(path.resolve().as_uri()+'?mode=ro',uri=True,timeout=2)) as db:
            row=db.execute("SELECT finished,status,category FROM requests WHERE caller IN ('chat','recovery') AND finished<=? ORDER BY finished DESC LIMIT 1",(time.time(),)).fetchone()
        if not row or row[1] not in ('success','failed','cancelled','deferred'): return None
        categories=('none','rate_limit','capacity','quota','auth','network','timeout','local','unknown','deferred')
        return {'finished_at':row[0],'outcome':row[1],'category':row[2] if row[2] in categories else 'unknown'}
    except (OSError,sqlite3.Error,ValueError): return None


def periodic_observation(root,now=None):
    now=time.time() if now is None else now
    try:
        value=json.loads((Path(root)/'.local/health/periodic-model-check.json').read_text(encoding='utf8'))
        at=value.get('checked_at');result=value.get('result')
        if type(at) not in (int,float) or not 0<=now-at<=1200 or result not in ('healthy','failed'):return None
        return {'finished_at':at,'outcome':'success' if result=='healthy' else 'failed','category':'none' if result=='healthy' else 'unknown'}
    except (OSError,ValueError,TypeError):return None


def model_evidence(observation,now=None,blocked=False):
    now=time.time() if now is None else now
    if blocked is True:return 'offline'
    if blocked is None or not isinstance(observation,dict):return 'unknown'
    at=observation.get('finished_at');outcome=observation.get('outcome')
    if type(at) not in (int,float) or not 0<=now-at<=1200:return 'unknown'
    return 'healthy' if outcome=='success' else 'offline' if outcome in ('failed','deferred') else 'unknown'

def model_blocked(root,now=None):
    now=time.time() if now is None else now
    path=Path(root)/'.local/health/request-gate.db'
    if not path.is_file():return None
    try:
        with closing(sqlite3.connect(path.resolve().as_uri()+'?mode=ro',uri=True,timeout=2)) as db:
            row=db.execute('SELECT category,until,auth_stamp FROM circuit WHERE id=1').fetchone()
        if row is None:return False
        category,until,stamp=row
        if category=='auth':
            from integration.request_gate import auth_stamp
            return stamp==auth_stamp()
        return type(until) in (int,float) and now<until
    except (OSError,sqlite3.Error,ValueError,ImportError):return None


def model_name(root):
    try:
        from integration.recovery_notify import configured_model
        config=json.loads((Path(root)/'.local/astrbot-runtime/data/cmd_config.json').read_text(encoding='utf8'))
        model=configured_model(config)
        return model if isinstance(model,str) and re.fullmatch(r'[A-Za-z0-9._-]{1,40}',model) else None
    except (OSError,ValueError,KeyError,TypeError): return None


def probe(root):
    result = {'process': 'unknown', 'qq_connection': 'unknown',
              'model_api': 'unknown', 'message_processing': 'unknown'}
    try:
        with socket.create_connection(('127.0.0.1', 6199), timeout=2):
            result['process'] = 'healthy'
    except OSError:
        pass
    try:
        from integration.napcat_api import NapCatAPI
        api = NapCatAPI(Path(root))
        login = api.call('get_login_info')
        bot_id = api.identity.get('bot_qq')
        login_id = login.get('user_id')
        if bot_id and login_id and str(login_id) == str(bot_id):
            online = api.call('get_status').get('online')
            if online is True:
                result['qq_connection'] = 'healthy'
            elif online is False:
                result['qq_connection'] = 'offline'
    except Exception:
        pass
    result.update(delivery_metrics(root))
    try:
        from token_hook import update as update_token_ledger
        result['token_ledger']=update_token_ledger(root)
    except (ImportError, OSError):
        result['token_ledger']=None
    result['model_name']=model_name(root)
    result['last_chat_observation']=last_chat_observation(root)
    check=periodic_observation(root)
    if check and (not result['last_chat_observation'] or check['finished_at']>result['last_chat_observation']['finished_at']):result['last_chat_observation']=check
    result['model_api']=model_evidence(result['last_chat_observation'],blocked=model_blocked(root))
    return result


if __name__ == '__main__':
    print(json.dumps(probe(Path.cwd())))
