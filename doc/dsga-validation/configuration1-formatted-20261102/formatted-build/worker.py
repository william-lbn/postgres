from pathlib import Path
import json,hashlib,subprocess,time,traceback,os
p=Path(__file__).parent;m=json.loads((p/'metadata.json').read_bytes());old=Path(m['old_build']);r=Path('/data/neon-build/src/postgres-v18');start=time.monotonic();sha=lambda q:hashlib.sha256(q.read_bytes()).hexdigest();result={'Source':False,'normal':False,'E':0};name='pg18-conf1-format-'+p.name[-21:].lower()
try:
 for n,h in m['owned4'].items():assert sha(r/n)==h,(n,'source changed')
 subprocess.run(['cp','-al',str(old/'source'),str(p/'source')],check=True)
 for n,h in m['owned4'].items():
  q=p/'source'/n
  if sha(q)!=h:q.unlink();q.write_bytes((r/n).read_bytes())
 for label in ['build','install']:
  subprocess.run(['cp','-a','--reflink=auto',str(old/label),str(p/label)],check=True)
  a=old/label/('src/backend/storage/buffer/dsg_buffer_arrays.o' if label=='build' else 'bin/postgres');b=p/label/a.relative_to(old/label)
  assert (a.stat().st_dev,a.stat().st_ino)!=(b.stat().st_dev,b.stat().st_ino),'mutable hardlink forbidden'
 (p/'evidence').mkdir();os.chown(p/'evidence',1000,1000)
 files={str(q.relative_to(p/'source')):sha(q)for q in sorted((p/'source').rglob('*'))if q.is_file()and not q.is_symlink()}
 lock=dict(m,actual_inputs=files);(p/'source-lock.json').write_text(json.dumps(lock,indent=2,sort_keys=True)+'\n');(p/'expected-owned4-sha256.txt').write_text(''.join(h+'  '+n+'\n'for n,h in m['owned4'].items()))
 cmd=['docker','run','--name',name,'--pull=never','--network','none','--read-only','--cap-drop','ALL','--security-opt','no-new-privileges','--user','1000:1000','--cpus','6','--memory','8g','--memory-swap','8g','--pids-limit','256','--tmpfs','/tmp:rw,nosuid,nodev,size=1g','-v',str(p/'source')+':/src:ro','-v',str(p)+':/report:ro','-v',str(p/'build')+':/build:rw','-v',str(p/'install')+':/opt/pg18:rw','-v',str(p/'evidence')+':/evidence:rw','-v','/data/neon-build/cache/pg18-ccache:/ccache:rw','--entrypoint','/bin/bash','127.0.0.1:5001/neon/pg18-native-tools@sha256:68c6b14f76f04bdcebbf15dba0c97de2bdcb2bed5850654797cc6d631f694ee6','/report/build.sh']
 (p/'build-command.json').write_text(json.dumps(cmd,indent=2)+'\n')
 with(p/'build.log').open('w')as log:result['build_exit']=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,timeout=180).returncode
 x=subprocess.run(['docker','inspect',name],capture_output=True,text=True)
 if x.returncode==0:
  o=json.loads(x.stdout)[0];(p/'container.json').write_text(json.dumps({'ID':o['Id'],'Image':o['Image'],'State':o['State'],'HostConfig':{k:o['HostConfig'].get(k)for k in ['Memory','MemorySwap','NanoCpus','ReadonlyRootfs','NetworkMode','PidsLimit','SecurityOpt','CapDrop','Tmpfs']}},indent=2)+'\n');assert not o['State']['OOMKilled']
 if result.get('build_exit')==0:
  result['postgres_sha256']=sha(p/'install/bin/postgres');result['test_DSO_sha256']=sha(p/'install/lib/postgresql/test_dsg_region18.so');assert result['test_DSO_sha256']=='cada2bc8f6b62de9e92be5b3863c9603dc2ef099ae53966d1e27b16c065c5b49'
 assert sha(old/'install/bin/postgres')=='e3471d5062fb9dc7e25fdd9093b42a485f20bdf78441ae57a74f133c3e6e5d2e'
 assert sha(Path('/data/neon-build/validation/pg18-configuration1-final-freeze-20261102/review-source-and-docs.json'))==m['preserve_old_freeze_sha256']
except BaseException:
 result['driver_error']=True;(p/'driver-error.txt').write_text(traceback.format_exc())
finally:
 x=subprocess.run(['docker','rm','-f',name],capture_output=True,text=True);(p/'cleanup.json').write_text(json.dumps({'exit':x.returncode,'stdout':x.stdout,'stderr':x.stderr},indent=2)+'\n')
result['seconds']=time.monotonic()-start;(p/'result.json').write_text(json.dumps(result,indent=2)+'\n')
if result.get('build_exit')==0 and not result.get('driver_error'):
 with(p/'native-chain.log').open('w')as log:result['native_launch_exit']=subprocess.run(['/usr/bin/python3',str(p/'native-start.py'),str(p)],stdout=log,stderr=subprocess.STDOUT).returncode
 (p/'result.json').write_text(json.dumps(result,indent=2)+'\n')
raise SystemExit(0 if result.get('build_exit')==0 and not result.get('driver_error')and result.get('native_launch_exit')==0 else 1)
