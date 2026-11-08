"""Independent sender for the existing local snapshot."""
import json,os,sys,time
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime,timezone
from pathlib import Path
from hub_store import HubStore
from hub_upload import upload
from worker_lock import instance_lock

ROOT=Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:sys.path.insert(0,str(ROOT))
from qr_publisher import QRPush,current_qr,CachedLoginAPI
from integration.health_email import _protect

def push_snapshot(config,snapshot,*,now,sender=upload):
    def one(source):
        try:return sender(config['base'],source,snapshot,config['write_tokens'][source],now=now) is True
        except Exception:return False
    with ThreadPoolExecutor(max_workers=len(HubStore.SOURCES)) as pool:
        return dict(zip(HubStore.SOURCES,pool.map(one,HubStore.SOURCES)))

def main():
    directory=Path(__file__).resolve().parent
    with instance_lock(directory/'hub-push.lock') as acquired:
        if not acquired:return 1
        config=json.loads((directory/'hub-push-private.json').read_text(encoding='utf8'))
        try:qr_token=_protect((directory/'qr-writer.dpapi').read_bytes(),decrypt=True).decode('utf8')
        except (OSError,UnicodeError):qr_token=None
        qr_push=QRPush();qq_api=CachedLoginAPI(ROOT)
        while True:
            start=time.monotonic()
            try:
                snapshot=json.loads((directory/'snapshot.json').read_text(encoding='utf8'))
                now=datetime.now(timezone.utc).isoformat()
                result=push_snapshot(config,snapshot,now=now)
                if qr_token:qr_push.tick(config['base'],qr_token,current_qr(ROOT,api=qq_api))
                state={'updated_at':now,'sources':result}
                target=directory/'hub-push-state.json';temp=target.with_suffix('.tmp')
                temp.write_text(json.dumps(state),encoding='utf8');os.replace(temp,target)
            except (OSError,ValueError,KeyError,TypeError):pass
            time.sleep(max(1,10-(time.monotonic()-start)))

if __name__=='__main__':sys.exit(main())
