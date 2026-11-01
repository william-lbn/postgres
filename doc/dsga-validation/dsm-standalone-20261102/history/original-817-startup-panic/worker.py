from pathlib import Path
import subprocess,json,time,traceback
p=Path(__file__).parent;lock=json.loads((p/'source-lock.json').read_text());start=time.monotonic();name=p.name.lower();result={'Source':False,'normal':False,'E':0,'production_changed':False}
cmd=['docker','run','--name',name,'--pull=never','--network=none','--read-only','--cap-drop=ALL','--security-opt=no-new-privileges','--cpus=1','--memory=512m','--memory-swap=512m','--pids-limit=128','--shm-size=64m','--ulimit','core=0','--user','1000:1000','--tmpfs','/tmp:rw,nosuid,nodev,size=128m,mode=1777','-v',str(p/'probe')+':/probe:ro','-v',str(p/'evidence')+':/evidence:rw','-v',str(p/'run.sh')+':/run-repro.sh:ro','--entrypoint','/bin/sh',lock['PG17_image'],'/run-repro.sh']
(p/'command.json').write_text(json.dumps(cmd,indent=2)+'\n')
try:
 with(p/'run.log').open('w')as h:rc=subprocess.run(cmd,stdout=h,stderr=subprocess.STDOUT,timeout=90).returncode
 result['container_unique_Wait_exit']=rc
 def code(n):return int((p/'evidence'/n).read_text())if(p/'evidence'/n).exists()else None
 result.update({n:code(n+'.exitcode') for n in ['compile','initdb','startup','repro']})
 text=(p/'evidence/repro-console.log').read_text(errors='replace')if(p/'evidence/repro-console.log').exists()else''
 result['exact_publication_PANIC']='PANIC:  DSGA DSM publication lost its original reservation'in text
 result['status']='REPRODUCED_NEW_HOOK_STANDALONE_FAILURE'if rc==0 and result['compile']==0 and result['initdb']==0 and result['startup']==0 and result['repro']==134 and result['exact_publication_PANIC']else'NOT_REPRODUCED_OR_FIXTURE_FAILURE'
 o=json.loads(subprocess.check_output(['docker','inspect',name],text=True))[0];(p/'container.json').write_text(json.dumps({'ID':o['Id'],'Image':o['Image'],'State':o['State'],'HostConfig':{k:o['HostConfig'].get(k)for k in ['Memory','MemorySwap','NanoCpus','ReadonlyRootfs','NetworkMode','SecurityOpt','CapDrop','CapAdd','PidsLimit','Ulimits','ShmSize']}},indent=2)+'\n');result['OOMKilled']=o['State']['OOMKilled']
except BaseException:
 result['driver_error']=True;(p/'driver-error.txt').write_text(traceback.format_exc())
finally:
 r=subprocess.run(['docker','rm','-f',name],capture_output=True,text=True);(p/'cleanup.json').write_text(json.dumps({'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr},indent=2)+'\n')
result['seconds']=time.monotonic()-start;(p/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True);raise SystemExit(0 if result.get('status')=='REPRODUCED_NEW_HOOK_STANDALONE_FAILURE' and not result.get('OOMKilled')else 1)
