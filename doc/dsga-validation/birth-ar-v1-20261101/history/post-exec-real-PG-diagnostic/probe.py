import sys,os,json,time,subprocess
from pathlib import Path
sys.path.insert(0,'/src/src/test/modules/test_dsg_region18/t')
import test_birth18 as birth
import test_array_root18 as core
pg=birth.Cluster()
readfd,writefd=os.pipe2(os.O_CLOEXEC)
def prepare():
 os.setresgid(1000,1000,1000)
 os.setresuid(1000,0,0)
 birth.nnp()
 raw={'real_effective_saved_uid':os.getresuid(),'real_effective_saved_gid':os.getresgid(),'nnp':birth.libc.prctl(39,0,0,0,0)}
 os.write(writefd,(json.dumps(raw)+'\n').encode());os.set_inheritable(writefd,False)
try:
 env=core.environment();env['NEON_DSGA_EARLY_ARRAYS_PROFILE']='test-v1:2048';env['NEON_DSGA_ADDRESS_SPACE_MAX_BYTES']=str(birth.AS)
 pg.stream=pg.log.open('a')
 pg.process=subprocess.Popen([str(core.BIN/'postgres'),'-D',str(pg.data)],env=env,stdout=pg.stream,stderr=subprocess.STDOUT,preexec_fn=prepare,pass_fds=(writefd,),user=0,group=0,extra_groups=[])
 os.close(writefd);writefd=-1
 pre=json.loads(os.read(readfd,4096))
 until=time.monotonic()+15
 while time.monotonic()<until:
  if pg.process.poll() is not None:raise RuntimeError(pg.logs())
  if pg.sql('SELECT 1',check=False).returncode==0:break
  time.sleep(.025)
 else:raise RuntimeError('probe startup deadline '+pg.logs())
 pg.sql('CREATE EXTENSION test_dsg_region18')
 status=Path(f'/proc/{pg.process.pid}/status').read_text()
 limits=Path(f'/proc/{pg.process.pid}/limits').read_text()
 facts=pg.birth()
 out={'pre_exec_kernel_calls':pre,'post_exec_status':status,'post_exec_limits':limits,'birth':facts,'PG_ready':True,'scope':'diagnostic actual post-exec credentials only; no Source or normal auth'}
 Path('/evidence/actual.json').write_text(json.dumps(out,indent=2)+'\n')
 print(json.dumps({'pre_exec':pre,'post_exec_Uid':next(x for x in status.splitlines()if x.startswith('Uid:')),'post_exec_Gid':next(x for x in status.splitlines()if x.startswith('Gid:')),'NoNewPrivs':next(x for x in status.splitlines()if x.startswith('NoNewPrivs:')),'PG_ready':True,'root_enrolled':facts['root_enrolled']},indent=2))
finally:
 if writefd>=0:os.close(writefd)
 os.close(readfd);pg.close()
