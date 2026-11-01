from pathlib import Path
import datetime,hashlib,json,os,secrets,subprocess,sys
build=Path(sys.argv[1]);v=Path('/data/neon-build/validation');sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
br=json.loads((build/'result.json').read_bytes());assert br.get('build_exit')==0 and not br.get('driver_error'),br
nonce=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')+'-'+secrets.token_hex(3);p=v/('pg18-connection1-native-'+nonce);p.mkdir(mode=0o755);(p/'evidence').mkdir();os.chown(p/'evidence',1000,1000)
lock=json.loads((build/'source-lock.json').read_bytes());(p/'source-lock.json').write_text(json.dumps({'build':str(build),'compile_lock_sha256':sha(build/'source-lock.json'),'owned':lock['owned10'],'postgres_sha256':sha(build/'install/bin/postgres'),'test_DSO_sha256':sha(build/'install/lib/postgresql/test_dsg_region18.so'),'test_python_sha256':sha(build/'source/src/test/modules/test_dsg_region18/t/test_connection18.py'),'scope':'ACTUAL_PG18_CONNECTION1_ONLY_NOT_SOURCE_MEMORYCONTEXT_BUDGET_OR_NORMAL'},sort_keys=True,indent=2)+'\n')
runner=r'''from pathlib import Path
import json,sys,unittest
sys.path.insert(0,'/src/src/test/modules/test_dsg_region18/t')
import test_connection18
class Exact(unittest.TextTestResult):
 def __init__(self,*a,**k):super().__init__(*a,**k);self.passed=[];self.subtests=[]
 def addSuccess(self,t):self.passed.append(t.id());super().addSuccess(t)
 def addSubTest(self,t,s,e):self.subtests.append({'id':str(s),'passed':e is None});super().addSubTest(t,s,e)
suite=unittest.defaultTestLoader.loadTestsFromTestCase(test_connection18.Connection)
r=unittest.TextTestRunner(verbosity=2,resultclass=Exact).run(suite)
Path('/evidence/test-result.json').write_text(json.dumps({'testsRun':r.testsRun,'passed':r.passed,'subtests':r.subtests,'failures':[{'id':str(t),'traceback':e}for t,e in r.failures],'errors':[{'id':str(t),'traceback':e}for t,e in r.errors],'skips':r.skipped,'Source':False,'normal':False,'E':0},indent=2)+'\n')
raise SystemExit(0 if r.wasSuccessful()and r.testsRun==6 and len(r.passed)==6 and not r.skipped else 1)
'''
(p/'runner.py').write_text(runner);(p/'metadata.json').write_text(json.dumps({'build':str(build),'scope':'CONNECTION1_ACTUAL_PM_GATE_ONLY_NO_NORMAL'},indent=2)+'\n')
worker=r'''from pathlib import Path
import json,subprocess,time,hashlib,traceback
p=Path(__file__).parent;build=Path(json.loads((p/'metadata.json').read_bytes())['build']);start=time.monotonic();name='pg18-connection1-'+p.name[-21:].lower();result={'Source':False,'normal':False,'E':0}
image='127.0.0.1:5001/neon/pg18-native-tools@sha256:68c6b14f76f04bdcebbf15dba0c97de2bdcb2bed5850654797cc6d631f694ee6'
cmd=['docker','run','--name',name,'--pull=never','--network','none','--read-only','--cap-drop','ALL','--security-opt','no-new-privileges','--user','1000:1000','--cpus','2','--memory','3g','--memory-swap','3g','--pids-limit','256','--tmpfs','/tmp:rw,nosuid,nodev,size=1g,mode=1777','-v',str(build/'source')+':/src:ro','-v',str(build/'install')+':/opt/pg18:ro','-v',str(p)+':/report:ro','-v',str(p/'evidence')+':/evidence:rw','-e','NEON_TEST_PG_BIN=/opt/pg18/bin','-e','NEON_TEST_DSG_LIBRARY=/opt/pg18/lib/postgresql/test_dsg_region18.so','-e','NEON_PG18_EVIDENCE=/evidence','-e','PYTHONDONTWRITEBYTECODE=1','--entrypoint','/bin/bash',image,'-c','set +e; python3 /report/runner.py; rc=$?; for metric in cpu.stat memory.peak io.stat memory.events; do cat /sys/fs/cgroup/$metric >/evidence/$metric; done; exit "$rc"']
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
'''
compile(worker,'worker.py','exec');(p/'worker.py').write_text(worker);unit='neon-pg18-connection1-native-'+nonce.lower()+'.service';spec={'protocol_version':1,'nonce':nonce,'unit':unit,'kind':'native','evidence_dir':str(p),'argv':['/usr/bin/python3',str(p/'worker.py')]};(p/'spec.json').write_text(json.dumps(spec,indent=2)+'\n');(p/'spec.json').chmod(0o600);cli=Path('/data/neon-build/continuous/resource-budget/budget.py');assert sha(cli)=='a6b017b60469706e58a150e90820e72e96cbb1052e8150459ca7c10d0226b057'
for op in ['reserve','launch']:
 x=subprocess.run(['/usr/bin/python3',str(cli),op,'--spec',str(p/'spec.json')],capture_output=True,text=True);(p/(op+'-receipt.json')).write_text(json.dumps({'exit':x.returncode,'stdout':x.stdout,'stderr':x.stderr},indent=2)+'\n');print(op,x.returncode,x.stdout.strip())
 if x.returncode:break
(build/'auto-native-run.json').write_text(json.dumps({'path':str(p),'unit':unit,'source_lock_sha256':sha(p/'source-lock.json')},indent=2)+'\n');(v/'pg18-connection1-native-active.txt').write_text(str(p)+'\n');print('NATIVE',str(p),unit)
