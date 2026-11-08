import importlib.util,unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('app_tasks',Path(__file__).with_name('app_task_probe.py'));module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
class TaskProbeTests(unittest.TestCase):
 def test_maps_authoritative_status_and_turn_outcome(self):
  def row(status,turn=None):return module.classify({'thread':{'status':status},'turns':[] if turn is None else [{'status':turn}]})
  self.assertEqual(row({'type':'active','activeFlags':['waitingOnApproval']}),'waiting_approval')
  self.assertEqual(row({'type':'active','activeFlags':['waitingOnUserInput']}),'blocked')
  self.assertEqual(row({'type':'active','activeFlags':[]}),'running')
  self.assertEqual(row({'type':'idle'},'completed'),'completed')
  self.assertEqual(row({'type':'idle'},'failed'),'error')
  self.assertEqual(row({'type':'notLoaded'},'completed'),'unknown')
  self.assertEqual(row({'type':'active'}),'unknown')
 def test_sensitive_titles_and_bad_rows_are_filtered(self):
  rows=module.pick({'threads':[{'kind':'codex','id':'abc','title':'密码 test','updatedAt':2},{'kind':'chatgpt','id':'xyz','title':'chat','updatedAt':3}]})
  self.assertEqual(len(rows),1);self.assertEqual(rows[0]['title'],'标题已隐藏')
if __name__=='__main__':unittest.main()
