"""Read Codex desktop task metadata through its bundled app-tools MCP."""
import asyncio,hashlib,json,os,re,shutil,sys
from pathlib import Path
from mcp import ClientSession,StdioServerParameters
from mcp.client.stdio import stdio_client


def classify(result):
    thread=result.get('thread') if isinstance(result,dict) else None
    status=thread.get('status') if isinstance(thread,dict) else None
    if not isinstance(status,dict):return 'unknown'
    kind=status.get('type')
    if kind=='active':
        flags=status.get('activeFlags')
        if not isinstance(flags,list) or any(not isinstance(x,str) for x in flags):return 'unknown'
        return 'waiting_approval' if 'waitingOnApproval' in flags else 'blocked' if 'waitingOnUserInput' in flags else 'running'
    if kind=='systemError':return 'error'
    if kind!='idle':return 'unknown'
    turns=result.get('turns')
    last=turns[-1].get('status') if isinstance(turns,list) and turns and isinstance(turns[-1],dict) else None
    return {'completed':'completed','failed':'error','interrupted':'interrupted'}.get(last,'idle')


def effective_status(result, unread):
    state=classify(result)
    return "idle" if state=="completed" and not unread else state

def notice_fields(result, thread_id):
    turns=result.get('turns') if isinstance(result,dict) else None
    turn=turns[-1] if isinstance(turns,list) and turns and isinstance(turns[-1],dict) else {}
    turn_id=turn.get('id')
    if not isinstance(turn_id,str) or not re.fullmatch(r'[0-9a-zA-Z_-]{1,80}',turn_id):return {}
    if not isinstance(thread_id,str) or not re.fullmatch(r'[0-9a-zA-Z_-]{1,80}',thread_id):return {}
    key=hashlib.sha256((thread_id+'/'+turn_id).encode('ascii')).hexdigest()[:32]
    return {'notice_key':key,'notice_state':classify(result)}


def pick(data):
    if not isinstance(data,dict):return []
    threads=(data.get('pinnedThreads') or [])+(data.get('threads') or [])
    rows=[];seen=set()
    for t in threads:
        if not isinstance(t,dict) or t.get('kind')!='codex':continue
        ident=t.get('id');title=t.get('title');updated=t.get('updatedAt')
        if not isinstance(ident,str) or not re.fullmatch(r'[0-9a-zA-Z_-]{1,80}',ident) or ident in seen or not isinstance(title,str) or not title.strip():continue
        seen.add(ident)
        if re.search(r'(?i)api[ _-]*key|password|secret|token|credential|密码|密钥|口令|凭据|[A-Za-z0-9_-]{32,}',title):title='标题已隐藏'
        rows.append({'id':ident,'title':title.strip()[:100],'updated':(updated/1000 if updated>100000000000 else updated) if type(updated) in (int,float) else 0,'unread':t.get('isUnread') is True})
    return sorted(rows,key=lambda r:r['updated'],reverse=True)[:4]


def parse(result):
    if result.isError:raise ValueError('app tool unavailable')
    for item in result.content:
        if isinstance(getattr(item,'text',None),str):
            obj=json.loads(item.text)
            if isinstance(obj,dict):return obj
    raise ValueError('missing app response')


async def probe():
    if not os.environ.get('CODEX_THREAD_ID') or not os.environ.get('CODEX_APP_TOOLS_PIPE_PATH'):return None
    node=shutil.which('node');folder=Path.home()/'.codex/plugins/cache/openai-bundled/codex-app-tools'
    servers=sorted(folder.glob('*/server.mjs'),reverse=True)
    if not node or not servers:return None
    params=StdioServerParameters(command=node,args=[str(servers[0])],env=dict(os.environ))
    try:
        with open(os.devnull,'w') as quiet:
            async with stdio_client(params,errlog=quiet) as (read,write),ClientSession(read,write) as session:
                await session.initialize();catalog=await session.list_tools();names={x.name for x in catalog.tools}
                list_name=next((x for x in names if x.endswith('list_threads')),None)
                read_name=next((x for x in names if x.endswith('read_thread')),None)
                if not list_name or not read_name:return None
                meta={'openai/threadId':os.environ['CODEX_THREAD_ID']}
                rows=pick(parse(await session.call_tool(list_name,{'limit':40},meta=meta)))
                for row in rows:
                    try:
                        result=parse(await session.call_tool(read_name,{'threadId':row['id'],'turnLimit':1,'includeOutputs':False,'maxOutputCharsPerItem':1},meta=meta))
                        row['status']=effective_status(result,row['unread']);row.update(notice_fields(result,row['id']))
                    except (ValueError,TypeError,KeyError,StopIteration):row['status']='unknown'
                return [{'title':r['title'],'status':r['status'],**{k:r[k] for k in ('notice_key','notice_state') if k in r}} for r in rows]
    except Exception:return None

if __name__=='__main__':
    try:result=asyncio.run(asyncio.wait_for(probe(),20))
    except (TimeoutError,KeyboardInterrupt):result=None
    if result is None:sys.exit(1)
    print(json.dumps(result,ensure_ascii=False))
