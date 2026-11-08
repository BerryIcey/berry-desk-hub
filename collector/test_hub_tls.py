import datetime,ipaddress,json,ssl,tempfile,threading,unittest
from pathlib import Path
from urllib.request import build_opener,HTTPSHandler,Request
from cryptography import x509
from cryptography.hazmat.primitives import hashes,serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID
from hub_http import make_server
from hub_upload import NoRedirect,upload

class TLSIntegrationTests(unittest.TestCase):
 def test_tls_trusted_peer_and_untrusted_peer(self):
  with tempfile.TemporaryDirectory() as d:
   key=rsa.generate_private_key(public_exponent=65537,key_size=2048)
   name=x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,'localhost')]);utc=datetime.datetime.now(datetime.timezone.utc)
   cert=(x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key()).serial_number(x509.random_serial_number()).not_valid_before(utc-datetime.timedelta(minutes=1)).not_valid_after(utc+datetime.timedelta(days=1)).add_extension(x509.BasicConstraints(ca=True,path_length=None),critical=True).add_extension(x509.SubjectAlternativeName([x509.DNSName('localhost')]),critical=False).sign(key,hashes.SHA256()))
   certpath=Path(d)/'cert.pem';keypath=Path(d)/'key.pem';certpath.write_bytes(cert.public_bytes(serialization.Encoding.PEM));keypath.write_bytes(key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.TraditionalOpenSSL,serialization.NoEncryption()))
   server=make_server({'codex':'temporary-test-key'},'temporary-test-reader');ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);ctx.load_cert_chain(certpath,keypath);server.socket=ctx.wrap_socket(server.socket,server_side=True)
   thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
   try:
    now=utc.isoformat();snap={'generated_at':now,'codex':{'updated_at':now,'quota_windows':[]}};url='https://localhost:'+str(server.server_port)
    self.assertFalse(upload(url,'codex',snap,'temporary-test-key',now=now))
    trusted=build_opener(HTTPSHandler(context=ssl.create_default_context(cafile=str(certpath))),NoRedirect())
    self.assertTrue(upload(url,'codex',snap,'temporary-test-key',now=now,opener=trusted.open))
    with trusted.open(Request(url+'/api/v1/device',headers={'Authorization':'Bearer temporary-test-reader'}),timeout=3) as response:
     device=json.loads(response.read());self.assertEqual(response.status,200);self.assertEqual(device['status'],'fresh');self.assertEqual(device['codex']['quota_windows'],[])
   finally:server.shutdown();server.server_close();thread.join()
if __name__=='__main__':unittest.main()
