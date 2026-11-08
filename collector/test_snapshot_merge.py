import unittest
from snapshot_merge import SnapshotMerge

class MergeTests(unittest.TestCase):
 def test_independent_parts_keep_newest_values_and_timestamps(self):
  s=SnapshotMerge();self.assertTrue(s.put("codex.tasks",20,{"status":"running"}));self.assertTrue(s.put("codex.quota",10,{"remaining":50}))
  self.assertFalse(s.put("codex.tasks",19,{"status":"idle"}));v=s.read()
  self.assertEqual(v["codex.tasks"]["data"]["status"],"running");self.assertEqual(v["codex.quota"]["data"]["remaining"],50)
 def test_invalid_or_shared_mutable_input_does_not_modify_saved_value(self):
  s=SnapshotMerge();data={"usage":1};self.assertTrue(s.put("gpu",10,data));data["usage"]=9
  self.assertFalse(s.put("gpu",float("nan"),{"usage":3}));copy=s.read();copy["gpu"]["data"]["usage"]=8
  self.assertEqual(s.read()["gpu"]["data"]["usage"],1)
