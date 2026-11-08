import sqlite3,tempfile,unittest
from contextlib import closing
from pathlib import Path
from conversation_watch import marker

class ConversationWatchTests(unittest.TestCase):
 def test_detects_new_chat_completion_and_delivery_without_reading_content(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);health=root/'.local/health';health.mkdir(parents=True);state=root/'.local/state-sync';state.mkdir(parents=True)
   with closing(sqlite3.connect(health/'request-diagnostics.db')) as db,db:
    db.execute('CREATE TABLE requests(caller TEXT,finished REAL)');db.execute("INSERT INTO requests VALUES('chat',1)")
   with closing(sqlite3.connect(state/'deliveries.db')) as db,db:
    db.execute('CREATE TABLE deliveries(at REAL)');db.execute('INSERT INTO deliveries VALUES(2)')
   first=marker(root);self.assertEqual(first,(1.0,2.0))
   with closing(sqlite3.connect(health/'request-diagnostics.db')) as db,db:db.execute("INSERT INTO requests VALUES('chat',3)")
   self.assertNotEqual(marker(root),first)
 def test_missing_database_is_unknown(self):
  with tempfile.TemporaryDirectory() as d:self.assertIsNone(marker(Path(d)))
