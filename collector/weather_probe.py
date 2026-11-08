"""Read Xiang'an observations and forecasts from Moji's public pages."""
import json
import re
from datetime import datetime, timedelta, timezone
from urllib.request import Request, urlopen
from bs4 import BeautifulSoup

ZONE=timezone(timedelta(hours=8))
BASE='https://tianqi.moji.com'
PLACE='/china/fujian/xiang%27an-district'
URL_CURRENT=BASE+'/weather'+PLACE
URL_FORECAST=BASE+'/forecast15'+PLACE
URL_AIR=BASE+'/aqi'+PLACE

def parse_air(html,now):
    page=BeautifulSoup(html,'html.parser')
    if not page.title or '翔安区' not in page.title.get_text(): return None
    value=page.select_one('#aqi_value'); description=page.select_one('#aqi_desc'); stamp=page.select_one('.aqi_info_time')
    match=re.search(r'(\d{4})年(\d{1,2})月(\d{1,2})日\s*(\d{1,2})时(\d{1,2})分',stamp.get_text(' ',strip=True)) if stamp else None
    try:
        updated=datetime(*map(int,match.groups()),tzinfo=ZONE) if match else None
        aqi=int(value.get_text(strip=True)) if value else None
    except (ValueError,TypeError): return None
    if updated is None or not 0 <= (now-updated).total_seconds() <= 86400 or aqi is None or not 0<=aqi<=500: return None
    label=description.get_text(strip=True) if description else ''
    if label not in ('优','良','轻度污染','中度污染','重度污染','严重污染'): return None
    return {'aqi':aqi,'description':label,'updated_at':updated.isoformat()}

def code(condition):
    if condition == '晴': return 0
    if condition in ('多云','晴间多云'): return 2
    if condition == '阴': return 3
    if '雾' in condition: return 45
    if '雷' in condition: return 95
    if '雪' in condition: return 71
    if '阵雨' in condition: return 80
    if '雨' in condition: return 61
    return None

def parse(current_html,forecast_html,now):
    result={'schema_version':2,'status':'unknown','location':'翔安','forecast':[]}
    current=BeautifulSoup(current_html,'html.parser')
    forecast=BeautifulSoup(forecast_html,'html.parser')
    observed=current.select_one('.wea_weather')
    date_text=forecast.select_one('.detail_time')
    date_match=re.search(r'(\d{4})年(\d{1,2})月(\d{1,2})日',date_text.get_text(' ',strip=True)) if date_text else None
    try:
        page_date=datetime(*map(int,date_match.groups()),tzinfo=ZONE).date() if date_match else None
    except ValueError:
        page_date=None
    if observed and page_date==now.date():
        temp=observed.select_one('em'); condition=observed.select_one('b'); update=observed.select_one('.info_uptime')
        match=re.search(r'今天\s*(\d{1,2}):(\d{2})更新',update.get_text(' ',strip=True)) if update else None
        try:
            updated=now.replace(hour=int(match[1]),minute=int(match[2]),second=0,microsecond=0) if match else None
            value=int(temp.get_text(strip=True)) if temp else None
        except (ValueError,TypeError):
            updated=value=None
        if updated and 0 <= (now-updated).total_seconds() <= 7200 and value is not None and -60<=value<=60:
            result.update(status='available',temperature_c=value,code=code(condition.get_text(strip=True) if condition else ''),updated_at=updated.isoformat())
            for entry in current.select('#live_index dl'):
                name=entry.select_one('dd');level=entry.select_one('dt')
                if name and level and name.get_text(strip=True)=='紫外线':
                    text=level.get_text(strip=True)
                    if text in ('弱','最弱','中等','强','很强','极强'):result['uv_text']=text
                    break
            alert=current.select_one('.wea_alert ul')
            if alert:
                links=alert.select('li a')
                other=[a for a in links if '/aqi/' not in a.get('href','')]
                if any('预警' in a.get_text(' ',strip=True) or 'warning' in a.get('href','').lower() for a in other):result['warnings']='present'
                elif links and not other:result['warnings']='none'
            about=current.select_one('.wea_about')
            if about:
                humidity=re.search(r'湿度\s*(\d{1,3})%',about.get_text(' ',strip=True))
                result['humidity_percent']=int(humidity[1]) if humidity and int(humidity[1])<=100 else None
                wind=about.select_one('em')
                result['wind_text']=wind.get_text(' ',strip=True)[:20] if wind else None
    if page_date==now.date():
        active=forecast.select_one('.wea_list li.active')
        entries=[active]+list(active.find_next_siblings('li')) if active else []
        for index,entry in enumerate(entries[:8]):
            dates=entry.select('.week'); date=dates[-1].get_text(strip=True) if dates else ''
            target=now.date()+timedelta(days=index)
            if date != target.strftime('%m/%d'): break
            title=entry.select_one('.wea'); high=entry.select_one('.tree b'); low=entry.select_one('.tree strong')
            def value(element):
                match=re.fullmatch(r'(-?\d{1,2})\s*°',element.get_text(strip=True)) if element else None
                return int(match[1]) if match and -60<=int(match[1])<=60 else None
            result['forecast'].append({'date':target.isoformat(),'high':value(high),'low':value(low),'code':code(title.get_text(strip=True) if title else ''),'rain_probability':None})
    return result

def probe():
    try:
        def read(url):
            with urlopen(Request(url,headers={'User-Agent':'Mozilla/5.0'}),timeout=12) as response:
                return response.read(1024*1024).decode('utf8')
        now=datetime.now(ZONE)
        result=parse(read(URL_CURRENT),read(URL_FORECAST),now)
        try: result['air_quality']=parse_air(read(URL_AIR),now)
        except Exception: result['air_quality']=None
        return result
    except Exception:
        return {'status':'unknown','location':'翔安','forecast':[]}

if __name__=='__main__': print(json.dumps(probe()))
