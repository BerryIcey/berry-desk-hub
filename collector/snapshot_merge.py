"""Keep independent observations without letting older completions win."""
from copy import deepcopy
from math import isfinite
from threading import Lock

class SnapshotMerge:
    def __init__(self):self._parts={};self._lock=Lock()
    def put(self,name,observed,data):
        if not isinstance(name,str) or not name or type(observed) not in (int,float) or not isfinite(observed) or not isinstance(data,dict):return False
        with self._lock:
            if name in self._parts and observed<=self._parts[name]["observed"]:return False
            self._parts[name]={"observed":observed,"data":deepcopy(data)}
            return True
    def read(self):
        with self._lock:return deepcopy(self._parts)
