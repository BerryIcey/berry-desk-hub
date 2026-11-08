"""Explicit loopback-only server entry; TLS termination is external."""
import json,re,sys
from pathlib import Path
from hub_store import HubStore
from hub_http import make_server

def load_config(path):
    value=json.loads(Path(path).read_text(encoding="utf8"))
    if not isinstance(value,dict):raise ValueError("invalid configuration")
    port=value.get("port");tokens=value.get("write_tokens");reader=value.get("read_token");store=value.get("store_path")
    if type(port) is not int or not 1024<=port<=65535 or not isinstance(tokens,dict) or set(tokens)!=set(HubStore.SOURCES):raise ValueError("invalid configuration")
    keys=[reader,*tokens.values()]
    if any(not isinstance(key,str) or not re.fullmatch(r"[A-Za-z0-9_-]{32,128}",key) for key in keys) or len(set(keys))!=len(keys):raise ValueError("invalid configuration")
    if not isinstance(store,str) or not Path(store).is_absolute():raise ValueError("invalid store")
    return value

def main():
    if len(sys.argv)!=2:return 2
    cfg=load_config(sys.argv[1]);server=make_server(cfg["write_tokens"],cfg["read_token"],store_path=cfg["store_path"],port=cfg["port"])
    try:server.serve_forever()
    finally:server.server_close()
    return 0

if __name__=="__main__":raise SystemExit(main())
