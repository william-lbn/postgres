from pathlib import Path
import subprocess,json,time,sys,os,signal,traceback,hashlib
v=Path(__file__).parent
image='127.0.0.1:5001/neon/pg18-native-tools@sha256:68c6b14f76f04bdcebbf15dba0c97de2bdcb2bed5850654797cc6d631f694ee6'
suffix=v.name[-21:].lower()
active=[]
def run(name,cmd,limit):
 cname='pg18-ar-'+name+'-'+suffix
 cmd=cmd[:2]+['--name',cname]+cmd[2:]
 active.append(cname)
 (v/(name+'-command.json')).write_text(json.dumps(cmd,indent=2)+'\n')
 start=time.monotonic();rc=124
 try:
  with(v/(name+'.log')).open('wb')as f:rc=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,timeout=limit).returncode
 except subprocess.TimeoutExpired:rc=124
 finally:
  x=subprocess.run(['docker','inspect',cname],capture_output=True,text=True)
  if x.returncode==0:
   obj=json.loads(x.stdout)[0]
   selected={'Image':obj['Image'],'State':obj['State'],'HostConfig':{k:obj['HostConfig'].get(k)for k in ['Memory','MemorySwap','NanoCpus','NetworkMode','ReadonlyRootfs','CapDrop','CapAdd','SecurityOpt','PidsLimit','Tmpfs']}}
   (v/(name+'-container.json')).write_text(json.dumps(selected,indent=2)+'\n')
   if obj['State'].get('OOMKilled'):rc=137
  subprocess.run(['docker','rm','-f',cname],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
  active.remove(cname)
 (v/(name+'-result.json')).write_text(json.dumps({'exit':rc,'seconds':time.monotonic()-start},indent=2)+'\n')
 return rc
def terminate(sig,frame):raise KeyboardInterrupt('signal '+str(sig))
signal.signal(signal.SIGTERM,terminate)
result={}
try:
 build=['docker','run','--pull=never','--network','none','--read-only','--cap-drop','ALL','--security-opt','no-new-privileges','--user','1000:1000','--cpus','4','--memory','5g','--memory-swap','5g','--pids-limit','256','--tmpfs','/tmp:rw,nosuid,nodev,size=1g','-v',str(v/'source')+':/src:ro','-v',str(v)+':/report:ro','-v',str(v/'build')+':/build:rw','-v',str(v/'install')+':/opt/pg18:rw','-v',str(v/'evidence')+':/evidence:rw','-v','/data/neon-build/cache/pg18-ccache:/ccache:rw','--entrypoint','/bin/bash',image,'/report/build.sh']
 result['build']=run('build',build,900)
 if result['build']==0:
  for name in ['filter5','root9','birth6','new4','old30','uring1']:
   caps=['--user','0:0','--cap-add','SYS_ADMIN','--cap-add','SYS_PTRACE','--cap-add','SETUID','--cap-add','SETGID','--cap-add','CHOWN','--cap-add','DAC_OVERRIDE','--cap-add','FOWNER','--cap-add','KILL','--security-opt','seccomp=unconfined'] if name in ['filter5','root9','birth6','new4'] else ['--user','1000:1000','--security-opt','no-new-privileges']
   if name=='uring1':caps+=['--security-opt','seccomp='+str(v/'source/src/test/modules/test_dsg_region18/seccomp-uring3-test.json')]
   cmd=['docker','run']+(['--init'] if name in ['filter5','root9','birth6','new4'] else [])+['--pull=never','--network','none','--read-only','--cap-drop','ALL','--cpus','4','--memory','5g','--memory-swap','5g','--pids-limit','512','--tmpfs','/tmp:rw,nosuid,nodev,size=1g,mode=1777','--tmpfs','/run:rw,nosuid,nodev,size=16m,mode=755']+caps+['-v',str(v/'source')+':/src:ro','-v',str(v)+':/report:ro','-v',str(v/'install')+':/opt/pg18:ro','-v',str(v/'evidence'/name)+':/evidence:rw','-e','NEON_TEST_PG_BIN=/opt/pg18/bin','-e','NEON_TEST_DSG_LIBRARY=/opt/pg18/lib/postgresql/test_dsg_region18.so','-e','NEON_PG18_EVIDENCE=/evidence','-e','NEON_ARRAY_ROOT_EVIDENCE=/evidence','-e','NEON_ROOT_WIRE_EVIDENCE=/evidence','-e','NEON_DSGA_ROOT_COMPONENT_TEST=1','-e','PYTHONDONTWRITEBYTECODE=1','--entrypoint','/usr/bin/python3',image,'/report/runner.py',name]
   result[name]=run(name,cmd,300)
except BaseException as e:
 result['driver_error']=repr(e)
 (v/'driver-traceback.txt').write_text(traceback.format_exc())
finally:
 for name in active:subprocess.run(['docker','rm','-f',name],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 rc=0 if result and all(isinstance(x,int)and x==0 for x in result.values()) and len(result)==7 else 1
 (v/'terminal.json').write_text(json.dumps({'exit':rc,'phases':result,'not_proven':['production PG18 original Source/private WAL/authorization','original Root Source registry/budget/WAL','physical disk durability','safe normal decrease','KVM','K8s E2E']},indent=2)+'\n')
 (v/'test.rc').write_text(str(rc)+'\n')
 raise SystemExit(rc)
