import http.client
import json
import threading
import tempfile
from pathlib import Path
import unittest
from datetime import datetime,timezone
from hub_http import make_server

class HubHttpTests(unittest.TestCase):
 def test_authenticated_upload_read_and_redaction(self):
  server=make_server({'codex':'writer-secret'},'reader-secret')
  t=threading.Thread(target=server.serve_forever,daemon=True);t.start()
  try:
   conn=http.client.HTTPConnection('127.0.0.1',server.server_port,timeout=3)
   now=datetime.now(timezone.utc).isoformat();snap={'generated_at':now,'codex':{'updated_at':now,'recent_tasks':[{'title':'password PRIVATE','status':'running'}]}}
   body=json.dumps(snap).encode()
   def post(key):
    conn.request('POST','/api/v1/sources/codex',body,{'Authorization':'Bearer '+key,'Content-Type':'application/json'});r=conn.getresponse();r.read();return r.status
   self.assertEqual(post('wrong'),401);self.assertEqual(post('writer-secret'),200);self.assertEqual(post('writer-secret'),200)
   conn.request('POST','/api/v1/sources/aliyun',body,{'Authorization':'Bearer writer-secret'});r=conn.getresponse();r.read();self.assertEqual(r.status,401)
   malformed=json.dumps({'generated_at':now,'codex':{'updated_at':now,'quota_windows':[42]}}).encode()
   conn.request('POST','/api/v1/sources/codex',malformed,{'Authorization':'Bearer writer-secret'});r=conn.getresponse();r.read();self.assertEqual(r.status,400)
   conn.request('GET','/api/v1/snapshot',headers={'Authorization':'Bearer wrong'});r=conn.getresponse();r.read();self.assertEqual(r.status,401)
   conn.request('GET','/api/v1/snapshot',headers={'Authorization':'Bearer reader-secret'});r=conn.getresponse();out=r.read().decode();self.assertEqual(r.status,200);self.assertNotIn('PRIVATE',out);self.assertNotIn('writer-secret',out);self.assertEqual(json.loads(out)['codex']['status'],'fresh')
   conn.close()
  finally:server.shutdown();server.server_close();t.join()

 def test_device_endpoint_is_authenticated_and_filters_stale_sources(self):
  server=make_server({'codex':'writer'},'reader');thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
  try:
   c=http.client.HTTPConnection('127.0.0.1',server.server_port,timeout=3)
   c.request('GET','/api/v1/device');r=c.getresponse();r.read();self.assertEqual(r.status,401)
   now=datetime.now(timezone.utc).isoformat();body=json.dumps({'generated_at':now,'codex':{'updated_at':now,'quota_windows':[{'remaining_percent':42}]}})
   c.request('POST','/api/v1/sources/codex',body,{'Authorization':'Bearer writer'});r=c.getresponse();r.read();self.assertEqual(r.status,200)
   c.request('GET','/api/v1/device',headers={'Authorization':'Bearer reader'});r=c.getresponse();out=json.loads(r.read());self.assertEqual(r.status,200)
   self.assertEqual(out['codex']['quota_windows'][0]['remaining_percent'],42);self.assertEqual(out['dorm_pc'],{});self.assertEqual(out['status'],'fresh')
   c.close()
  finally:server.shutdown();server.server_close();thread.join()

 def test_device_response_fits_usb_frame_limit_with_long_titles(self):
  server=make_server({'codex':'writer','schedule':'other'},'reader');thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
  try:
   c=http.client.HTTPConnection('127.0.0.1',server.server_port,timeout=3);now=datetime.now(timezone.utc).isoformat()
   snapshots={'codex':{'updated_at':now,'recent_tasks':[{'title':'测'*60,'status':'running'} for _ in range(4)]},'schedule':{'updated_at':now,'status':'available','courses':[[i%7+1,'10:00','课'*60,'楼'*80] for i in range(24)]}}
   for source,section in snapshots.items():
    c.request('POST','/api/v1/sources/'+source,json.dumps({'generated_at':now,source:section},ensure_ascii=False).encode(),{'Authorization':'Bearer '+('writer' if source=='codex' else 'other')});r=c.getresponse();r.read();self.assertEqual(r.status,200)
   c.request('GET','/api/v1/device',headers={'Authorization':'Bearer reader'});r=c.getresponse();raw=r.read();self.assertEqual(r.status,200);self.assertLessEqual(len(raw)+14,8192);self.assertTrue(json.loads(raw)['schedule']['courses'])
   c.close()
  finally:server.shutdown();server.server_close();thread.join()

 def test_restart_retains_snapshot_without_freshening_it(self):
  with tempfile.TemporaryDirectory() as d:
   path=Path(d)/'test.db';now=datetime.now(timezone.utc).isoformat();snap={'generated_at':now,'codex':{'updated_at':now,'quota_windows':[]}}
   one=make_server({'codex':'writer'},'reader',store_path=path);thread=threading.Thread(target=one.serve_forever,daemon=True);thread.start()
   try:
    c=http.client.HTTPConnection('127.0.0.1',one.server_port,timeout=3);c.request('POST','/api/v1/sources/codex',json.dumps(snap),{'Authorization':'Bearer writer'});r=c.getresponse();r.read();self.assertEqual(r.status,200);c.close()
   finally:one.shutdown();one.server_close();thread.join()
   two=make_server({'codex':'writer'},'reader',store_path=path);thread=threading.Thread(target=two.serve_forever,daemon=True);thread.start()
   try:
    c=http.client.HTTPConnection('127.0.0.1',two.server_port,timeout=3);c.request('POST','/api/v1/sources/codex',json.dumps(snap),{'Authorization':'Bearer writer'});r=c.getresponse();r.read();self.assertEqual(r.status,200)
    c.request('GET','/api/v1/snapshot',headers={'Authorization':'Bearer reader'});r=c.getresponse();result=json.loads(r.read());self.assertEqual(result['codex']['updated_at'],now);c.close()
   finally:two.shutdown();two.server_close();thread.join()

if __name__=='__main__':unittest.main()
