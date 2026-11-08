"""Observe only timestamps, without accessing chat content."""
import sqlite3
from contextlib import closing
from pathlib import Path

def marker(root):
    paths=[(Path(root)/'.local/health/request-diagnostics.db',"SELECT MAX(finished) FROM requests WHERE caller='chat'"),
           (Path(root)/'.local/state-sync/deliveries.db','SELECT MAX(at) FROM deliveries')]
    values=[]
    try:
        for path,query in paths:
            if not path.is_file():return None
            with closing(sqlite3.connect(path.resolve().as_uri()+'?mode=ro',uri=True,timeout=1)) as db:
                values.append(db.execute(query).fetchone()[0])
        return tuple(values)
    except (OSError,sqlite3.Error):return None
