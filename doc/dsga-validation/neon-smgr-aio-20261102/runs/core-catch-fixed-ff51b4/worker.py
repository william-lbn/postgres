from pathlib import Path
import json,subprocess,time,os,hashlib,shutil
p=Path(__file__).parent
sha=lambda f:hashlib.sha256(Path(f).read_bytes()).hexdigest()
raw=subprocess.check_output(['/bin/bash','-c','set -a; source /data/neon-build/config/env.sh >/dev/null; env -0'])
env=os.environ.copy();env.update({k.decode():v.decode()for x in raw.split(b'\0')if x and b'='in x for k,v in [x.split(b'=',1)]})
name=p.name.lower();start=time.monotonic();image='127.0.0.1:5001/neon/pg18-native-tools@sha256:68c6b14f76f04bdcebbf15dba0c97de2bdcb2bed5850654797cc6d631f694ee6'
cmd=['docker','run','--name',name,'--pull=never','--network=none','--read-only','--cap-drop=ALL','--security-opt=no-new-privileges','--cpus=6','--memory=8g','--memory-swap=8g','--pids-limit=256','--user=1000:1000','--tmpfs','/tmp:rw,nosuid,nodev,size=1g','-v',str(p/'pg-source')+':/src:ro','-v',str(p)+':/report:ro','-v',str(p/'build')+':/build:rw','-v',str(p/'install')+':/opt/pg18:rw','-v',str(p/'evidence')+':/evidence:rw','-v','/data/neon-build/cache/pg18-ccache:/ccache:rw','--entrypoint','/bin/bash',image,'/report/core-build.sh']
(p/'core-command.json').write_text(json.dumps(cmd,indent=2)+'\n')
with(p/'core-build.log').open('wb')as log:q=subprocess.run(cmd,env=env,stdout=log,stderr=subprocess.STDOUT)
o=json.loads(subprocess.check_output(['docker','inspect',name],env=env,text=True))[0];(p/'core-container.json').write_text(json.dumps({'ID':o['Id'],'Image':o['Image'],'State':o['State'],'HostConfig':{k:o['HostConfig'][k]for k in ['Memory','MemorySwap','NanoCpus','ReadonlyRootfs','NetworkMode','PidsLimit','SecurityOpt','CapDrop']}},indent=2)+'\n')
z=subprocess.run(['docker','rm',name],env=env,capture_output=True,text=True);(p/'core-cleanup.json').write_text(json.dumps({'exit':z.returncode,'stdout':z.stdout,'stderr':z.stderr},indent=2)+'\n')
lock=json.loads((p/'source-lock.json').read_text());old=Path(lock['old_build'])
unchanged={n:sha(old/'build'/n)==h for n,h in lock['original_mutable'].items()};assert all(unchanged.values()),unchanged
(p/'original-cache-preserved.json').write_text(json.dumps(unchanged,indent=2)+'\n')
if q.returncode:
 (p/'result.json').write_text(json.dumps({'core_build_exit':q.returncode,'extension_make_exit':None,'seconds':time.monotonic()-start,'normal':False,'Source':False,'E':0},indent=2)+'\n');raise SystemExit(1)
result={'core_build_exit':0,'postgres_sha256':sha(p/'install/bin/postgres'),'test_DSO_sha256':sha(p/'install/lib/postgresql/test_dsg_region18.so'),'seconds':time.monotonic()-start,'scope':'ERROR_CATCH_HOLDOFF_ONLY_NO_EXTENSION_REBUILD','normal':False,'Source':False,'E':0}
(p/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True)
