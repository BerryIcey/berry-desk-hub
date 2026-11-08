"""HTTPS-only sender prototype; no automatic execution or network changes."""
import json
import ssl
from urllib.error import HTTPError,URLError
from urllib.parse import urlsplit
from urllib.request import Request,build_opener,HTTPSHandler,HTTPRedirectHandler
from hub_store import HubStore,source_envelope

class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self,*args,**kwargs):return None


def upload(base,source,snapshot,token,*,now,opener=None):
    try:
        url=urlsplit(base)
        if url.scheme!='https' or not url.hostname or url.username or url.password or url.path not in ('','/') or url.query or url.fragment or source not in HubStore.SOURCES:return False
        if not isinstance(token,str) or not token or any(ord(c)<33 or ord(c)>126 for c in token):return False
        envelope=source_envelope(source,snapshot,now=now)
        if envelope is None:return False
        body=json.dumps(envelope,ensure_ascii=False,separators=(',',':'),allow_nan=False).encode('utf8')
        request=Request(base.rstrip('/')+'/api/v1/sources/'+source,data=body,method='POST',headers={'Authorization':'Bearer '+token,'Content-Type':'application/json','Accept':'application/json'})
        if opener is None:opener=build_opener(HTTPSHandler(context=ssl.create_default_context()),NoRedirect()).open
        with opener(request,timeout=8) as response:
            if response.status!=200:return False
            raw=response.read(4097)
            if len(raw)>4096:return False
            value=json.loads(raw.decode('utf8'))
            return isinstance(value,dict) and value.get('accepted') is True
    except (AttributeError,TypeError,ValueError,OverflowError,OSError,URLError,HTTPError):return False
