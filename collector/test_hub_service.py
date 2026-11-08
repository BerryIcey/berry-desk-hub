import json,tempfile,unittest
from pathlib import Path
from hub_service import load_config
from hub_store import HubStore

class HubServiceTests(unittest.TestCase):
 def test_accepts_only_complete_separate_credentials_and_loopback_port(self):
  with tempfile.TemporaryDirectory() as d:
   p=Path(d)/"service.json";tokens={name:(name+"_"+"a"*48) for name in HubStore.SOURCES};config={"port":18473,"read_token":"r"*48,"write_tokens":tokens,"store_path":str(Path(d)/"state.db")}
   p.write_text(json.dumps(config));self.assertEqual(load_config(p)["port"],18473)
   config["write_tokens"]["codex"]=config["read_token"];p.write_text(json.dumps(config))
   with self.assertRaises(ValueError):load_config(p)
   config["write_tokens"]={name:(name+"_"+"a"*48) for name in HubStore.SOURCES};config["port"]=0;p.write_text(json.dumps(config))
   with self.assertRaises(ValueError):load_config(p)
