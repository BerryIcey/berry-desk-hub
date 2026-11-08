"""Pinned, read-only SSH health snapshot. No credentials are returned."""
import json
import subprocess
import socket
import time
from pathlib import Path

def tcp_latency(host,connect=socket.create_connection,clock=time.monotonic):
    try:
        start=clock();connection=connect((host,22),timeout=4);elapsed=clock()-start;connection.close()
        return round(elapsed*1000) if 0<=elapsed<=4 else None
    except OSError:
        return None

REMOTE = r'''
import json, os, time
def cpu():
    with open('/proc/stat', encoding='ascii') as f: v = list(map(int, f.readline().split()[1:]))
    return sum(v), v[3] + v[4]
def net():
    try:
        with open('/proc/net/route', encoding='ascii') as f:
            routes = [line.split() for line in f.readlines()[1:]]
        iface = next(r[0] for r in routes if len(r)>3 and r[1]=='00000000' and int(r[3],16)&1)
        with open('/proc/net/dev', encoding='ascii') as f:
            fields = next(line.split(':',1)[1].split() for line in f if line.split(':',1)[0].strip()==iface and ':' in line)
        return int(fields[0]), int(fields[8])
    except (OSError, StopIteration, ValueError, IndexError):
        return None
a = cpu(); na = net(); start = time.monotonic(); time.sleep(1); b = cpu(); nb = net(); elapsed = time.monotonic() - start
delta = b[0] - a[0]
usage = round(100 * (1 - (b[1] - a[1]) / delta)) if delta > 0 else None
with open('/proc/meminfo', encoding='ascii') as f:
    mem = {p[0].rstrip(':'): int(p[1])*1024 for line in f if len(p := line.split()) >= 2 and p[1].isdigit()}
total, available = mem.get('MemTotal'), mem.get('MemAvailable')
disk = os.statvfs('/')
capacity = disk.f_blocks * disk.f_frsize
used = (disk.f_blocks - disk.f_bfree) * disk.f_frsize
with open('/proc/uptime', encoding='ascii') as f: uptime = int(float(f.read().split()[0]))
down = round((nb[0]-na[0])/elapsed) if na and nb and nb[0]>=na[0] and elapsed>0 else None
up = round((nb[1]-na[1])/elapsed) if na and nb and nb[1]>=na[1] and elapsed>0 else None
def relay():
    try:
        running=False
        for pid in os.listdir('/proc'):
            if not pid.isdigit(): continue
            try:
                with open('/proc/'+pid+'/comm',encoding='ascii') as f:
                    if f.read().strip()=='hbbr': running=True;break
            except (OSError,UnicodeError): continue
        listening=False
        for filename in ('/proc/net/tcp','/proc/net/tcp6'):
            try:
                with open(filename,encoding='ascii') as f:
                    for line in list(f)[1:]:
                        fields=line.split()
                        if len(fields)>3 and fields[3]=='0A' and int(fields[1].split(':')[-1],16)==21117: listening=True
            except OSError: pass
        return 'healthy' if running and listening else 'unknown'
    except (OSError,ValueError): return 'unknown'
print(json.dumps({'cpu_percent':usage, 'memory_total':total, 'memory_available':available,
                  'disk_total':capacity, 'disk_used':used, 'uptime_seconds':uptime,
                  'download_bytes_per_second':down, 'upload_bytes_per_second':up,'relay_status':relay()}))
'''


def probe(root):
    folder = Path(root) / '.local' / 'server-admin' / 'ALI-SZ-INIT-20261023'
    ssh = Path.home() / '.cache/codex-runtimes/codex-primary-runtime/dependencies/native/git/usr/bin/ssh.exe'
    if not all(p.is_file() for p in (ssh, folder/'id_ed25519', folder/'known_hosts')):
        return {'status': 'unknown'}
    # This target and its host key come from the existing locally pinned admin setup.
    import re
    script = (folder/'ssh_recovery_probe.py').read_text(encoding='utf8')
    match = re.search(r"'admin@([0-9.]+)'", script)
    if not match:
        return {'status': 'unknown'}
    args = [str(ssh), '-T', '-o', 'BatchMode=yes', '-o', 'PasswordAuthentication=no', '-o', 'KbdInteractiveAuthentication=no', '-o', 'StrictHostKeyChecking=yes', '-o', f'UserKnownHostsFile={folder / "known_hosts"}', '-o', 'IdentityAgent=none', '-o', 'IdentitiesOnly=yes', '-o', 'ConnectTimeout=8', '-o', 'IPQoS=none', '-i', str(folder/'id_ed25519'), 'admin@'+match.group(1), 'python3 -']
    try:
        out = subprocess.run(args, input=REMOTE, text=True, capture_output=True, timeout=15, check=True, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        data = json.loads(out.stdout)
        data['status'] = 'available'
        data['latency_ms'] = tcp_latency(match.group(1))
        # Includes remote one-second CPU sample; do not mislabel as network ping.
        return data
    except (OSError, subprocess.SubprocessError, ValueError):
        return {'status': 'unknown'}


if __name__ == '__main__':
    print(json.dumps(probe(Path.cwd())))
