import importlib.util,json,unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('hub_upload',Path(__file__).with_name('hub_upload.py'));module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
class Response:
 status=200
 def __enter__(self):return self
 def __exit__(self,*a):pass
 def read(self,limit):return b'{"accepted":true}'
class UploaderTests(unittest.TestCase):
 def test_only_https_and_only_selected_filtered_source(self):
  calls=[]
  def opener(req,timeout):calls.append(req);return Response()
  now='2026-10-30T01:00:01Z';snap={'generated_at':now,'secret':'PRIVATE','codex':{'updated_at':now,'recent_tasks':[{'title':'password PRIVATE TITLE','status':'running'}]},'aliyun':{'secret':'PRIVATE'}}
  self.assertFalse(module.upload('http://example.test','codex',snap,'test-secret',now=now,opener=opener));self.assertFalse(calls)
  self.assertTrue(module.upload('https://example.test','codex',snap,'test-secret',now=now,opener=opener))
  body=json.loads(calls[0].data);self.assertNotIn('aliyun',body);self.assertNotIn('PRIVATE',str(body))
 def test_invalid_destination_and_failed_ack(self):
  now='2026-10-30T01:00:01Z';snap={'generated_at':now,'codex':{'updated_at':now}}
  for url in ('https://user:pass@example.test','https://example.test/path','https://example.test/?x=1'):
   self.assertFalse(module.upload(url,'codex',snap,'token',now=now,opener=lambda *a,**k:Response()))
  class Bad(Response):
   def read(self,limit):return b'{"accepted":false}'
  self.assertFalse(module.upload('https://example.test','codex',snap,'token',now=now,opener=lambda *a,**k:Bad()))
if __name__=='__main__':unittest.main()
