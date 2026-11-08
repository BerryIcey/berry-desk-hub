"""Explicit foreground entry point; the caller owns stopping the worker."""
import os
import shutil
import subprocess
import sys
from pathlib import Path
from worker_lock import instance_lock

def main():
    root=Path(__file__).resolve().parents[2]
    node=shutil.which('node')
    if not node or not os.environ.get('BERRY_CODEX_EXECUTABLE'):
        print('worker_configuration_missing')
        return 1
    with instance_lock(root/'.local'/'berry-hub-live'/'worker.lock') as acquired:
        if not acquired:
            print('worker_already_running')
            return 1
        env=dict(os.environ,BERRY_WORKER_LOCKED='1')
        sys.path.insert(0,str(root))
        from integration.process_guard import ChildJob
        child=subprocess.Popen([node,str(Path(__file__).with_name('worker_runtime.mjs')),'--run'],cwd=root,env=env)
        try:
            guard=ChildJob(child.pid)
        except BaseException:
            child.terminate()
            child.wait()
            raise
        try:
            return child.wait()
        finally:
            guard.close()

if __name__=='__main__': sys.exit(main())
