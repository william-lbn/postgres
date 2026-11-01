from pathlib import Path
import subprocess,json,time,re,traceback
p=Path(__file__).parent;lock=json.loads((p/'source-lock.json').read_text());name=p.name.lower();start=time.monotonic();result={'Source':False,'normal':False,'E':0,'supplements':'PG17-S-DSM-STANDALONE-AND-CAP-001','counts_as_new_root':False}
cmd=['docker','run','--name',name,'--pull=never','--network=none','--read-only','--cap-drop=ALL','--security-opt=no-new-privileges','--cpus=1','--memory=512m','--memory-swap=512m','--pids-limit=128','--shm-size=64m','--ulimit','core=0','--user','1000:1000','--tmpfs','/tmp:rw,nosuid,nodev,size=128m,mode=1777','-v',lock['original_target_run']+'/evidence:/probe:ro','-v',str(p/'evidence')+':/evidence:rw','-v',str(p/'run.sh')+':/run.sh:ro','--entrypoint','/bin/sh',lock['image'],'/run.sh'];(p/'command.json').write_text(json.dumps(cmd,indent=2)+'\n')
try:
 with (p/'run.log').open('w') as h:rc=subprocess.run(cmd,stdout=h,stderr=subprocess.STDOUT,timeout=45).returncode
 result['container_unique_Wait_exit']=rc;o=json.loads(subprocess.check_output(['docker','inspect',name],text=True))[0];result['OOMKilled']=o['State']['OOMKilled'];(p/'container.json').write_text(json.dumps({'ID':o['Id'],'Image':o['Image'],'State':o['State'],'HostConfig':{k:o['HostConfig'].get(k)for k in ['Memory','MemorySwap','NanoCpus','ReadonlyRootfs','NetworkMode','SecurityOpt','CapDrop','PidsLimit']}},indent=2)+'\n')
 assert rc==0 and not result['OOMKilled']
 text=(p/'evidence/standalone-snapshot.log').read_text();assert not re.search('ERROR:|PANIC:|FATAL:',text)
 snap=json.loads(re.search(r'\{[^\n]+\}',text).group(0));assert snap['accounting_valid'] is False and snap['limit_enforced'] is False and snap['reserved_upper_bytes']==0 and snap['external_segments']==0 and snap['pending_creations']==0 and snap['unknown_segments']==0
 actual=(p/'evidence/actual-artifacts.sha256').read_text();assert lock['PG_ELF_sha256'] in actual and lock['test_DSO_sha256'] in actual
 result.update({'status':'PASS_SUPPLEMENTAL_STANDALONE_SNAPSHOT','actual_snapshot':snap})
except BaseException:
 result['status']='FAIL_SUPPLEMENTAL';(p/'driver-error.txt').write_text(traceback.format_exc())
finally:
 r=subprocess.run(['docker','rm','-f',name],capture_output=True,text=True);(p/'cleanup.json').write_text(json.dumps({'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr},indent=2)+'\n')
result['seconds']=time.monotonic()-start;(p/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True);raise SystemExit(0 if result.get('status')=='PASS_SUPPLEMENTAL_STANDALONE_SNAPSHOT'else 1)
