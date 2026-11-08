export class Schedule {
  constructor(intervals) {
    this.slots = Object.fromEntries(Object.entries(intervals).map(([k,interval])=>[k,{interval,next:0,failures:0,running:false}]));
  }
  due(key,now) { const s=this.slots[key]; return !!s && !s.running && now>=s.next; }
  begin(key,now) { if(!this.due(key,now))return false;this.slots[key].running=true;return true; }
  finish(key,now,ok) {
    const s=this.slots[key];if(!s || !s.running)throw new Error('source was not started');
    s.running=false;s.failures=ok?0:Math.min(s.failures+1,4);
    s.next=now+s.interval*(ok?1:Math.min(2**s.failures,8));
  }
  snapshot() {
    return Object.fromEntries(Object.entries(this.slots).map(([key,s])=>[key,{next:s.next,failures:s.failures}]));
  }
  restore(saved,now) {
    if (!saved || typeof saved !== 'object') return;
    for (const [key,s] of Object.entries(this.slots)) {
      const value=saved[key];
      if (!value || !Number.isFinite(value.next) || !Number.isInteger(value.failures)) continue;
      // Preserve backoff over restarts, but bound corrupt or obsolete state.
      s.next=Math.max(0,Math.min(value.next,now+s.interval*8));
      s.failures=Math.max(0,Math.min(value.failures,4));
    }
  }
}
