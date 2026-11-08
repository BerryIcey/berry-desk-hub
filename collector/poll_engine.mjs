import { Schedule } from './scheduler.mjs';

// The caller supplies source functions and durable state storage. No service is
// started merely by importing this module.
export class PollEngine {
  constructor({intervals, sources, readState, writeState, now=Date.now}) {
    this.schedule=new Schedule(intervals);
    this.sources=sources;
    this.readState=readState;
    this.writeState=writeState;
    this.now=now;
    this.values={};
  }
  async init() {
    const saved=await this.readState();
    this.schedule.restore(saved?.schedule,this.now());
    this.values=saved?.values && typeof saved.values==='object' ? saved.values : {};
  }
  async persist() { await this.writeState({schedule:this.schedule.snapshot(),values:this.values}); }
  async poll(key) {
    const start=this.now();
    if (!this.schedule.begin(key,start)) return false;
    // Persist a conservative retry deadline before the external call, so a
    // crash or restart cannot bypass rate limiting.
    const slot=this.schedule.slots[key];
    slot.next=start+slot.interval;
    await this.persist();
    let ok=false;
    try {
      const value=await this.sources[key]();
      if (value == null) throw new Error('unavailable');
      this.values[key]={updated_at:new Date(this.now()).toISOString(),value};
      ok=true;
    } catch {
      // Retain timestamped previous data: consumers must apply freshness rules.
    } finally {
      this.schedule.finish(key,this.now(),ok);
      await this.persist();
    }
    return ok;
  }
}
