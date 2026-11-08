import unittest
from start_context import choose_pipe

class ContextTests(unittest.TestCase):
 def test_uses_unique_live_pipe_and_rejects_ambiguous(self):
  self.assertEqual(choose_pipe(['one','one']), 'one')
  self.assertIsNone(choose_pipe(['one','two']))
  self.assertIsNone(choose_pipe([]))
