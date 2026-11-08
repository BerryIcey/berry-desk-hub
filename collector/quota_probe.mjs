import {AppServerClient} from '../berry-hub-reference-codex-meter/src/app-server-client.mjs';
import {quotaSnapshot} from './snapshot.mjs';
const executable=process.env.BERRY_CODEX_EXECUTABLE;
if(!executable)process.exit(1);
const client=new AppServerClient({command:executable});
try {
  await client.start();
  const response=await client.request('account/rateLimits/read');
  const quota=quotaSnapshot(response.rateLimits ?? {});
  if(!quota.some(x=>x.remaining_percent!==null))process.exitCode=1;
  else console.log(JSON.stringify({updated_at:new Date().toISOString(),source:'codex-app-server',status:'available',quota_windows:quota}));
} catch {process.exitCode=1;}
finally {await client.stop();}
