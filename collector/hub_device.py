"""Project independently aged hub sections to the existing device payload."""
from copy import deepcopy
from datetime import datetime

def to_device(sources, *, generated_at):
    if not isinstance(sources,dict):raise ValueError("invalid hub response")
    try:
        timestamp=datetime.fromisoformat(generated_at.replace("Z","+00:00"))
        if timestamp.tzinfo is None:raise ValueError()
    except (AttributeError,TypeError,ValueError):raise ValueError("invalid timestamp")
    result={"status":"fresh","generated_at":generated_at}
    for name,target in (("codex","codex"),("dorm_pc","dorm_pc"),("zhiqing","zhiqing"),("aliyun","aliyun"),("daily","weather"),("schedule","schedule")):
        section=sources.get(name)
        fresh=isinstance(section,dict) and section.get("status")=="fresh" and isinstance(section.get("data"),dict)
        result[target]=deepcopy(section["data"]) if fresh else {}
        if fresh and name in ("dorm_pc","aliyun") and isinstance(section.get("updated_at"),str):
            result[target]["updated_at"]=section["updated_at"]
    return result
