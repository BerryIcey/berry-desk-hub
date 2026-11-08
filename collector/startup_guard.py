"""Start missing Berry workers without restarting unrelated services."""
import json,os,subprocess,sys
from pathlib import Path
import psutil

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]

def is_running(processes,script):
    return any((p.get('name') or '').lower()=='python.exe' and any(str(x).replace('\\','/').split('/')[-1].lower()==script.lower() for x in (p.get('cmdline') or [])) for p in processes)

def spawn(runner,env):
    python=str(Path(sys.executable).with_name('python.exe'))
    subprocess.Popen([python,str(HERE/runner)],cwd=ROOT,env=env,stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=subprocess.CREATE_NO_WINDOW)

def main():
    config=json.loads((HERE/'startup-settings.json').read_text(encoding='utf8'))
    env=os.environ.copy();env['BERRY_CODEX_EXECUTABLE']=config['codex_executable'];env['CODEX_THREAD_ID']=config['thread_id']
    processes=[]
    for p in psutil.process_iter(['name','cmdline','exe']):
        try:processes.append(p.info)
        except (psutil.NoSuchProcess,psutil.AccessDenied):pass
    for p in processes:
        if (p.get('name') or '').lower()=='codex.exe' and 'app-server' in (p.get('cmdline') or []) and p.get('exe'):
            env['BERRY_CODEX_EXECUTABLE']=p['exe'];break
    for worker,runner in (('parallel_worker.py','parallel_runner.py'),('hub_push.py','hub_push.py')):
        if not is_running(processes,worker):
            spawn(runner,env)

if __name__=='__main__':main()
