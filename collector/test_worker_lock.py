import importlib.util
import multiprocessing
import tempfile
import unittest
from pathlib import Path

spec=importlib.util.spec_from_file_location('worker_lock',Path(__file__).with_name('worker_lock.py'))
worker_lock=importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker_lock)

def try_lock(path, queue):
    with worker_lock.instance_lock(Path(path)) as acquired:
        queue.put(acquired)

class WorkerLockTest(unittest.TestCase):
    def test_second_process_is_excluded_and_lock_is_released(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'worker.lock'
            with worker_lock.instance_lock(path) as acquired:
                self.assertTrue(acquired)
                queue=multiprocessing.Queue();child=multiprocessing.Process(target=try_lock,args=(str(path),queue))
                child.start();child.join(10);self.assertEqual(child.exitcode,0);self.assertFalse(queue.get(timeout=2))
            with worker_lock.instance_lock(path) as acquired:
                self.assertTrue(acquired)

if __name__=='__main__': unittest.main()
