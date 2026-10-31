#!/usr/bin/env python3
# PG18 TEST_ONLY core arrays and actual worker completion lease; no resize/normal grant.
import importlib.util,json,os,signal,subprocess,time,unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('region18_helper',Path(__file__).with_name('test_region18.py'))
region=importlib.util.module_from_spec(spec);spec.loader.exec_module(region)
class Cluster(region.Cluster):
 def __init__(self,mode='test-v1:2048',method='worker'):
  super().__init__(method);self.mode=mode
  with (self.data/'postgresql.conf').open('a') as f:f.write("logging_collector=on\nlog_directory='logs'\nlog_filename='pg.log'\n")
 def start(self):
  self.stream=self.log.open('a');e=region.env()
  if self.mode is not None:e['NEON_DSGA_PG18_NATIVE_ARRAYS']=self.mode
  self.process=subprocess.Popen([str(region.BIN/'postgres'),'-D',str(self.data)],env=e,stdout=self.stream,stderr=subprocess.STDOUT)
  self.ready();self.sql('CREATE EXTENSION IF NOT EXISTS test_dsg_region18')
 def ready(self):
  until=time.monotonic()+20
  while time.monotonic()<until:
   if self.process.poll() is not None:raise RuntimeError(self.log.read_text())
   if self.sql('SELECT 1',check=False).returncode==0:return
   time.sleep(.03)
  raise RuntimeError('PG18 arrays startup/recovery timeout '+self.log.read_text())
 def snapshot(self):return json.loads(self.sql('SELECT test_dsg_arrays18_snapshot()').stdout)
 def asynchronous(self,sql):
  return subprocess.Popen([str(region.BIN/'psql'),'-h',str(self.sock),'-p','55438','-U',region.USER,'-d','postgres','-XqAt','-v','ON_ERROR_STOP=1','-c',sql],env=region.env(),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
 def close(self):
  if os.environ.get('NEON_PG18_EVIDENCE'):
   target=Path(os.environ['NEON_PG18_EVIDENCE'])/self.root.name;target.mkdir(exist_ok=True)
   for p in (self.data/'logs').glob('*'): (target/p.name).write_bytes(p.read_bytes())
  super().close()
class Arrays(unittest.TestCase):
 def cluster(self,mode='test-v1:2048',method='worker'):
  p=Cluster(mode,method);self.addCleanup(p.close);p.start();return p
 def table(self,p):p.sql('CREATE TABLE array18_data AS SELECT i,repeat(md5(i::text),4) v FROM generate_series(1,150000) i;CHECKPOINT')
 def wait_snapshot(self,p,predicate,seconds=6):
  until=time.monotonic()+seconds
  while time.monotonic()<until:
   s=p.snapshot()
   if predicate(s):return s
   time.sleep(.02)
  self.fail('snapshot predicate timeout '+json.dumps(s))
 def test_PG18_U_ARRAYS_001_strict_profile_rejects(self):
  for raw in ('root-v1:2048','test-v1:0','test-v1:02048','test-v1:511','test-v1:2147483648','test-v1:2048x'):
   with self.subTest(mode=raw):
    p=Cluster(raw);self.addCleanup(p.close);e=region.env();e['NEON_DSGA_PG18_NATIVE_ARRAYS']=raw
    r=subprocess.run([str(region.BIN/'postgres'),'-D',str(p.data)],env=e,capture_output=True,text=True,timeout=10)
    self.assertNotEqual(r.returncode,0);self.assertIn('PG18 native arrays',r.stderr)
 def test_PG18_S_ARRAYS_001_actual_four_native_arrays(self):
  p=self.cluster();s=p.snapshot();self.assertTrue(s['enabled']);self.assertEqual((s['maximum_buffers'],s['startup_buffers']),(2048,512))
  abi=json.loads(p.sql('SELECT test_dsg_region18_abi()').stdout);elements=[int(abi['descriptor_padded_bytes']),8192,int(abi['iocv_bytes']),int(abi['checkpoint_bytes'])]
  for i,a in enumerate(s['arrays']):self.assertEqual(int(a['maximum_bytes']),elements[i]*2048);self.assertEqual(int(a['committed_bytes']),(elements[i]*512+4095)//4096*4096);self.assertEqual(int(a['base'])%4096,0)
  self.assertFalse(s['normal_permit']);self.assertFalse(s['resize_authorized']);self.table(p);self.assertEqual(p.sql('SELECT count(*),sum(i) FROM array18_data').stdout.strip(),'150000|11250075000')
 def test_PG18_S_ARRAYS_002_worker_AIO_data_and_completion(self):
  for method in ('sync','worker'):
   with self.subTest(method=method):
    p=self.cluster(method=method);self.table(p);before=p.snapshot();self.assertEqual(p.sql('SELECT count(*),sum(i) FROM array18_data').stdout.strip(),'150000|11250075000');s=self.wait_snapshot(p,lambda s:s['active_completions']==0)
    if method=='worker':self.assertEqual(int(p.sql("SELECT count(*) FROM pg_stat_activity WHERE backend_type='io worker'").stdout),2);self.assertGreater(int(s['completed_aio']),int(before['completed_aio']));self.assertEqual(s['completed_aio'],s['released_aio_pins'])
    else:
     # PG18 sync仍经pgaio handle/callback；只同步执行，不是绕开release_aio。
     self.assertGreater(int(s['completed_aio']),int(before['completed_aio']));self.assertEqual(s['completed_aio'],s['released_aio_pins'])
    if os.environ.get('NEON_PG18_EVIDENCE'):Path(os.environ['NEON_PG18_EVIDENCE'],'actual-arrays-aio-'+method+'.json').write_text(json.dumps({'method':method,'before':before,'after':s},indent=2)+'\n')
    self.assertTrue(s['completion_lock_available'])
 def test_PG18_S_ARRAYS_003_actual_completion_window_excludes_drain(self):
  p=self.cluster();self.table(p);p.sql('SELECT test_dsg_arrays18_pause(true)');reader=p.asynchronous('SELECT sum(i) FROM array18_data');self.addCleanup(lambda:reader.kill() if reader.poll() is None else None)
  try:
   s=self.wait_snapshot(p,lambda s:s['paused_completions']>0,seconds=4);self.assertGreater(s['active_completions'],0);self.assertGreater(int(s['released_aio_pins']),0);self.assertFalse(s['completion_lock_available']);self.assertFalse(s['resize_authorized'])
  finally:p.sql('SELECT test_dsg_arrays18_pause(false)')
  out,err=reader.communicate(timeout=10);self.assertEqual(reader.returncode,0,err);self.assertEqual(out.strip(),'11250075000');s=self.wait_snapshot(p,lambda s:s['active_completions']==0);self.assertTrue(s['completion_lock_available']);self.assertEqual(s['completed_aio'],s['released_aio_pins'])
 def test_PG18_S_ARRAYS_004_original_PM_recovery_three_times(self):
  p=self.cluster();self.table(p);first=p.snapshot();pm=p.process.pid
  for cycle in range(3):
   child=int(p.sql('SELECT pg_backend_pid()').stdout);holder=p.asynchronous('SELECT pg_backend_pid(),pg_sleep(20)');time.sleep(.1)
   # exact live external backend; do not signal an already exited psql connection.
   victim=int(p.sql("SELECT pid FROM pg_stat_activity WHERE query LIKE 'SELECT pg_backend_pid(),pg_sleep(20)%'").stdout.strip());os.kill(victim,signal.SIGKILL);holder.communicate(timeout=10)
   time.sleep(.15);p.ready();s=p.snapshot();self.assertEqual(p.process.pid,pm);self.assertEqual([a['base'] for a in s['arrays']],[a['base'] for a in first['arrays']]);self.assertEqual(p.sql('SELECT count(*),sum(i) FROM array18_data').stdout.strip(),'150000|11250075000');self.assertEqual(int(p.sql("SELECT count(*) FROM pg_stat_activity WHERE backend_type='io worker'").stdout),2)
 def test_PG18_S_ARRAYS_005_default_off_and_new_API_ACL(self):
  p=self.cluster(None);self.assertFalse(p.snapshot()['enabled']);p.sql('CREATE ROLE arrays18_ordinary LOGIN')
  for fn in ('test_dsg_arrays18_snapshot()','test_dsg_arrays18_pause(boolean)'):self.assertEqual(p.sql("SELECT has_function_privilege('arrays18_ordinary','"+fn+"','EXECUTE')").stdout.strip(),'f')
  p.sql('GRANT EXECUTE ON FUNCTION test_dsg_arrays18_snapshot(),test_dsg_arrays18_pause(boolean) TO arrays18_ordinary')
  p.sql(f"CREATE FUNCTION arrays18_definer() RETURNS text AS '{region.LIB}','test_dsg_arrays18_snapshot' LANGUAGE C SECURITY DEFINER")
  for sql in ('SET ROLE arrays18_ordinary; SELECT test_dsg_arrays18_snapshot()','SET SESSION AUTHORIZATION arrays18_ordinary;SELECT arrays18_definer()'):
   r=p.sql(sql,check=False);self.assertNotEqual(r.returncode,0);self.assertIn('original administrator',r.stderr)
 def test_PG18_S_ARRAYS_006_restarted_logger_drops_actual_inherited_backing(self):
  p=self.cluster();self.table(p);snapshot=p.snapshot();pm=p.process.pid
  def logger():
   children=Path(f'/proc/{pm}/task/{pm}/children').read_text().split()
   out=[]
   for pid in children:
    try:cmd=Path(f'/proc/{pid}/cmdline').read_bytes()
    except FileNotFoundError:continue
    if b'logger' in cmd:out.append(int(pid))
   return out
  actual=logger();self.assertEqual(len(actual),1,actual);old=actual[0]
  # Rootless native fixture opens actual proc maps of its descendant PM.
  spans=[]
  for line in Path(f'/proc/{pm}/maps').read_text().splitlines():
   cols=line.split();a,b=(int(x,16)for x in cols[0].split('-'));spans.append((a,b,cols[1],int(cols[4])))
  inodes=set()
  for a in snapshot['arrays']:
   base=int(a['base']);end=base+int(a['reserved_bytes']);match=[x for x in spans if x[0]<=base and end<=x[1] and x[2]=='rw-s'];self.assertEqual(len(match),1);self.assertGreater(match[0][3],0);inodes.add(match[0][3])
  self.assertEqual(len(inodes),4)
  os.kill(old,signal.SIGKILL);until=time.monotonic()+8;replacement=[]
  while time.monotonic()<until:
   replacement=logger()
   if len(replacement)==1 and replacement[0]!=old:break
   time.sleep(.03)
  self.assertEqual(len(replacement),1);self.assertNotEqual(replacement[0],old)
  inherited={int(line.split()[4])for line in Path(f'/proc/{replacement[0]}/maps').read_text().splitlines()}
  self.assertTrue(inodes.isdisjoint(inherited));self.assertEqual(p.process.pid,pm)
  self.assertEqual(p.sql('SELECT count(*),sum(i) FROM array18_data').stdout.strip(),'150000|11250075000')
  if os.environ.get('NEON_PG18_EVIDENCE'):Path(os.environ['NEON_PG18_EVIDENCE'],'actual-restarted-logger.json').write_text(json.dumps({'pmPid':pm,'oldLoggerPid':old,'newLoggerPid':replacement[0],'actualPMArrayInodes':sorted(inodes),'noSharedArrayInNewLogger':True},indent=2)+'\n')
if __name__=='__main__':unittest.main(verbosity=2)
