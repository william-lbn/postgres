from pathlib import Path
import subprocess,json,time,shutil,os,traceback,hashlib
p=Path(__file__).parent;meta=json.loads((p/'metadata.json').read_bytes());start=time.monotonic();result={'source':False,'normal':False,'S':0};name='pg18-dsm1-build-'+p.name[-21:].lower();image='127.0.0.1:5001/neon/pg18-native-tools@sha256:68c6b14f76f04bdcebbf15dba0c97de2bdcb2bed5850654797cc6d631f694ee6'
try:
 for label in ['build','install']:
  (p/label).mkdir()
 (p/'evidence').mkdir()
 for label in ['build','install','evidence']:
  root=p/label;os.chown(root,1000,1000)
  for q in root.rglob('*'):
   if q.is_symlink():os.lchown(q,1000,1000)
   else:os.chown(q,1000,1000)
 cmd=['docker','run','--name',name,'--pull=never','--network','none','--read-only','--cap-drop','ALL','--security-opt','no-new-privileges','--user','1000:1000','--cpus','6','--memory','8g','--memory-swap','8g','--pids-limit','256','--tmpfs','/tmp:rw,nosuid,nodev,size=1g','-v',str(p/'source')+':/src:ro','-v',str(p)+':/report:ro','-v',str(p/'build')+':/build:rw','-v',str(p/'install')+':/opt/pg18:rw','-v',str(p/'evidence')+':/evidence:rw','-v','/data/neon-build/cache/pg18-ccache:/ccache:rw','--entrypoint','/bin/bash',image,'/report/build.sh']
 (p/'build-command.json').write_text(json.dumps(cmd,indent=2)+'\n')
 with(p/'build.log').open('w')as log:result['build_exit']=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,timeout=900).returncode
 x=subprocess.run(['docker','inspect',name],capture_output=True,text=True)
 if x.returncode==0:
  o=json.loads(x.stdout)[0];(p/'container.json').write_text(json.dumps({'ID':o['Id'],'Image':o['Image'],'State':o['State'],'HostConfig':{k:o['HostConfig'].get(k)for k in ['Memory','MemorySwap','NanoCpus','ReadonlyRootfs','NetworkMode','PidsLimit','SecurityOpt','CapDrop','Tmpfs']}},indent=2)+'\n');assert not o['State']['OOMKilled']
 if result.get('build_exit')==0:
  for n in ['postgres','pg_config']:
   q=p/'install/bin'/n;result[n+'_sha256']=hashlib.sha256(q.read_bytes()).hexdigest()
  result['test_DSO_sha256']=hashlib.sha256((p/'install/lib/postgresql/test_dsg_region18.so').read_bytes()).hexdigest()
except BaseException:
 result['driver_error']=True;(p/'driver-error.txt').write_text(traceback.format_exc())
finally:
 x=subprocess.run(['docker','rm','-f',name],capture_output=True,text=True);(p/'cleanup.json').write_text(json.dumps({'exit':x.returncode,'stdout':x.stdout,'stderr':x.stderr},indent=2)+'\n')
result['seconds']=time.monotonic()-start;(p/'result.json').write_text(json.dumps(result,indent=2)+'\n')
if result.get('build_exit')==0 and not result.get('driver_error'):
 starter=Path('/data/neon-build/validation/pg18-dsm1-planning-20261102/native-start.py')
 assert hashlib.sha256(starter.read_bytes()).hexdigest()==meta['native_starter_sha256']
 with(p/'native-chain.log').open('w')as log:result['native_launch_exit']=subprocess.run(['/usr/bin/python3',str(starter),str(p)],stdout=log,stderr=subprocess.STDOUT).returncode
 (p/'result.json').write_text(json.dumps(result,indent=2)+'\n')
raise SystemExit(0 if result.get('build_exit')==0 and not result.get('driver_error')and result.get('native_launch_exit')==0 else 1)
