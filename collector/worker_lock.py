"""Local Windows process lock for the optional desktop-screen worker."""
from contextlib import contextmanager
from pathlib import Path
import msvcrt

@contextmanager
def instance_lock(path):
    path=Path(path)
    path.parent.mkdir(parents=True,exist_ok=True)
    with path.open('a+b') as handle:
        handle.seek(0)
        try:
            msvcrt.locking(handle.fileno(),msvcrt.LK_NBLCK,1)
        except OSError:
            yield False
            return
        try:
            yield True
        finally:
            handle.seek(0)
            msvcrt.locking(handle.fileno(),msvcrt.LK_UNLCK,1)
