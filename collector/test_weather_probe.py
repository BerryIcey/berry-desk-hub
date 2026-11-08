import importlib.util
import unittest
from datetime import datetime, timezone, timedelta
from pathlib import Path
spec=importlib.util.spec_from_file_location('weather',Path(__file__).with_name('weather_probe.py'))
weather=importlib.util.module_from_spec(spec)
spec.loader.exec_module(weather)
class WeatherTests(unittest.TestCase):
    def test_daily_uv_and_warning_panel_are_only_used_for_valid_current_page(self):
        now=datetime(2026,10,29,20,30,tzinfo=timezone(timedelta(hours=8)))
        current='<div class="wea_weather"><em>22</em><b>晴</b><strong class="info_uptime">今天20:24更新</strong></div><div id="live_index"><dl><dt>中等</dt><dd>紫外线</dd></dl></div><div class="wea_alert"><ul><li><a href="/aqi/x">33 优</a></li></ul></div>'
        forecast='<div class="detail_time">2026年10月29日</div>'
        result=weather.parse(current,forecast,now)
        self.assertEqual(result['uv_text'],'中等')
        self.assertEqual(result['warnings'],'none')
        alert=current.replace('</ul></div>','<li><a href="/warning/x">台风预警</a></li></ul></div>')
        self.assertEqual(weather.parse(alert,forecast,now)['warnings'],'present')
        self.assertNotIn('warnings',weather.parse(current.replace('20:24','08:24'),forecast,now))
    def test_air_quality_requires_xiangan_and_fresh_observation(self):
        now=datetime(2026,10,29,20,30,tzinfo=timezone(timedelta(hours=8)))
        html='<title>翔安区空气质量</title><em id="aqi_value">36</em><span id="aqi_desc">优</span><div class="aqi_info_time">发布日期：2026年10月29日 20时00分</div>'
        self.assertEqual(weather.parse_air(html,now)['aqi'],36)
        self.assertEqual(weather.parse_air(html.replace('20时00分','08时00分'),now)['aqi'],36)
        self.assertIsNone(weather.parse_air(html.replace('29日 20时00分','28日 08时00分'),now))
        self.assertIsNone(weather.parse_air(html.replace('翔安区','厦门市'),now))
    def test_moji_current_and_future(self):
        current='<div class="wea_weather"><em>22</em><b>多云</b><strong class="info_uptime">今天20:24更新</strong></div><div class="wea_about"><span>湿度 82%</span><em>东风2级</em></div>'
        forecast='<div class="detail_time">2026年10月29日</div><div class="detail_future_title"><span>08:10更新</span></div><div class="wea_list"><ul><li class="active"><span class="wea">晴</span><div class="tree"><b>27°</b><strong>21°</strong></div><span class="week">10/29</span></li><li><span class="wea">多云</span><div class="tree"><b>28°</b><strong>22°</strong></div><span class="week">10/30</span></li></ul></div>'
        now=datetime(2026,10,29,20,30,tzinfo=timezone(timedelta(hours=8)))
        result=weather.parse(current,forecast,now)
        self.assertEqual(result['temperature_c'],22)
        self.assertEqual(result['humidity_percent'],82)
        self.assertEqual(result['location'],'翔安')
        self.assertEqual(result['forecast'][1]['date'],'2026-10-30')
        self.assertIsNone(result['forecast'][0]['rain_probability'])
    def test_missing_or_stale_observation_is_unknown(self):
        now=datetime(2026,10,29,20,30,tzinfo=timezone(timedelta(hours=8)))
        self.assertEqual(weather.parse('<html></html>','',now)['status'],'unknown')
        stale='<div class="wea_weather"><em>22</em><strong class="info_uptime">今天08:00更新</strong></div>'
        self.assertEqual(weather.parse(stale,'',now)['status'],'unknown')
if __name__=='__main__': unittest.main()
