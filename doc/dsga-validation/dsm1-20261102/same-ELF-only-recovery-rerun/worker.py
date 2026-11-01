from pathlib import Path
import json,subprocess,time,hashlib,traceback
p=Path(__file__).parent;build=Path(json.loads((p/'metadata.json').read_bytes())['build']);start=time.monotonic();name='pg18-dsm1-'+p.name[-21:].lower();result={'Source':False,'normal':False,'E':0}
image='127.0.0.1:5001/neon/pg18-native-tools@sha256:68c6b14f76f04bdcebbf15dba0c97de2bdcb2bed5850654797cc6d631f694ee6'
cmd=['docker','run','--name',name,'--pull=never','--network','none','--read-only','--cap-drop','ALL','--security-opt','no-new-privileges','--user','1000:1000','--cpus','2','--memory','3g','--memory-swap','3g','--pids-limit','256','--tmpfs','/tmp:rw,nosuid,nodev,size=1g,mode=1777','-v',json.loads((p/'metadata.json').read_bytes())['runtime_source']+':/src:ro','-v',str(build/'install')+':/opt/pg18:ro','-v',str(p)+':/report:ro','-v',str(p/'evidence')+':/evidence:rw','-e','NEON_TEST_PG_BIN=/opt/pg18/bin','-e','NEON_TEST_DSG_LIBRARY=/opt/pg18/lib/postgresql/test_dsg_region18.so','-e','NEON_PG18_EVIDENCE=/evidence','-e','PYTHONDONTWRITEBYTECODE=1','--entrypoint','/bin/bash',image,'-c','set +e; python3 /report/runner.py; rc=$?; for metric in cpu.stat memory.peak io.stat memory.events; do cat /sys/fs/cgroup/$metric >/evidence/$metric; done; exit "$rc"']
(p/'command.json').write_text(json.dumps(cmd,indent=2)+'\n')
try:
 with(p/'run.log').open('w')as log:result['native_exit']=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,timeout=240).returncode
 x=subprocess.run(['docker','inspect',name],capture_output=True,text=True)
 if x.returncode==0:
  o=json.loads(x.stdout)[0];(p/'container.json').write_text(json.dumps({'ID':o['Id'],'Image':o['Image'],'State':o['State'],'HostConfig':{k:o['HostConfig'].get(k)for k in ['Memory','MemorySwap','NanoCpus','ReadonlyRootfs','NetworkMode','PidsLimit','SecurityOpt','CapDrop','Tmpfs']}},indent=2)+'\n');assert not o['State']['OOMKilled']
except BaseException:
 result['driver_error']=True;(p/'driver-error.txt').write_text(traceback.format_exc())
finally:
 x=subprocess.run(['docker','rm','-f',name],capture_output=True,text=True);(p/'cleanup.json').write_text(json.dumps({'exit':x.returncode,'stdout':x.stdout,'stderr':x.stderr},indent=2)+'\n')
result['seconds']=time.monotonic()-start;(p/'result.json').write_text(json.dumps(result,indent=2)+'\n');raise SystemExit(0 if result.get('native_exit')==0 and not result.get('driver_error')else 1)
