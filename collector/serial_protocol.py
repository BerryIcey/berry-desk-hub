"""Minimal device payload and checksum; never include host credentials."""
import datetime as dt
import json
import re
import zlib


def device_payload(snapshot, *, now=None, max_age_seconds=600):
    def when(value):
        try:
            parsed = dt.datetime.fromisoformat(value.replace('Z', '+00:00'))
            return parsed.timestamp() if parsed.tzinfo else None
        except (AttributeError, ValueError, OverflowError):
            return None
    stamp = when(snapshot.get('generated_at'))
    current = when(now) if now is not None else dt.datetime.now(dt.timezone.utc).timestamp()
    if stamp is None or current is None or stamp > current:
        return {'status':'unknown'}
    if current-stamp > max_age_seconds:
        return {'status':'stale'}
    out = {'status':'fresh', 'generated_at':snapshot['generated_at']}
    def fresh_section(name, age=max_age_seconds):
        section=snapshot.get(name) or {}
        if not isinstance(section,dict): return {}
        source_time=when(section.get('updated_at',snapshot.get('generated_at')))
        return section if source_time is not None and 0<=current-source_time<=age else {}
    codex = fresh_section('codex')
    windows=[]
    for i,item in enumerate((codex.get('quota_windows') or [])[:2]):
        value=item.get('remaining_percent')
        value=value if isinstance(value,(int,float)) and not isinstance(value,bool) and 0<=value<=100 else None
        reset=item.get('resets_at')
        windows.append({'label':('5 小时额度','周额度')[i],'remaining_percent':value,'resets_at':reset if when(reset) is not None else None})
    raw_codex=snapshot.get('codex') if isinstance(snapshot.get('codex'),dict) else {}
    task_section=raw_codex if 'tasks_updated_at' in raw_codex else codex
    task_time=when(task_section.get('tasks_updated_at'))
    task_fresh=(task_time is not None and 0<=current-task_time<=120) if 'tasks_updated_at' in task_section else bool(codex)
    tasks=[]
    for item in (task_section.get('recent_tasks') or [])[:4]:
        title=item.get('title')
        if isinstance(title,str) and re.search(r'(?i)api[ _-]*key|password|secret|token|credential|密码|密钥|口令|凭据|[A-Za-z0-9_-]{32,}',title):
            title='标题已隐藏'
        status=item.get('status')
        tasks.append({'title':''.join(list(title)[:60]) if isinstance(title,str) else '', 'status':status if task_fresh and status in ('running','waiting_approval','blocked','error','unknown','idle','completed','interrupted') else 'unknown'})
        notice_key=item.get('notice_key');notice_state=item.get('notice_state')
        if task_fresh and isinstance(notice_key,str) and re.fullmatch(r'[0-9a-f]{32}',notice_key) and notice_state in ('running','waiting_approval','blocked','completed','error','interrupted','idle','unknown'):
            tasks[-1].update(notice_key=notice_key,notice_state=notice_state)
    out['codex']={'quota_windows':windows,'recent_tasks':tasks,'tasks_status':'available' if task_fresh and task_section.get('tasks_status')=='available' else 'unknown'}
    if task_fresh and task_time is not None:out['codex']['tasks_updated_at']=task_section['tasks_updated_at']
    def number(value):
        return value if isinstance(value,(int,float)) and not isinstance(value,bool) and 0<=value<1e16 else None
    def metric(value, keys):
        return {key:number(value.get(key)) for key in keys} if isinstance(value,dict) else None
    pc=fresh_section('dorm_pc')
    boot=pc.get('boot_at')
    out['dorm_pc']={'cpu':metric(pc.get('cpu'),('usage_percent','temperature_c')), 'gpu':metric(pc.get('gpu'),('usage_percent','temperature_c')), 'memory':metric(pc.get('memory'),('usage_percent','used_bytes','total_bytes')), 'boot_at':boot if when(boot) is not None else None}
    if pc:out['dorm_pc']['updated_at']=pc.get('updated_at',snapshot['generated_at'])
    raw_pc=snapshot.get('dorm_pc') or {}
    raw_gpu=raw_pc.get('gpu') if isinstance(raw_pc,dict) else None
    if isinstance(raw_gpu,dict) and 'usage_updated_at' in raw_gpu:
        gpu_time=when(raw_gpu.get('usage_updated_at'))
        gpu_usage=number(raw_gpu.get('usage_percent')) if gpu_time is not None and 0<=current-gpu_time<=600 else None
        if out['dorm_pc']['gpu'] is None:out['dorm_pc']['gpu']={'usage_percent':None,'temperature_c':None}
        out['dorm_pc']['gpu']['usage_percent']=gpu_usage
        if gpu_usage is not None:out['dorm_pc']['gpu']['usage_updated_at']=raw_gpu['usage_updated_at']
    boot_time=when(boot)
    out['dorm_pc']['uptime_seconds']=int(current-boot_time) if boot_time is not None and boot_time<=current else None
    z=fresh_section('zhiqing')
    components=z.get('components') or {}
    model=z.get('model_name');last=z.get('last_reply_seconds_ago');z_time=when(z.get('updated_at'))
    rounds=z.get('today_confirmed_rounds')
    ledger=z.get('token_ledger') if isinstance(z.get('token_ledger'),dict) else {}
    def token_value(key,count):
        value=ledger.get(key);n=ledger.get(count)
        return value if type(value) is int and 0<=value<=1000000000000 and type(n) is int and n>0 else None
    out['zhiqing']={'observed_tokens':token_value('observed_total','observed_requests'),'chat_request_average':(ledger.get('round_tokens') if type(ledger.get('round_tokens')) is int and 0<=ledger['round_tokens']<=1000000000000 else None),'components':{key:components.get(key) if components.get(key) in ('healthy','offline','unknown') else 'unknown' for key in ('process','qq_connection','model_api','message_processing')},
        'model_name':model if isinstance(model,str) and re.fullmatch(r'[A-Za-z0-9._-]{1,40}',model) else None,
        'last_reply_seconds_ago':int(last+current-z_time) if type(last) is int and 0<=last<=86400 and z_time is not None and 0<=current-z_time<=600 else None,
        'today_confirmed_rounds':rounds if type(rounds) is int and 0<=rounds<=100000 and z_time is not None and dt.datetime.fromtimestamp(z_time,dt.timezone(dt.timedelta(hours=8))).date()==dt.datetime.fromtimestamp(current,dt.timezone(dt.timedelta(hours=8))).date() else None}
    cloud=fresh_section('aliyun')
    out['aliyun']={key:number(cloud.get(key)) for key in ('uptime_seconds','latency_ms','download_bytes_per_second','upload_bytes_per_second')}
    out['aliyun']['status']='available' if cloud.get('status')=='available' else 'unknown'
    out['aliyun']['relay_status']='healthy' if cloud.get('relay_status')=='healthy' else 'unknown'
    cloud_updated=cloud.get('updated_at')
    out['aliyun']['updated_at']=cloud_updated if when(cloud_updated) is not None else None
    for key in ('cpu','memory','storage'):
        out['aliyun'][key]=metric(cloud.get(key),('usage_percent','used_bytes','total_bytes'))
    schedule=fresh_section('schedule')
    local_date=dt.datetime.fromtimestamp(current,dt.timezone(dt.timedelta(hours=8))).date()
    today=local_date.isoweekday()
    monday=local_date-dt.timedelta(days=today-1)
    rows=[]
    for row in (schedule.get('courses') or [])[:24]:
        if isinstance(row,list) and len(row)==4 and type(row[0]) is int and 1<=row[0]<=7 and all(isinstance(v,str) for v in row[1:]):
            rows.append({'day':row[0],'time':row[1][:32],'title':row[2][:60],'location':row[3][:80]})
    out['schedule']={'status':'available' if schedule.get('status')=='available' else 'unknown','day':today,'week_dates':[(monday+dt.timedelta(days=i)).day for i in range(7)],'courses':rows}
    daily=fresh_section('daily',7200)
    weather=(daily.get('weather') or {}) if daily.get('source')=='moji-xiangan' else {}
    def weather_number(value,low,high):
        return value if type(value) in (int,float) and low<=value<=high else None
    wind=weather.get('wind_text')
    out['weather']={'status':'available' if weather.get('status')=='available' else 'unknown','location':'翔安',
                    'temperature_c':weather_number(weather.get('temperature_c'),-60,60),
                    'code':weather_number(weather.get('code'),0,99),
                    'humidity_percent':weather_number(weather.get('humidity_percent'),0,100),
                    'wind_kmh':None,'wind_text':wind if isinstance(wind,str) and re.fullmatch(r'[东南西北无风微静转偏\d级]{1,20}',wind) else None,'forecast':[]}
    air=weather.get('air_quality')
    air_time=when(air.get('updated_at')) if isinstance(air,dict) else None
    air_value=air.get('aqi') if isinstance(air,dict) else None
    air_label=air.get('description') if isinstance(air,dict) else None
    out['weather']['air_quality']={'aqi':air_value,'description':air_label,**({'stale':True} if current-air_time>10800 else {})} if air_time is not None and 0<=current-air_time<=86400 and type(air_value) is int and 0<=air_value<=500 and air_label in ('优','良','轻度污染','中度污染','重度污染','严重污染') else None
    uv=weather.get('uv_text');warning=weather.get('warnings')
    out['weather']['uv_text']=uv if uv in ('弱','最弱','中等','强','很强','极强') else None
    out['weather']['warnings']=warning if warning in ('none','present') else 'unknown'
    for day in (weather.get('forecast') or [])[:8]:
        if not isinstance(day,dict): continue
        date=day.get('date')
        if not isinstance(date,str) or not re.fullmatch(r'\d{4}-\d{2}-\d{2}',date): continue
        out['weather']['forecast'].append({'date':date,'high':weather_number(day.get('high'),-60,60),'low':weather_number(day.get('low'),-60,60),'code':weather_number(day.get('code'),0,99),'rain_probability':weather_number(day.get('rain_probability'),0,100)})
    return out


def make_frame(payload):
    body=json.dumps(payload,ensure_ascii=False,separators=(',',':'),allow_nan=False).encode('utf8')
    checksum=zlib.crc32(body)
    frame=b'BH1 '+format(checksum,'08x').encode()+b' '+body+b'\n'
    if len(frame)>8192:
        raise ValueError('Payload exceeds device buffer')
    return frame


def make_device_frame(payload):
    """Fit longer display text to the device buffer without mutating the source."""
    try:
        return make_frame(payload)
    except ValueError:
        pass
    compact=json.loads(json.dumps(payload,ensure_ascii=False,allow_nan=False))
    for limit in (30,15,8):
        for task in compact.get('codex',{}).get('recent_tasks',[]):
            if isinstance(task.get('title'),str) and len(task['title'])>limit:
                task['title']=task['title'][:limit-1]+'…'
        for course in compact.get('schedule',{}).get('courses',[]):
            for key in ('title','location'):
                if isinstance(course.get(key),str) and len(course[key])>limit:
                    course[key]=course[key][:limit-1]+'…'
        try:
            return make_frame(compact)
        except ValueError:
            continue
    raise ValueError('Payload exceeds device buffer after text compaction')
