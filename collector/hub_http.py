"""Loopback-only HTTP prototype. Not a production or TLS server."""
import hmac
import json
from datetime import datetime,timezone
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from hub_store import HubStore
from hub_device import to_device
from serial_protocol import make_device_frame


def make_server(write_tokens,read_token,*,store_path=None,port=0):
    if not isinstance(read_token,str) or not read_token or not write_tokens or any(not isinstance(v,str) or not v for v in write_tokens.values()):raise ValueError('tokens required')
    store=HubStore(store_path);read_token=read_token.encode('utf8');write_tokens={k:v.encode('utf8') for k,v in write_tokens.items()}
    class Handler(BaseHTTPRequestHandler):
        def log_message(self,*args):pass
        def authorized(self,expected):
            value=self.headers.get('Authorization','')
            try:return expected is not None and hmac.compare_digest(value.encode('utf8'),b'Bearer '+expected)
            except UnicodeError:return False
        def answer(self,status,value):
            data=json.dumps(value,ensure_ascii=False,separators=(',',':')).encode('utf8')
            self.send_response(status);self.send_header('Content-Type','application/json; charset=utf-8');self.send_header('Content-Length',str(len(data)));self.send_header('Cache-Control','no-store');self.end_headers();self.wfile.write(data)
        def do_GET(self):
            if self.path not in ('/api/v1/snapshot','/api/v1/device'):return self.answer(404,{'error':'not_found'})
            if not self.authorized(read_token):return self.answer(401,{'error':'unauthorized'})
            now=datetime.now(timezone.utc).isoformat()
            values={key:store.read(key,now=now) for key in store.SOURCES}
            if self.path=='/api/v1/device':
                payload=to_device(values,generated_at=now)
                try:body=make_device_frame(payload).split(b' ',2)[2].rstrip(b'\n')
                except (ValueError,TypeError):return self.answer(503,{'error':'payload_too_large'})
                self.send_response(200);self.send_header('Content-Type','application/json; charset=utf-8');self.send_header('Content-Length',str(len(body)));self.send_header('Cache-Control','no-store');self.end_headers();self.wfile.write(body);return
            self.answer(200,values)
        def do_POST(self):
            prefix='/api/v1/sources/'
            source=self.path[len(prefix):] if self.path.startswith(prefix) else ''
            if source not in store.SOURCES:return self.answer(404,{'error':'not_found'})
            if not self.authorized(write_tokens.get(source)):return self.answer(401,{'error':'unauthorized'})
            try:
                length=int(self.headers.get('Content-Length','-1'))
                if not 0<length<=65536:return self.answer(413,{'error':'invalid_length'})
                body=json.loads(self.rfile.read(length).decode('utf8'))
            except (ValueError,UnicodeError):return self.answer(400,{'error':'invalid_json'})
            if not store.update(source,body,now=datetime.now(timezone.utc).isoformat()):return self.answer(400,{'error':'invalid_snapshot'})
            return self.answer(200,{'accepted':True})
    return ThreadingHTTPServer(('127.0.0.1',port),Handler)
