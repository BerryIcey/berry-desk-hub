"""Read-only AMD ADL GPU edge temperature in an isolated short-lived process.

Interface definitions follow AMD's MIT-licensed display-library headers.
The deprecated read-only PMLog query is made at the existing five-minute
sampling interval; no driver settings or clocks are changed.
"""
import ctypes
import json
import re

def select_temperature(samples,target):
    values={round(value/1000) for name,value in samples if name.casefold()==target.casefold() and type(value) is int and 0<=value<=120000}
    return max(values) if values and max(values)-min(values)<=2 else None

def probe():
    import gpu_probe
    hardware=[a for a in gpu_probe.adapters() if not a['software'] and a['vram']>0 and a.get('name')]
    if not hardware:return None
    maximum=max(a['vram'] for a in hardware)
    names={a['name'] for a in hardware if a['vram']==maximum}
    if len(names)!=1:return None
    target=next(iter(names))
    if not re.search(r'\b(Radeon|AMD)\b',target,re.I):return None
    class AdapterInfo(ctypes.Structure):
        _fields_=[('size',ctypes.c_int),('index',ctypes.c_int),('udid',ctypes.c_char*256),('bus',ctypes.c_int),('device',ctypes.c_int),('function',ctypes.c_int),('vendor',ctypes.c_int),('name',ctypes.c_char*256),('display',ctypes.c_char*256),('present',ctypes.c_int),('exists',ctypes.c_int),('path',ctypes.c_char*256),('path_ext',ctypes.c_char*256),('pnp',ctypes.c_char*256),('os_display',ctypes.c_int)]
    class Sensor(ctypes.Structure):
        _fields_=[('supported',ctypes.c_int),('value',ctypes.c_int)]
    class PMLog(ctypes.Structure):
        _fields_=[('size',ctypes.c_int),('sensors',Sensor*256)]
    try:
        adl=ctypes.WinDLL('atiadlxx.dll')
        crt=ctypes.CDLL('msvcrt');crt.malloc.argtypes=[ctypes.c_size_t];crt.malloc.restype=ctypes.c_void_p
        callback_type=ctypes.WINFUNCTYPE(ctypes.c_void_p,ctypes.c_int)
        callback=callback_type(lambda size:crt.malloc(size))
        create=adl.ADL_Main_Control_Create;create.argtypes=[callback_type,ctypes.c_int];create.restype=ctypes.c_int
        if create(callback,1)!=0:return None
        try:
            number=ctypes.c_int();get_count=adl.ADL_Adapter_NumberOfAdapters_Get;get_count.argtypes=[ctypes.POINTER(ctypes.c_int)];get_count.restype=ctypes.c_int
            if get_count(ctypes.byref(number))!=0 or not 0<number.value<=64:return None
            info=(AdapterInfo*number.value)()
            get_info=adl.ADL_Adapter_AdapterInfo_Get;get_info.argtypes=[ctypes.POINTER(AdapterInfo),ctypes.c_int];get_info.restype=ctypes.c_int
            if get_info(info,ctypes.sizeof(info))!=0:return None
            get_temp=adl.ADL2_New_QueryPMLogData_Get;get_temp.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.POINTER(PMLog)];get_temp.restype=ctypes.c_int
            samples=[]
            for adapter in info:
                name=adapter.name.decode('utf8','replace').strip('\x00')
                if name.casefold()!=target.casefold():continue
                data=PMLog();data.size=ctypes.sizeof(data)
                if get_temp(None,adapter.index,ctypes.byref(data))==0 and data.sensors[8].supported==1:
                    samples.append((name,data.sensors[8].value*1000))
            return select_temperature(samples,target)
        finally:
            adl.ADL_Main_Control_Destroy()
    except (OSError,AttributeError,ValueError):return None

if __name__=='__main__': print(json.dumps({'temperature_c':probe()}))
