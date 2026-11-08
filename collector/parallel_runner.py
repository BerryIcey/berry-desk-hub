"""Keep child probes tied to the collector process on Windows."""
import os,subprocess,sys
from pathlib import Path

def main():
    root=Path(__file__).resolve().parents[2]
    sys.path.insert(0,str(root))
    from integration.process_guard import ChildJob
    child=subprocess.Popen([sys.executable,str(Path(__file__).with_name('parallel_worker.py'))],cwd=root,env=os.environ.copy(),creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    try:guard=ChildJob(child.pid)
    except BaseException:
        child.terminate();child.wait();raise
    try:return child.wait()
    finally:guard.close()

if __name__=='__main__':sys.exit(main())
