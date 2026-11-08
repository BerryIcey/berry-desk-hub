import {test} from 'node:test';
import assert from 'node:assert/strict';
import {Schedule} from './scheduler.mjs';

test('each source has independent intervals and bounded failure backoff',()=>{
 const s=new Schedule({pc:10000,codex:300000});
 assert.equal(s.due('pc',0),true); assert.equal(s.begin('pc',0),true); assert.equal(s.due('pc',1),false);
 s.finish('pc',10,true); assert.equal(s.due('pc',10009),false); assert.equal(s.due('pc',10010),true);
 s.begin('codex',0);s.finish('codex',10,false);assert.equal(s.due('codex',600009),false);assert.equal(s.due('codex',600010),true);
 s.begin('codex',600010);s.finish('codex',600020,false);assert.equal(s.due('codex',1800019),false);assert.equal(s.due('codex',1800020),true);
});

test('restoring retry state preserves delay and bounds invalid future values',()=>{
 const first=new Schedule({codex:300000});
 first.begin('codex',0); first.finish('codex',100,false);
 const restarted=new Schedule({codex:300000}); restarted.restore(first.snapshot(),200);
 assert.equal(restarted.due('codex',600099),false);
 assert.equal(restarted.due('codex',600100),true);
 restarted.restore({codex:{next:1e15,failures:99}},200);
 assert.equal(restarted.slots.codex.next,2400200);
 assert.equal(restarted.slots.codex.failures,4);
});
