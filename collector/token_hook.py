"""Keep a local, content-free ledger of reported usage from the passive monitor."""
import json
import re
import sqlite3
import time
from contextlib import closing
from pathlib import Path


def total_usage(raw):
    try:
        data=json.loads(raw)
        if not isinstance(data,dict) or not data: return None
        totals=[v.get('total') for v in data.values() if isinstance(v,dict)]
        if len(totals)!=len(data) or any(type(v) is not int or not 0<=v<=1000000000 for v in totals): return None
        return sum(totals)
    except (TypeError,ValueError): return None


def last_reply_interval(root,src,now):
    path=Path(root)/'.local/state-sync/deliveries.db'
    if not path.is_file(): return None
    try:
        with closing(sqlite3.connect(path.resolve().as_uri()+'?mode=ro',uri=True,timeout=2)) as db:
            groups={}
            for identifier,at in db.execute("SELECT id,at FROM deliveries WHERE at<=? AND at>=?",(now,now-86400)):
                match=re.fullmatch(r'(\d+):(?:transport|voice):\d+',identifier or '')
                if match and isinstance(at,(int,float)):
                    groups[match.group(1)]=max(at,groups.get(match.group(1),0))
        times=sorted(groups.values())
        if len(times)<2 or times[-1]<=times[-2]: return None
        rows=src.execute("SELECT m.usage FROM requests r LEFT JOIN request_metrics m ON r.id=m.request_id WHERE r.status='success' AND r.finished>? AND r.finished<=?",(times[-2],times[-1]))
        total=0
        for (raw,) in rows:
            value=total_usage(raw)
            if value is None:return None
            total+=value
        return total
    except (OSError,sqlite3.Error,TypeError,ValueError):return None


def update(root,now=None):
    now=time.time() if now is None else now
    source=Path(root)/'.local/health/request-diagnostics.db'
    if not source.is_file(): return None
    target=Path(root)/'.local/berry-hub-live/token-ledger.db'
    target.parent.mkdir(parents=True,exist_ok=True)
    try:
        with closing(sqlite3.connect(source.resolve().as_uri()+'?mode=ro',uri=True,timeout=2)) as src, closing(sqlite3.connect(target,timeout=2)) as dst, dst:
            dst.execute('CREATE TABLE IF NOT EXISTS meta(started REAL NOT NULL,last_scan REAL NOT NULL,gap INTEGER NOT NULL)')
            dst.execute('CREATE TABLE IF NOT EXISTS usage(request_id TEXT PRIMARY KEY,caller TEXT NOT NULL,finished REAL NOT NULL,tokens INTEGER)')
            row=dst.execute('SELECT started,last_scan,gap FROM meta').fetchone()
            if row is None:
                dst.execute('INSERT INTO meta VALUES(?,?,0)',(now,now));started=now;last_scan=now;gap=0
            else: started,last_scan,gap=row
            if now < last_scan or now-last_scan>13*86400:gap=1
            dst.execute('CREATE TABLE IF NOT EXISTS baseline_meta(id INTEGER PRIMARY KEY,created REAL NOT NULL)')
            dst.execute('CREATE TABLE IF NOT EXISTS baseline(caller TEXT PRIMARY KEY,reported INTEGER NOT NULL,missing INTEGER NOT NULL,tokens INTEGER NOT NULL)')
            if dst.execute('SELECT 1 FROM baseline_meta WHERE id=1').fetchone() is None:
                totals={}
                for caller,raw in src.execute("SELECT r.caller,m.usage FROM requests r LEFT JOIN request_metrics m ON r.id=m.request_id WHERE r.at<? AND r.status='success' AND r.finished<=?",(started,now)):
                    if caller not in ('chat','auxiliary','companion','avatar','relationship','image','recovery'):continue
                    item=totals.setdefault(caller,[0,0,0]);value=total_usage(raw)
                    if value is None:item[1]+=1
                    else:item[0]+=1;item[2]+=value
                for caller,(reported,missing_before,tokens) in totals.items():dst.execute('INSERT INTO baseline VALUES(?,?,?,?)',(caller,reported,missing_before,tokens))
                dst.execute('INSERT INTO baseline_meta VALUES(1,?)',(now,))
            rows=src.execute("SELECT r.id,r.caller,r.finished,m.usage FROM requests r LEFT JOIN request_metrics m ON r.id=m.request_id WHERE r.at>=? AND r.status='success' AND r.finished<=?",(started,now))
            for request_id,caller,finished,raw in rows:
                dst.execute('INSERT OR IGNORE INTO usage VALUES(?,?,?,?)',(request_id,caller,finished,total_usage(raw)))
            dst.execute('UPDATE meta SET last_scan=?,gap=?',(now,gap))
            recorded,missing,total=dst.execute('SELECT COUNT(tokens),COUNT(*)-COUNT(tokens),COALESCE(SUM(tokens),0) FROM usage').fetchone()
            latest=src.execute("SELECT r.finished,m.usage FROM requests r LEFT JOIN request_metrics m ON r.id=m.request_id WHERE r.caller='chat' AND r.status='success' AND r.finished<=? ORDER BY r.finished DESC LIMIT 1",(now,)).fetchone()
            prior=dst.execute('SELECT COALESCE(SUM(reported),0),COALESCE(SUM(missing),0),COALESCE(SUM(tokens),0) FROM baseline').fetchone()
            base_chat=dst.execute("SELECT reported,tokens FROM baseline WHERE caller='chat'").fetchone() or (0,0)
            live_chat=dst.execute("SELECT COUNT(tokens),COALESCE(SUM(tokens),0) FROM usage WHERE caller='chat'").fetchone()
            chat_count=base_chat[0]+live_chat[0];chat_total=base_chat[1]+live_chat[1]
            return {'started_at':started,'recorded_total':total,'recorded_requests':recorded,'missing_requests':missing,'gap':bool(gap),'latest_chat_tokens':total_usage(latest[1]) if latest else None,'latest_chat_finished_at':latest[0] if latest else None,'observed_total':prior[2]+total if prior[0]+recorded else None,'observed_requests':prior[0]+recorded,'observed_missing':prior[1]+missing,'chat_average':round(chat_total/chat_count) if chat_count else None,'chat_requests':chat_count,'round_tokens':last_reply_interval(root,src,now)}
    except (OSError,sqlite3.Error,TypeError,ValueError): return None
