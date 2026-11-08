import importlib.util
import json
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('berry_serial', Path(__file__).with_name('serial_protocol.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class SerialProtocolTests(unittest.TestCase):
    def test_only_allowlisted_fields_leave_host(self):
        src = {'generated_at':'2026-10-29T12:00:00Z', 'secret':'must-not-leave', 'codex':{'quota_windows':[{'remaining_percent':72,'label':'5 小时额度','resets_at':None,'token':'secret'}], 'recent_tasks':[{'title':'测试','status':'running','preview':'secret'}]}, 'zhiqing':{'components':{'qq_connection':'healthy','password':'secret'}},'dorm_pc':{'cpu':{'usage_percent':5,'password':'secret'}},'aliyun':{'storage':{'usage_percent':9,'password':'secret'}}}
        output = module.device_payload(src, now='2026-10-29T12:00:10Z')
        self.assertNotIn('secret',json.dumps(output,ensure_ascii=False))
        self.assertEqual(output['codex']['quota_windows'][0]['remaining_percent'],72)

    def test_stale_and_future_data_are_not_exported_as_live(self):
        src={'generated_at':'2026-10-29T11:00:00Z','codex':{'quota_windows':[{'remaining_percent':72}]}}
        self.assertEqual(module.device_payload(src,now='2026-10-29T12:00:00Z')['status'],'stale')
        self.assertNotIn('codex',module.device_payload(src,now='2026-10-29T12:00:00Z'))
        src['generated_at']='2026-10-30T12:00:00Z'
        self.assertEqual(module.device_payload(src,now='2026-10-29T12:00:00Z')['status'],'unknown')

    def test_desktop_task_status_expires_independently_of_quota(self):
        src={'generated_at':'2026-10-30T01:03:00Z','codex':{'updated_at':'2026-10-30T01:00:00Z','tasks_updated_at':'2026-10-30T01:02:30Z','recent_tasks':[{'title':'task','status':'completed'}],'tasks_status':'available'}}
        current=module.device_payload(src,now='2026-10-30T01:03:00Z');self.assertEqual(current['codex']['recent_tasks'][0]['status'],'completed')
        expired=module.device_payload(src,now='2026-10-30T01:05:00Z');self.assertEqual(expired['codex']['recent_tasks'][0]['status'],'unknown')

    def test_zhiqing_observed_tokens_are_bounded_and_never_fabricated(self):
        now='2026-10-30T01:00:00Z';src={'generated_at':now,'zhiqing':{'updated_at':now,'token_ledger':{'observed_total':3861446,'observed_requests':341,'observed_missing':174,'round_tokens':22765}}}
        z=module.device_payload(src,now=now)['zhiqing'];self.assertEqual(z['observed_tokens'],3861446);self.assertEqual(z['chat_request_average'],22765)
        src['zhiqing']['token_ledger']['round_tokens']=True;src['zhiqing']['token_ledger']['observed_total']=True
        z=module.device_payload(src,now=now)['zhiqing'];self.assertIsNone(z['observed_tokens']);self.assertIsNone(z['chat_request_average'])

    def test_suspicious_task_title_is_hidden(self):
        src={'generated_at':'2026-10-29T12:00:00Z','codex':{'recent_tasks':[{'title':'设置 API key secret-value','status':'running'}]}}
        result=module.device_payload(src,now='2026-10-29T12:00:01Z')
        self.assertEqual(result['codex']['recent_tasks'][0]['title'],'标题已隐藏')

    def test_one_stale_source_does_not_retain_old_metrics(self):
        src={'generated_at':'2026-10-29T12:00:00Z','codex':{'updated_at':'2026-10-29T11:00:00Z','quota_windows':[{'remaining_percent':72}]},'dorm_pc':{'updated_at':'2026-10-29T12:00:00Z','cpu':{'usage_percent':32}}}
        result=module.device_payload(src,now='2026-10-29T12:00:10Z')
        self.assertEqual(result['codex']['quota_windows'],[])
        self.assertEqual(result['dorm_pc']['cpu']['usage_percent'],32)

    def test_gpu_fast_sample_does_not_refresh_old_cpu_or_temperature(self):
        src={'generated_at':'2026-10-29T12:00:00Z','dorm_pc':{'updated_at':'2026-10-29T11:00:00Z','cpu':{'usage_percent':32},'gpu':{'usage_percent':17,'temperature_c':50,'usage_updated_at':'2026-10-29T12:00:00Z'}}}
        result=module.device_payload(src,now='2026-10-29T12:00:10Z')
        self.assertIsNone(result['dorm_pc']['cpu'])
        self.assertEqual(result['dorm_pc']['gpu']['usage_percent'],17)
        self.assertIsNone(result['dorm_pc']['gpu']['temperature_c'])
        src['dorm_pc']['gpu']['usage_percent']=0.16
        self.assertEqual(module.device_payload(src,now='2026-10-29T12:00:10Z')['dorm_pc']['gpu']['usage_percent'],0.16)

    def test_pc_uptime_comes_from_boot_timestamp(self):
        src={'generated_at':'2026-10-29T12:00:00Z','dorm_pc':{'boot_at':'2026-10-28T12:00:00Z'}}
        self.assertEqual(module.device_payload(src,now='2026-10-29T12:00:10Z')['dorm_pc']['uptime_seconds'],86410)

    def test_zhiqing_reply_age_advances_and_model_is_validated(self):
        src={'generated_at':'2026-10-29T12:00:00Z','zhiqing':{'updated_at':'2026-10-29T12:00:00Z','model_name':'gpt-6.1-sol','last_reply_seconds_ago':120,'today_confirmed_rounds':3}}
        z=module.device_payload(src,now='2026-10-29T12:00:10Z')['zhiqing']
        self.assertEqual(z['last_reply_seconds_ago'],130);self.assertEqual(z['model_name'],'gpt-6.1-sol');self.assertEqual(z['today_confirmed_rounds'],3)

    def test_cloud_sample_time_is_only_exported_for_fresh_valid_sample(self):
        src={'generated_at':'2026-10-29T12:00:00Z','aliyun':{'updated_at':'2026-10-29T11:59:00Z','status':'available','upload_bytes_per_second':50}}
        result=module.device_payload(src,now='2026-10-29T12:00:10Z')
        self.assertEqual(result['aliyun']['updated_at'],'2026-10-29T11:59:00Z')
        src['aliyun']['updated_at']='2026-10-29T11:00:00Z'
        result=module.device_payload(src,now='2026-10-29T12:00:10Z')
        self.assertIsNone(result['aliyun']['updated_at'])
        self.assertIsNone(result['aliyun']['upload_bytes_per_second'])

    def test_schedule_exports_confirmed_week_and_current_day(self):
        src={'generated_at':'2026-10-29T12:00:00Z','schedule':{'status':'available','courses':[[4,'10:10','课程','地点'],[5,'14:00','其他','地点']]}}
        result=module.device_payload(src,now='2026-10-29T12:00:01Z')
        self.assertEqual(len(result['schedule']['courses']),2)
        self.assertEqual(result['schedule']['day'],4)
        self.assertEqual(result['schedule']['week_dates'],[26,27,28,29,30,31,1])
        self.assertEqual(result['schedule']['courses'][0]['title'],'课程')

    def test_weather_exports_only_readings(self):
        src={'generated_at':'2026-10-29T12:00:00Z','daily':{'status':'available','source':'moji-xiangan','weather':{'status':'available','location':'翔安','temperature_c':26,'code':3,'password':'secret','forecast':[]}}}
        result=module.device_payload(src,now='2026-10-29T12:00:01Z')
        self.assertEqual(result['weather']['temperature_c'],26)
        self.assertNotIn('secret',json.dumps(result,ensure_ascii=False))
        src['daily']['source']='open-meteo'
        self.assertEqual(module.device_payload(src,now='2026-10-29T12:00:01Z')['weather']['status'],'unknown')

    def test_air_quality_has_independent_expiry(self):
        src={'generated_at':'2026-10-29T12:00:00Z','daily':{'source':'moji-xiangan','weather':{'status':'available','air_quality':{'aqi':36,'description':'优','updated_at':'2026-10-29T11:00:00Z'}}}}
        self.assertEqual(module.device_payload(src,now='2026-10-29T12:00:01Z')['weather']['air_quality'],{'aqi':36,'description':'优'})
        src['daily']['weather']['air_quality']['updated_at']='2026-10-29T08:00:00Z'
        self.assertTrue(module.device_payload(src,now='2026-10-29T12:00:01Z')['weather']['air_quality']['stale'])
        src['daily']['weather']['air_quality']['updated_at']='2026-10-28T08:00:00Z'
        self.assertIsNone(module.device_payload(src,now='2026-10-29T12:00:01Z')['weather']['air_quality'])

    def test_weather_uv_and_warning_are_allowlisted(self):
        src={'generated_at':'2026-10-29T12:00:00Z','daily':{'source':'moji-xiangan','weather':{'status':'available','uv_text':'中等','warnings':'none'}}}
        weather=module.device_payload(src,now='2026-10-29T12:00:01Z')['weather']
        self.assertEqual(weather['uv_text'],'中等');self.assertEqual(weather['warnings'],'none')
        src['daily']['weather']['warnings']='invalid'
        self.assertEqual(module.device_payload(src,now='2026-10-29T12:00:01Z')['weather']['warnings'],'unknown')

    def test_frame_has_bounded_payload_and_checksum(self):
        data={'status':'unknown'}
        frame=module.make_frame(data)
        self.assertTrue(frame.startswith(b'BH1 '))
        self.assertLessEqual(len(frame),8192)
        with self.assertRaises(ValueError): module.make_frame({'x':'a'*8192})

    def test_long_titles_fit_without_dropping_weather_or_schedule(self):
        stamp='2026-10-29T12:00:00Z'
        src={'generated_at':stamp,'codex':{'recent_tasks':[{'title':'任务'*30,'status':'running'} for _ in range(4)]},'schedule':{'status':'available','courses':[[1,'10:10', '课程'*30,'地点'*40] for _ in range(24)]},'daily':{'source':'moji-xiangan','weather':{'status':'available','temperature_c':26}}}
        payload=module.device_payload(src,now='2026-10-29T12:00:01Z')
        frame=module.make_device_frame(payload)
        self.assertLessEqual(len(frame),8192)
        self.assertEqual(payload['weather']['temperature_c'],26)
        self.assertEqual(len(payload['schedule']['courses']),24)


if __name__ == '__main__':
    unittest.main()
