import { test } from 'node:test';
import assert from 'node:assert/strict';
import { TaskStatusTracker } from './task_status_tracker.mjs';

test('maps live state and approval flags from the owning server',()=>{
 const t=new TaskStatusTracker();t.seed('a',{type:'active',activeFlags:[]},1000);
 assert.equal(t.read('a',1001),'running');
 t.event({method:'thread/status/changed',params:{threadId:'a',status:{type:'active',activeFlags:['waitingOnApproval']}}},1002);
 assert.equal(t.read('a',1003),'waiting_approval');
 t.event({method:'thread/status/changed',params:{threadId:'a',status:{type:'active',activeFlags:['waitingOnUserInput']}}},1004);
 assert.equal(t.read('a',1005),'blocked');
});
test('does not confuse unloaded or idle with completed; terminal status requires matching event',()=>{
 const t=new TaskStatusTracker();t.seed('a',{type:'notLoaded'},1000);assert.equal(t.read('a',1001),'unknown');
 t.event({method:'turn/completed',params:{threadId:'other',turn:{id:'t1',status:'completed'}}},1002);
 t.event({method:'turn/started',params:{threadId:'a',turn:{id:'t1'}}},1002);
 t.event({method:'thread/status/changed',params:{threadId:'a',status:{type:'idle'}}},1003);assert.equal(t.read('a',1004),'idle');
 t.event({method:'turn/completed',params:{threadId:'a',turn:{id:'t1',status:'completed'}}},1005);assert.equal(t.read('a',1006),'completed');
 t.event({method:'thread/status/changed',params:{threadId:'a',status:{type:'notLoaded'}}},1007);assert.equal(t.read('a',1008),'unknown');
});
test('drops state after disconnect, expiration or invalid event',()=>{
 const t=new TaskStatusTracker({maxAge:100});t.seed('a',{type:'active',activeFlags:[]},1000);assert.equal(t.read('a',1101),'unknown');
 t.seed('a',{type:'active',activeFlags:[]},1200);t.disconnect();assert.equal(t.read('a',1201),'unknown');
 t.seed('a',{type:'active',activeFlags:'bad'},1300);assert.equal(t.read('a',1301),'unknown');
});

test('turn lifecycle never reuses a previous completed outcome',()=>{
 const t=new TaskStatusTracker();t.seed('a',{type:'idle'},1000);
 t.event({method:'turn/started',params:{threadId:'a',turn:{id:'t1'}}},1001);assert.equal(t.read('a',1002),'running');
 t.event({method:'turn/completed',params:{threadId:'a',turn:{id:'t1',status:'completed'}}},1003);assert.equal(t.read('a',1004),'unknown');
 t.event({method:'thread/status/changed',params:{threadId:'a',status:{type:'idle'}}},1005);assert.equal(t.read('a',1006),'completed');
 t.event({method:'turn/started',params:{threadId:'a',turn:{id:'t2'}}},1007);assert.equal(t.read('a',1008),'running');
 t.event({method:'turn/completed',params:{threadId:'a',turn:{id:'wrong',status:'failed'}}},1009);assert.equal(t.read('a',1010),'running');
});
