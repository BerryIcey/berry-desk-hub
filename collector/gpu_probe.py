"""Read the busiest GPU engine of the discrete adapter using Windows DXGI counters."""
import ctypes
import json
import os
import re
import subprocess
from ctypes import wintypes

def measure(adapters,counters):
    visible=set()
    for row in counters:
        match=re.search(r'luid_(0x[0-9a-fA-F]{8}_0x[0-9a-fA-F]{8})_phys_',str(row.get('Name','')))
        if match: visible.add(match[1].lower())
    hardware=[a for a in adapters if not a.get('software') and isinstance(a.get('vram'),int) and a['vram']>0 and re.fullmatch(r'0x[0-9a-fA-F]{8}_0x[0-9a-fA-F]{8}',str(a.get('luid',''))) and a['luid'].lower() in visible]
    if not hardware: return None
    selected=max(hardware,key=lambda a:a['vram'])
    if sum(a['vram']==selected['vram'] for a in hardware)>1: return None
    totals={}
    for row in counters:
        name=row.get('Name','')
        match=re.search(r'luid_(0x[0-9a-fA-F]{8}_0x[0-9a-fA-F]{8})_phys_(\d+)_eng_(\d+)_engtype_',name)
        value=row.get('UtilizationPercentage')
        if match and match[1].lower()==selected['luid'].lower() and type(value) in (int,float) and 0<=value<=100:
            key=(match[2],match[3]);totals[key]=totals.get(key,0)+value
    return min(100,round(max(totals.values()),2)) if totals else None

def adapters():
    class GUID(ctypes.Structure):
        _fields_=[('data1',wintypes.DWORD),('data2',wintypes.WORD),('data3',wintypes.WORD),('data4',ctypes.c_ubyte*8)]
    class LUID(ctypes.Structure):
        _fields_=[('low',wintypes.DWORD),('high',wintypes.LONG)]
    class DESC(ctypes.Structure):
        _fields_=[('description',wintypes.WCHAR*128),('vendor',wintypes.UINT),('device',wintypes.UINT),('subsys',wintypes.UINT),('revision',wintypes.UINT),('vram',ctypes.c_size_t),('system',ctypes.c_size_t),('shared',ctypes.c_size_t),('luid',LUID),('flags',wintypes.UINT)]
    iid=GUID(0x770aae78,0xf26f,0x4dba,(ctypes.c_ubyte*8)(0xa8,0x29,0x25,0x3c,0x83,0xd1,0xb3,0x87))
    dxgi=ctypes.WinDLL('dxgi');dxgi.CreateDXGIFactory1.argtypes=[ctypes.POINTER(GUID),ctypes.POINTER(ctypes.c_void_p)];dxgi.CreateDXGIFactory1.restype=ctypes.c_long
    factory=ctypes.c_void_p()
    if dxgi.CreateDXGIFactory1(ctypes.byref(iid),ctypes.byref(factory))<0: return []
    def method(obj,index,restype,*args):
        table=ctypes.cast(obj,ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
        return ctypes.WINFUNCTYPE(restype,ctypes.c_void_p,*args)(table[index])
    result=[]
    try:
        enum=method(factory,12,ctypes.c_long,wintypes.UINT,ctypes.POINTER(ctypes.c_void_p))
        for i in range(32):
            adapter=ctypes.c_void_p()
            if enum(factory,i,ctypes.byref(adapter))<0: break
            try:
                desc=DESC();get=method(adapter,10,ctypes.c_long,ctypes.POINTER(DESC))
                if get(adapter,ctypes.byref(desc))>=0:
                    result.append({'luid':f'0x{desc.luid.high & 0xffffffff:08X}_0x{desc.luid.low:08X}','vram':int(desc.vram),'software':bool(desc.flags&2),'name':desc.description})
            finally: method(adapter,2,wintypes.ULONG)(adapter)
    finally: method(factory,2,wintypes.ULONG)(factory)
    return result

def probe():
    try:
        command="$samples=Get-Counter '\\GPU Engine(*)\\Utilization Percentage' -SampleInterval 1 -MaxSamples 2 -ErrorAction Stop; @($samples[-1].CounterSamples | ForEach-Object { [PSCustomObject]@{Name=$_.InstanceName;UtilizationPercentage=$_.CookedValue} }) | ConvertTo-Json -Compress"
        env=os.environ.copy()
        env['PSModulePath']=str(__import__('pathlib').Path(os.environ.get('SystemRoot',r'C:\Windows'))/'System32/WindowsPowerShell/v1.0/Modules')+os.pathsep+env.get('PSModulePath','')
        run=subprocess.run(['powershell.exe','-NoProfile','-NonInteractive','-Command',command],capture_output=True,text=True,timeout=12,creationflags=subprocess.CREATE_NO_WINDOW,check=True,env=env)
        rows=json.loads(run.stdout)
        if isinstance(rows,dict): rows=[rows]
        return measure(adapters(),rows) if isinstance(rows,list) else None
    except (OSError,ValueError,subprocess.SubprocessError,AttributeError): return None

if __name__=='__main__': print(json.dumps({'usage_percent':probe()}))
