import importlib.util
import sys
import unittest
import sqlite3
import tempfile
from contextlib import closing
from datetime import datetime, timezone, timedelta
from pathlib import Path
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('berry_zhiqing_probe', Path(__file__).with_name('zhiqing_probe.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ProbeTests(unittest.TestCase):
    def test_delivery_metrics_read_only_deduplicate_rounds(self):
        now=datetime(2026,10,29,12,0,tzinfo=timezone(timedelta(hours=8))).timestamp()
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'.local/state-sync/deliveries.db';path.parent.mkdir(parents=True)
            with closing(sqlite3.connect(path)) as db:
                db.execute('CREATE TABLE deliveries(id TEXT PRIMARY KEY, at REAL, content TEXT, spoken TEXT)')
                db.executemany('INSERT INTO deliveries VALUES(?,?,?,?)',[('event:transport:0',now-30,'private',''),('event:transport:1',now-20,'private','')])
                db.commit()
            result=module.delivery_metrics(Path(directory),now)
            self.assertEqual(result['last_reply_seconds_ago'],20)
            self.assertEqual(result['today_confirmed_rounds'],1)
            self.assertNotIn('private',str(result))
    def test_last_chat_observation_is_not_inferred_from_auxiliary(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); path=root/'.local/health/request-diagnostics.db';path.parent.mkdir(parents=True)
            with closing(sqlite3.connect(path)) as db:
                db.execute('CREATE TABLE requests(caller TEXT,finished REAL,status TEXT,category TEXT)')
                db.executemany('INSERT INTO requests VALUES(?,?,?,?)',[('chat',100,'failed','timeout'),('auxiliary',120,'success','none')]);db.commit()
            self.assertEqual(module.last_chat_observation(root),{'finished_at':100,'outcome':'failed','category':'timeout'})

    def test_identity_must_be_present_and_match(self):
        from integration import napcat_api
        class Api:
            identity = {'bot_qq': None}
            def __init__(self, root): pass
            def call(self, method):
                return {} if method == 'get_login_info' else {'online': True}
        with patch.object(napcat_api, 'NapCatAPI', Api), patch.object(module.socket, 'create_connection', side_effect=OSError):
            self.assertEqual(module.probe(Path('.'))['qq_connection'], 'unknown')


if __name__ == '__main__':
    unittest.main()
