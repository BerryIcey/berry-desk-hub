import importlib.util
import unittest
import tempfile
import sqlite3
from pathlib import Path
spec=importlib.util.spec_from_file_location('hub_store',Path(__file__).with_name('hub_store.py'))
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

class HubStoreTests(unittest.TestCase):
    def test_hourly_weather_has_refresh_grace_then_expires(self):
        observed='2026-10-30T00:00:00Z';later='2026-10-30T01:10:00Z'
        snapshot={'generated_at':later,'daily':{'updated_at':observed,'source':'moji-xiangan','weather':{'status':'available','temperature_c':25}}}
        store=module.HubStore();self.assertTrue(store.update('daily',snapshot,now=later))
        self.assertEqual(store.read('daily',now=later)['status'],'fresh')
        self.assertEqual(store.read('daily',now='2026-10-30T02:00:01Z')['status'],'stale')
    def test_only_safe_data_and_original_timestamp(self):
        store=module.HubStore()
        snap={'generated_at':'2026-10-30T01:00:00Z','secret':'PRIVATE','codex':{'updated_at':'2026-10-30T01:00:00Z','recent_tasks':[{'title':'password PRIVATE TITLE','status':'running'}],'quota_windows':[]}}
        self.assertTrue(store.update('codex',snap,now='2026-10-30T01:00:01Z'))
        saved=store.read('codex',now='2026-10-30T01:01:00Z')
        self.assertNotIn('PRIVATE',str(saved));self.assertEqual(saved['updated_at'],'2026-10-30T01:00:00Z')
        self.assertEqual(saved['status'],'fresh');self.assertEqual(store.read('codex',now='2026-10-30T01:11:00Z')['status'],'stale')
    def test_task_titles_match_screen_but_known_secret_labels_are_hidden(self):
        now='2026-10-30T01:00:01Z'
        snapshot={'generated_at':now,'codex':{'updated_at':now,'recent_tasks':[{'title':'整理项目','status':'running'},{'title':'password: example','status':'error'}]}}
        envelope=module.source_envelope('codex',snapshot,now=now);store=module.HubStore();self.assertTrue(store.update('codex',envelope,now=now))
        rows=store.read('codex',now=now)['data']['recent_tasks']
        self.assertEqual(rows[0]['title'],'整理项目');self.assertEqual(rows[1]['title'],'标题已隐藏')

    def test_token_ledger_is_allowlisted_and_empty_is_not_reported_as_zero(self):
        store=module.HubStore();now='2026-10-30T01:00:01Z'
        snap={'generated_at':now,'zhiqing':{'updated_at':now,'token_ledger':{'started_at':1793320000,'recorded_total':0,'recorded_requests':0,'missing_requests':0,'gap':False,'latest_chat_tokens':None,'secret':'PRIVATE'}}}
        self.assertTrue(store.update('zhiqing',snap,now=now));ledger=store.read('zhiqing',now=now)['data']['token_ledger']
        self.assertIsNone(ledger['recorded_total']);self.assertNotIn('secret',ledger)

    def test_token_totals_are_transferred_with_their_coverage(self):
        now='2026-10-30T01:00:01Z';snap={'generated_at':now,'zhiqing':{'updated_at':now,'token_ledger':{'started_at':1793320000,'recorded_requests':0,'observed_requests':10,'observed_total':1200,'observed_missing':2,'chat_requests':2,'chat_average':100}}}
        envelope=module.source_envelope('zhiqing',snap,now=now);ledger=envelope['zhiqing']['token_ledger']
        self.assertEqual(ledger['observed_total'],1200);self.assertEqual(ledger['chat_average'],100);self.assertEqual(ledger['observed_missing'],2)

    def test_outbound_envelope_contains_only_the_chosen_source(self):
        now='2026-10-30T01:00:01Z'
        snap={'generated_at':now,'secret':'PRIVATE','zhiqing':{'updated_at':now,'components':{'qq_connection':'healthy','secret':'PRIVATE'}},'codex':{'secret':'PRIVATE'}}
        envelope=module.source_envelope('zhiqing',snap,now=now)
        self.assertNotIn('PRIVATE',str(envelope));self.assertNotIn('codex',envelope)
        self.assertEqual(envelope['zhiqing']['components']['qq_connection'],'healthy')
        store=module.HubStore();self.assertTrue(store.update('zhiqing',envelope,now=now))

    def test_reply_age_is_not_double_counted_and_advances(self):
        observed='2026-10-30T01:00:00Z'; now='2026-10-30T01:00:30Z'
        snap={'generated_at':observed,'zhiqing':{'updated_at':observed,'last_reply_seconds_ago':120}}
        envelope=module.source_envelope('zhiqing',snap,now=now)
        self.assertEqual(envelope['zhiqing']['last_reply_seconds_ago'],120)
        store=module.HubStore();self.assertTrue(store.update('zhiqing',envelope,now=now))
        self.assertEqual(store.read('zhiqing',now='2026-10-30T01:01:00Z')['data']['last_reply_seconds_ago'],180)

    def test_last_chat_observation_is_filtered(self):
        now='2026-10-30T01:00:01Z'
        snap={'generated_at':now,'zhiqing':{'updated_at':now,'last_chat_observation':{'finished_at':1793320000,'outcome':'success','category':'none','secret':'PRIVATE'}}}
        envelope=module.source_envelope('zhiqing',snap,now=now)
        obs=envelope['zhiqing']['last_chat_observation'];self.assertEqual(obs['outcome'],'success');self.assertNotIn('secret',obs)

    def test_fast_gpu_updates_without_refreshing_cpu_time(self):
        observed='2026-10-30T01:00:00Z';now='2026-10-30T01:00:30Z'
        snap={'generated_at':now,'dorm_pc':{'updated_at':observed,'cpu':{'usage_percent':10},'gpu':{'usage_percent':1,'usage_updated_at':now}}}
        store=module.HubStore();envelope=module.source_envelope('dorm_pc',snap,now=now)
        self.assertTrue(store.update('dorm_pc',envelope,now=now))
        newer='2026-10-30T01:01:00Z';snap['generated_at']=newer;snap['dorm_pc']['cpu']['usage_percent']=99;snap['dorm_pc']['gpu'].update(usage_percent=7,usage_updated_at=newer)
        envelope=module.source_envelope('dorm_pc',snap,now=newer)
        self.assertTrue(store.update('dorm_pc',envelope,now=newer))
        state=store.read('dorm_pc',now=newer);self.assertEqual(state['updated_at'],observed)
        self.assertEqual(state['data']['cpu']['usage_percent'],10);self.assertEqual(state['data']['gpu']['usage_percent'],7)

    def test_repeat_gpu_snapshot_does_not_fail_when_uptime_advances(self):
        observed='2026-10-30T01:00:00Z';first='2026-10-30T01:00:10Z';second='2026-10-30T01:00:20Z'
        snap={'generated_at':first,'dorm_pc':{'updated_at':observed,'boot_at':'2026-10-29T01:00:00Z','cpu':{'usage_percent':10},'gpu':{'usage_percent':5,'usage_updated_at':first}}}
        store=module.HubStore();self.assertTrue(store.update('dorm_pc',module.source_envelope('dorm_pc',snap,now=first),now=first))
        snap['generated_at']=second
        self.assertTrue(store.update('dorm_pc',module.source_envelope('dorm_pc',snap,now=second),now=second))
        snap['dorm_pc']['cpu']['usage_percent']=99
        self.assertFalse(store.update('dorm_pc',module.source_envelope('dorm_pc',snap,now=second),now=second))
        self.assertEqual(store.read('dorm_pc',now=second)['data']['cpu']['usage_percent'],10)

    def test_fast_tasks_update_and_expire_without_refreshing_quota(self):
        observed='2026-10-30T01:00:00Z'; first='2026-10-30T01:00:30Z'; second='2026-10-30T01:01:00Z'
        snap={'generated_at':first,'codex':{'updated_at':observed,'tasks_updated_at':first,'tasks_status':'available','quota_windows':[{'remaining_percent':50}],'recent_tasks':[{'title':'password PRIVATE','status':'running'}]}}
        store=module.HubStore();self.assertTrue(store.update('codex',module.source_envelope('codex',snap,now=first),now=first))
        snap['generated_at']=second;snap['codex']['tasks_updated_at']=second;snap['codex']['quota_windows'][0]['remaining_percent']=99;snap['codex']['recent_tasks'][0]['status']='waiting_approval'
        self.assertTrue(store.update('codex',module.source_envelope('codex',snap,now=second),now=second))
        value=store.read('codex',now=second);self.assertEqual(value['updated_at'],observed)
        self.assertEqual(value['data']['quota_windows'][0]['remaining_percent'],50)
        self.assertEqual(value['data']['recent_tasks'][0]['status'],'waiting_approval')
        self.assertNotIn('PRIVATE',str(value))
        expired=store.read('codex',now='2026-10-30T01:03:01Z')['data']
        self.assertEqual(expired['tasks_status'],'unknown');self.assertEqual(expired['recent_tasks'][0]['status'],'unknown')

    def test_reply_count_resets_to_unknown_at_midnight(self):
        observed='2026-10-30T15:59:59Z';now='2026-10-30T16:00:01Z'
        snap={'generated_at':observed,'zhiqing':{'updated_at':observed,'today_confirmed_rounds':3}}
        store=module.HubStore();self.assertTrue(store.update('zhiqing',snap,now=observed))
        self.assertIsNone(store.read('zhiqing',now=now)['data']['today_confirmed_rounds'])

    def test_restart_does_not_fabricate_a_current_value(self):
        now='2026-10-30T01:00:01Z';one=module.HubStore()
        self.assertTrue(one.update('codex',{'generated_at':now,'codex':{'updated_at':now,'quota_windows':[]}},now=now))
        restarted=module.HubStore().read('codex',now=now)
        self.assertEqual(restarted,{'status':'unknown','updated_at':None,'data':None})

    def test_persistent_restart_retains_time_and_expires(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/'hub.db';now='2026-10-30T01:00:01Z'
            one=module.HubStore(path);snap={'generated_at':now,'codex':{'updated_at':now,'recent_tasks':[{'title':'password PRIVATE','status':'running'}]}}
            self.assertTrue(one.update('codex',snap,now=now))
            restored=module.HubStore(path).read('codex',now='2026-10-30T01:11:01Z')
            self.assertEqual(restored['status'],'stale');self.assertEqual(restored['updated_at'],now);self.assertNotIn('PRIVATE',str(restored))

    def test_air_quality_expires_on_its_own_timestamp(self):
        observed='2026-10-30T02:00:00Z';now='2026-10-30T02:00:01Z'
        snap={'generated_at':observed,'daily':{'updated_at':observed,'source':'moji-xiangan','weather':{'status':'available','air_quality':{'aqi':41,'description':'优','updated_at':'2026-10-29T23:00:30Z'}}}}
        store=module.HubStore();self.assertTrue(store.update('daily',snap,now=now))
        self.assertIsNotNone(store.read('daily',now=now)['data']['air_quality'])
        self.assertTrue(store.read('daily',now='2026-10-30T02:00:31Z')['data']['air_quality']['stale'])
        self.assertIsNone(store.read('daily',now='2026-10-30T23:00:31Z')['data']['air_quality'])

    def test_air_updates_independently_when_weather_time_is_unchanged(self):
        now='2026-10-30T02:00:01Z'; observed='2026-10-30T02:00:00Z'
        snap={'generated_at':now,'daily':{'updated_at':observed,'source':'moji-xiangan','weather':{'status':'available','air_quality':None}}}
        store=module.HubStore();self.assertTrue(store.update('daily',snap,now=now))
        snap['daily']['weather']['air_quality']={'aqi':33,'description':'优','updated_at':'2026-10-29T21:00:00Z'}
        self.assertTrue(store.update('daily',snap,now=now)); data=store.read('daily',now=now)
        self.assertEqual(data['updated_at'],observed);self.assertEqual(data['data']['air_quality']['aqi'],33);self.assertTrue(data['data']['air_quality']['stale'])
    def test_cloud_uptime_advances_with_original_observation(self):
        observed='2026-10-30T01:00:00Z';now='2026-10-30T01:00:30Z'
        snap={'generated_at':observed,'aliyun':{'updated_at':observed,'status':'available','uptime_seconds':100}}
        store=module.HubStore();self.assertTrue(store.update('aliyun',snap,now=now))
        self.assertEqual(store.read('aliyun',now='2026-10-30T01:01:00Z')['data']['uptime_seconds'],160)

    def test_cloud_uptime_is_not_double_counted_during_upload(self):
        observed='2026-10-30T01:00:00Z';now='2026-10-30T01:00:30Z'
        snap={'generated_at':observed,'aliyun':{'updated_at':observed,'status':'available','uptime_seconds':100}}
        envelope=module.source_envelope('aliyun',snap,now=now)
        store=module.HubStore();self.assertTrue(store.update('aliyun',envelope,now=now))
        self.assertEqual(store.read('aliyun',now=now)['data']['uptime_seconds'],130)

    def test_schedule_rolls_to_new_local_day_without_refresh(self):
        observed='2026-10-30T15:59:59Z';now='2026-10-30T16:00:01Z'
        snap={'generated_at':observed,'schedule':{'updated_at':observed,'status':'available','courses':[]}}
        store=module.HubStore();self.assertTrue(store.update('schedule',snap,now=observed))
        self.assertEqual(store.read('schedule',now=now)['data']['day'],6)

    def test_corrupt_database_is_not_replaced(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/'hub.db';content=b'not a sqlite database';path.write_bytes(content)
            with self.assertRaises(sqlite3.DatabaseError):module.HubStore(path)
            self.assertEqual(path.read_bytes(),content)

    def test_old_future_and_wrong_sources_do_not_override(self):
        store=module.HubStore();snap={'generated_at':'2026-10-30T01:00:00Z','dorm_pc':{'updated_at':'2026-10-30T01:00:00Z','cpu':{'usage_percent':10}}}
        self.assertTrue(store.update('dorm_pc',snap,now='2026-10-30T01:00:01Z'))
        self.assertFalse(store.update('dorm_pc',snap,now='2026-10-30T00:59:00Z'))
        self.assertFalse(store.update('bad',snap,now='2026-10-30T01:00:01Z'))
        self.assertFalse(store.update('dorm_pc',snap,now='2026-10-30T01:11:00Z'))
        self.assertEqual(store.read('dorm_pc',now='2026-10-30T01:00:01Z')['data']['cpu']['usage_percent'],10)

if __name__=='__main__':unittest.main()
