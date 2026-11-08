import unittest
from hub_push import push_snapshot
from hub_store import HubStore

class PushTests(unittest.TestCase):
 def test_each_source_is_uploaded_once_and_failure_is_isolated(self):
  calls=[]
  def send(base,source,snapshot,token,*,now):
   calls.append((source,token));return source!='codex'
  config={'base':'https://example.test','write_tokens':{source:source for source in HubStore.SOURCES}}
  result=push_snapshot(config,{'generated_at':'2026-10-30T00:00:00Z'},now='2026-10-30T00:00:00Z',sender=send)
  self.assertEqual(set(result),set(HubStore.SOURCES));self.assertFalse(result['codex']);self.assertTrue(result['dorm_pc']);self.assertEqual(len(calls),6)
 def test_unexpected_source_failure_does_not_stop_other_uploads(self):
  def send(base,source,snapshot,token,*,now):
   if source=='codex':raise OSError('failed')
   return True
  config={'base':'https://example.test','write_tokens':{source:source for source in HubStore.SOURCES}}
  result=push_snapshot(config,{},now='2026-10-30T00:00:00Z',sender=send);self.assertFalse(result['codex']);self.assertTrue(result['daily'])
