"""Discover the current desktop app connection without storing its pipe."""
import psutil

def choose_pipe(values):
    unique={v for v in values if isinstance(v,str) and v}
    return next(iter(unique)) if len(unique)==1 else None

def live_pipe():
    values=[]
    for process in psutil.process_iter(['name']):
        try:
            if 'codex' in (process.info.get('name') or '').lower():values.append(process.environ().get('CODEX_APP_TOOLS_PIPE_PATH'))
        except (psutil.NoSuchProcess,psutil.AccessDenied,OSError):pass
    return choose_pipe(values)
