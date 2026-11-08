"""Publish only a fresh local NapCat QR while login is confirmed absent."""

import json
import ssl
import time
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import Request, build_opener, HTTPSHandler, HTTPRedirectHandler

from integration.napcat_api import NapCatAPI, NapCatRejected
from qr_codec import encode_png


class CachedLoginAPI:
    def __init__(self, root):
        self.root = Path(root)
        self.client = None

    def _post(self, route, data):
        try:
            if self.client is None:
                self.client = NapCatAPI(self.root)
            post=getattr(self.client,'_post',None)
            if not callable(post):raise NapCatRejected('backend has no NapCat login-status route')
            return post(route,data)
        except (OSError, ValueError, KeyError, TypeError, NapCatRejected):
            self.client = None
            raise


def current_qr(root, *, now=None, api=None):
    now = time.time() if now is None else now
    try:
        root = Path(root)
        post=getattr(NapCatAPI(root) if api is None else api,'_post',None)
        if not callable(post):return None
        value=post('QQLogin/CheckLoginStatus',{}).get('data')
        if not isinstance(value, dict) or value.get('isLogin') is not False:
            return None
        with (root/'.local/napcat-stdout.log').open('rb') as source:
            source.seek(0, 2)
            source.seek(max(0, source.tell()-262144))
            log = source.read().decode('utf8', errors='replace')
        saved = log.rfind('二维码已保存到')
        if saved < 0 or any(marker in log[saved:] for marker in ('二维码已被扫描', 'Login Error', '[KickedOffLine]')):
            return None
        path = root/'.local/napcat/cache/qrcode.png'
        if path.is_symlink() or not path.is_file():
            return None
        before = path.stat()
        if not -2 <= now-before.st_mtime < 100 or not 0 < before.st_size <= 65536:
            return None
        raw = path.read_bytes()
        after = path.stat()
        if (before.st_mtime_ns, before.st_size) != (after.st_mtime_ns, after.st_size):
            return None
        result = encode_png(raw)
        result['expires_at'] = min(now+20, before.st_mtime+100)
        return result if result['expires_at'] > now else None
    except (OSError, ValueError, KeyError, TypeError, NapCatRejected):
        return None


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def upload(base, token, value, *, opener=None):
    try:
        url = urlsplit(base)
        if url.scheme != 'https' or not url.hostname or url.username or url.password or url.path not in ('', '/') or url.query or url.fragment:
            return False
        if not isinstance(token, str) or not token or any(ord(c) < 33 or ord(c) > 126 for c in token):
            return False
        body = json.dumps({'qr': value}, separators=(',', ':'), allow_nan=False).encode('utf8')
        request = Request(base.rstrip('/')+'/api/v1/sources/zhiqing', data=body, method='POST',
                          headers={'Authorization': 'Bearer '+token, 'Content-Type': 'application/json'})
        if opener is None:
            opener = build_opener(HTTPSHandler(context=ssl.create_default_context()), NoRedirect()).open
        with opener(request, timeout=8) as response:
            return response.status == 200 and json.loads(response.read(4097).decode('utf8')).get('accepted') is True
    except (AttributeError, TypeError, ValueError, OverflowError, OSError, URLError, HTTPError):
        return False


class QRPush:
    def __init__(self):
        self.active = False

    def tick(self, base, token, value, *, sender=upload):
        if value is None and not self.active:
            return True
        if not sender(base, token, value):
            return False
        self.active = value is not None
        return True
