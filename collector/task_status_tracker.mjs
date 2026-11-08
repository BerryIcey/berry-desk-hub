// Adapter for events from the owning app-server; a separate server cannot supply desktop state.
export class TaskStatusTracker {
  constructor({maxAge=60000}={}) { this.maxAge=maxAge;this.items=new Map(); }
  seed(id,status,now) {
    if(typeof id!=='string' || !id || !Number.isFinite(now))return;
    this.items.set(id,{status:this.validate(status),at:now,turn:null,terminal:null});
  }
  validate(status) {
    if(!status || typeof status!=='object')return {type:'notLoaded'};
    if(status.type==='active') {
      if(!Array.isArray(status.activeFlags) || status.activeFlags.some(x=>typeof x!=='string'))return {type:'notLoaded'};
      return {type:'active',activeFlags:[...status.activeFlags]};
    }
    return {type:['idle','notLoaded','systemError'].includes(status.type)?status.type:'notLoaded'};
  }
  event(event,now) {
    if(!Number.isFinite(now) || !event || typeof event!=='object')return;
    const p=event.params??{},entry=this.items.get(p.threadId);if(!entry)return;
    if(event.method==='thread/status/changed') {
      entry.status=this.validate(p.status);entry.at=now;
      if(entry.status.type==='active' && !entry.turn)entry.terminal=null;
    } else if(event.method==='turn/started' && typeof p.turn?.id==='string' && p.turn.id) {
      entry.turn=p.turn.id;entry.terminal=null;entry.status={type:'active',activeFlags:[]};entry.at=now;
    } else if(event.method==='turn/completed' && entry.turn && p.turn?.id===entry.turn) {
      entry.terminal=['completed','failed','interrupted'].includes(p.turn.status)?p.turn.status:null;entry.turn=null;if(entry.status.type==='active')entry.status={type:'notLoaded'};entry.at=now;
    }
  }
  read(id,now) {
    const e=this.items.get(id);if(!e || !Number.isFinite(now) || now<e.at || now-e.at>this.maxAge)return 'unknown';
    if(e.status.type==='systemError')return 'error';
    if(e.status.type==='active')return e.status.activeFlags.includes('waitingOnApproval')?'waiting_approval':e.status.activeFlags.includes('waitingOnUserInput')?'blocked':'running';
    if(e.status.type==='idle')return e.terminal==='completed'?'completed':e.terminal==='failed'?'error':e.terminal==='interrupted'?'interrupted':'idle';
    return 'unknown';
  }
  disconnect() { this.items.clear(); }
}
