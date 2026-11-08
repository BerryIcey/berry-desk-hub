import unittest
from unittest.mock import patch
import subprocess
import startup_guard
from startup_guard import is_running

class StartupGuardTests(unittest.TestCase):
 def test_identifies_only_exact_script_argument(self):
  processes=[{'name':'python.exe','cmdline':['python.exe',r'C:\x\parallel_worker.py']}, {'name':'notepad.exe','cmdline':['parallel_worker.py']}]
  self.assertTrue(is_running(processes,'parallel_worker.py'))
  self.assertFalse(is_running(processes,'hub_push.py'))
  self.assertFalse(is_running([{'name':'python.exe','cmdline':['python.exe','parallel_worker.py.backup']}],'parallel_worker.py'))

 def test_windows_launch_uses_no_window_without_detached_flag(self):
  with patch.object(startup_guard.subprocess,'Popen') as launch, patch.object(startup_guard.sys,'executable',r'C:\Python\pythonw.exe'):
   startup_guard.spawn('hub_push.py',{})
  flags=launch.call_args.kwargs['creationflags']
  self.assertTrue(flags & subprocess.CREATE_NO_WINDOW)
  self.assertFalse(flags & subprocess.DETACHED_PROCESS)
  self.assertTrue(str(launch.call_args.args[0][0]).lower().endswith('python.exe'))
