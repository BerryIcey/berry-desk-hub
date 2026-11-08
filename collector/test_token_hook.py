import importlib.util
import json
import sqlite3
import tempfile
import unittest
from pathlib import Path
from contextlib import closing

spec=importlib.util.spec_from_file_location('token_hook',Path(__file__).with_name('token_hook.py'))
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

class TokenHookTests(unittest.TestCase):
    def test_new_requests_are_counted_once_and_missing_is_visible(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); source=root/'.local/health/request-diagnostics.db';source.parent.mkdir(parents=True)
            with closing(sqlite3.connect(source)) as c, c:
                c.execute('CREATE TABLE requests(id TEXT,caller TEXT,at REAL,finished REAL,status TEXT)')
                c.execute('CREATE TABLE request_metrics(request_id TEXT,usage TEXT)')
                c.execute('INSERT INTO requests VALUES(?,?,?,?,?)',('old','chat',90,91,'success'))
            self.assertEqual(module.update(root,now=100)['recorded_total'],0)
            with closing(sqlite3.connect(source)) as c, c:
                c.executemany('INSERT INTO requests VALUES(?,?,?,?,?)',[('a','chat',101,102,'success'),('b','auxiliary',103,104,'success'),('c','chat',105,106,'success')])
                c.executemany('INSERT INTO request_metrics VALUES(?,?)',[('a',json.dumps({'1':{'total':120}})),('b',json.dumps({'1':{'total':30},'2':{'total':20}})),('c',None)])
            first=module.update(root,now=110);again=module.update(root,now=111)
            self.assertEqual(first['recorded_total'],170);self.assertEqual(again['recorded_total'],170)
            self.assertEqual(first['missing_requests'],1);self.assertIsNone(first['latest_chat_tokens'])
            self.assertEqual(first['recorded_requests'],2)
    def test_latest_prior_chat_is_read_without_inventing_historical_total(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);source=root/'.local/health/request-diagnostics.db';source.parent.mkdir(parents=True)
            with closing(sqlite3.connect(source)) as c,c:
                c.execute('CREATE TABLE requests(id TEXT,caller TEXT,at REAL,finished REAL,status TEXT)')
                c.execute('CREATE TABLE request_metrics(request_id TEXT,usage TEXT)')
                c.execute('INSERT INTO requests VALUES(?,?,?,?,?)',('a','chat',90,91,'success'))
                c.execute('INSERT INTO request_metrics VALUES(?,?)',('a',json.dumps({'1':{'total':120}})))
            result=module.update(root,now=100)
            self.assertEqual(result['latest_chat_tokens'],120)
            self.assertEqual(result['latest_chat_finished_at'],91)
            self.assertEqual(result['recorded_requests'],0)

    def test_baseline_and_live_totals_are_disjoint_and_persistent(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);source=root/'.local/health/request-diagnostics.db';source.parent.mkdir(parents=True)
            with closing(sqlite3.connect(source)) as c,c:
                c.execute('CREATE TABLE requests(id TEXT,caller TEXT,at REAL,finished REAL,status TEXT)')
                c.execute('CREATE TABLE request_metrics(request_id TEXT,usage TEXT)')
                c.executemany('INSERT INTO requests VALUES(?,?,?,?,?)',[('old','chat',90,91,'success'),('missing','chat',92,93,'success')])
                c.execute('INSERT INTO request_metrics VALUES(?,?)',('old',json.dumps({'1':{'total':120}})))
            first=module.update(root,now=100);self.assertEqual(first['observed_total'],120);self.assertEqual(first['chat_average'],120);self.assertEqual(first['observed_missing'],1)
            with closing(sqlite3.connect(source)) as c,c:
                c.execute('DELETE FROM requests WHERE at<100')
                c.execute('INSERT INTO requests VALUES(?,?,?,?,?)',('new','chat',101,102,'success'))
                c.execute('INSERT INTO request_metrics VALUES(?,?)',('new',json.dumps({'1':{'total':80}})))
            second=module.update(root,now=110);self.assertEqual(second['observed_total'],200);self.assertEqual(second['chat_average'],100);self.assertEqual(second['observed_missing'],1)

    def test_invalid_usage_is_not_a_zero(self):
        self.assertIsNone(module.total_usage('{"1":{"total":-1}}'))
        self.assertIsNone(module.total_usage('{"1":{"total":true}}'))
        self.assertIsNone(module.total_usage('{}'))

if __name__=='__main__':unittest.main()
