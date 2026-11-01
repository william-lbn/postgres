#!/usr/bin/env python3
# 仅Connection1实际PM门禁；test-v1原生数组不是持续normal或MemoryContext预算。
import importlib.util,json,os,signal,socket,subprocess,time,types,unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('connection18_work',Path(__file__).with_name('test_work18.py'));work=importlib.util.module_from_spec(spec);spec.loader.exec_module(work)
arrays=work.arrays;region=work.region
class Cluster(arrays.Cluster):
 def __init__(self,mode='test-v1:2048',gate=True,admission=True,lease=30000,birth=False):
  super().__init__(mode,'worker');self.birth=birth;self.public=self.sock;self.private=self.data/'dsg_socket';self.private.mkdir(mode=0o700);self.sock=self.private
  with (self.data/'postgresql.conf').open('a')as f:
   f.write(f"unix_socket_directories='{self.public},{self.private}'\nunix_socket_permissions=0700\ndynamic_shared_buffers_require_connection_gate={'on'if gate else'off'}\ndynamic_shared_buffers_require_admission={'on'if admission else'off'}\ndynamic_shared_buffers_startup_budget={'128MB'if admission else'0'}\ndynamic_shared_buffers_admission_lease_timeout={lease}\n")
 def start(self):
  if not self.birth:return super().start()
  self.stream=self.log.open('a');e=region.env();e['NEON_DSGA_EARLY_ARRAYS_PROFILE']='test-v1:2048';e['NEON_DSGA_ADDRESS_SPACE_MAX_BYTES']=str(2<<30)
  self.process=subprocess.Popen([str(region.BIN/'postgres'),'-D',str(self.data)],env=e,stdout=self.stream,stderr=subprocess.STDOUT)
  self.ready();self.sql('CREATE EXTENSION IF NOT EXISTS test_dsg_region18')
 def children(self):return set(Path('/proc',str(self.process.pid),'task',str(self.process.pid),'children').read_text().split())
class Connection(unittest.TestCase):
 def cluster(self,**kw):
  p=Cluster(**kw);self.addCleanup(p.close);p.start();p.sql('CREATE ROLE work18_user LOGIN');return p
 def snap(self,p):return json.loads(p.sql('SELECT test_dsg_connection18_snapshot()').stdout)
 def wait(self,p,pred,seconds=5):
  until=time.monotonic()+seconds
  while time.monotonic()<until:
   s=self.snap(p)
   if pred(s):return s
   time.sleep(.02)
  self.fail('actual Connection snapshot timeout '+json.dumps(s))
 def fact(self,name,value):
  if os.environ.get('NEON_PG18_EVIDENCE'):Path(os.environ['NEON_PG18_EVIDENCE'],name+'.json').write_text(json.dumps(value,indent=2)+'\n')
 def gate(self,p,open):
  p.sql('SELECT test_dsg_work18_admission('+('true'if open else'false')+')')
  return self.wait(p,lambda s:s['acknowledged']and s['requested_open']==open and s['applied_open']==open)
 def blocked(self,p):
  s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);s.settimeout(3);self.addCleanup(s.close);s.connect(str(p.public/'.s.PGSQL.55438'))
  # 不发startup字节；若旧路径fork了，真实child会停在启动包而不会自行完成认证。
  try:self.assertEqual(s.recv(1),b'')
  except ConnectionResetError:pass
  return s
 def wire(self,p,startup=True):
  w=work.Wire(types.SimpleNamespace(sock=p.public),startup=startup);self.addCleanup(w.close);return w
 def test_PG18_U_CONNECTION1_001_default_off_and_original_session(self):
  p=self.cluster(mode=None,gate=False,admission=False);bad=p.sql('SELECT test_dsg_connection18_snapshot()',check=False);self.assertNotEqual(bad.returncode,0);self.assertIn('registered running server',bad.stderr)
  p=self.cluster(gate=False,admission=False);s=self.snap(p);self.assertEqual((s['api'],s['work_api']),(1,2));self.assertFalse(s['enabled']);self.assertEqual((s['requested_serial'],s['applied_serial']),(0,0));self.assertFalse(s['acknowledged'])
  rows=p.sql("SELECT name,context FROM pg_settings WHERE name IN ('dynamic_shared_buffers_require_connection_gate','dynamic_shared_buffers_require_admission','dynamic_shared_buffers_startup_budget','dynamic_shared_buffers_admission_lease_timeout') ORDER BY name").stdout.strip().splitlines();self.assertEqual(len(rows),4);self.assertTrue(all(x.endswith('|postmaster')for x in rows))
  self.assertEqual(p.sql("SELECT has_function_privilege('work18_user','test_dsg_connection18_snapshot()','EXECUTE')").stdout.strip(),'f');p.sql('GRANT EXECUTE ON FUNCTION test_dsg_connection18_snapshot() TO work18_user');p.sql(f"CREATE FUNCTION connection18_definer() RETURNS text AS '{region.LIB}','test_dsg_connection18_snapshot' LANGUAGE C SECURITY DEFINER")
  for sql in ['SET ROLE work18_user;SELECT test_dsg_connection18_snapshot()','SET SESSION AUTHORIZATION work18_user;SELECT connection18_definer()']:
   bad=p.sql(sql,check=False);self.assertNotEqual(bad.returncode,0);self.assertIn('original administrator',bad.stderr)
  self.fact('connection1-default-off-and-ABI',s)
 def test_PG18_U_CONNECTION1_002_required_configuration_fails_closed(self):
  cases=['no_admission','no_budget','no_native','private_mode','private_symlink','socket_mode','missing_endpoint']
  for case in cases:
   with self.subTest(case=case):
    p=Cluster(mode=None if case=='no_native'else'test-v1:2048',admission=case!='no_admission');self.addCleanup(p.close)
    extra=''
    if case=='no_budget':extra='dynamic_shared_buffers_startup_budget=0\n'
    if case=='private_mode':p.private.chmod(0o755)
    if case=='private_symlink':p.private.rmdir();p.private.symlink_to(p.public,target_is_directory=True)
    if case=='socket_mode':extra='unix_socket_permissions=0777\n'
    if case=='missing_endpoint':extra=f"unix_socket_directories='{p.public}'\n"
    with (p.data/'postgresql.conf').open('a')as f:f.write(extra)
    e=region.env()
    if p.mode is not None:e['NEON_DSGA_PG18_NATIVE_ARRAYS']=p.mode
    x=subprocess.run([str(region.BIN/'postgres'),'-D',str(p.data)],env=e,capture_output=True,text=True,timeout=10);self.assertNotEqual(x.returncode,0);self.assertIn('FATAL',x.stderr);self.assertIn('dynamic'if case in ['no_native','no_budget']else'DSGA',x.stderr)
 def test_PG18_S_CONNECTION1_001_actual_closed_PM_ack_private_and_public_no_fork(self):
  p=self.cluster();s=self.wait(p,lambda s:s['acknowledged']and s['public_connections_blocked']);self.assertEqual((s['requested_serial'],s['applied_serial']),(1,1));self.assertFalse(s['admission_open']);self.assertTrue(s['private_management_ready']);self.assertTrue(s['observer_excluded']);before=p.children()
  for _ in range(8):self.blocked(p)
  self.assertEqual(p.children(),before);after=self.snap(p);self.assertEqual(after['startup_clients'],0);self.assertEqual(after['client_processes'],0);self.assertTrue(after['foreground_idle']);self.assertFalse(after['normal_permit']);self.assertFalse(after['memory_context_budget']);self.fact('connection1-actual-closed-public-no-fork',{'before':s,'after':after,'children_before':sorted(before),'children_after':sorted(p.children()),'actual_empty_startup_connections':8})
 def test_PG18_S_CONNECTION1_002_reopen_and_inflight_prestart_survives_closed_ack(self):
  p=self.cluster();opened=self.gate(p,True);w=self.wire(p);self.assertFalse(w.query('SELECT 18')[0]);oldchildren=p.children();half=self.wire(p,False);inflight=self.wait(p,lambda s:s['startup_clients']==1);newchildren=p.children()-oldchildren
  # snapshot的psql已退出，PM可能尚未waitpid；只等该真实observer回收，不发新SQL。
  until=time.monotonic()+2
  while len(newchildren)!=1 and time.monotonic()<until:
   time.sleep(.005);newchildren=p.children()-oldchildren
  self.assertEqual(len(newchildren),1)
  closed=self.gate(p,False);self.assertEqual(closed['startup_clients'],1);self.assertFalse(closed['foreground_idle']);self.assertTrue(newchildren<=p.children());self.blocked(p);half.close();self.wait(p,lambda s:s['startup_clients']==0);reopened=self.gate(p,True);self.assertGreater(reopened['requested_serial'],closed['requested_serial']);new=self.wire(p);self.assertFalse(new.query('SELECT 18')[0]);self.fact('connection1-close-ack-preserves-real-prestart',{'opened':opened,'inflight':inflight,'closed':closed,'reopened':reopened,'actual_half_child':sorted(newchildren)})
 def test_PG18_S_CONNECTION1_003_actual_lease_expiry_blocks_later_accept(self):
  p=self.cluster(lease=1000);opened=self.gate(p,True);w=self.wire(p);self.assertFalse(w.query('SELECT 1')[0]);w.close();time.sleep(1.12);self.blocked(p);closed=self.wait(p,lambda s:s['acknowledged']and s['public_connections_blocked']and not s['admission_open']);bad=p.sql('SELECT test_dsg_work18_admission(true)',check=False);self.assertNotEqual(bad.returncode,0);self.assertIn('DSGA admission lease expired; controlled recovery is required',bad.stderr);self.fact('connection1-actual-expired-public-accept',{'opened':opened,'closed':closed})
 def test_PG18_S_CONNECTION1_004_actual_same_PM_crash_newboot_closed(self):
  p=self.cluster();old=self.gate(p,True);p.sql('CHECKPOINT');w=self.wire(p);self.assertFalse(w.query('BEGIN')[0]);pm=p.process.pid;os.kill(w.pid,signal.SIGKILL);w.close();p.ready();self.assertEqual(p.process.pid,pm);new=self.wait(p,lambda s:s['boot']!=old['boot']and s['acknowledged']and s['public_connections_blocked']);self.assertFalse(new['admission_open']);self.assertEqual((new['requested_serial'],new['applied_serial']),(1,1));self.assertEqual(int(new['admission_revision']),0);self.assertEqual(new['startup_clients'],0);self.blocked(p);self.fact('connection1-actual-same-PM-newboot-closed',{'old':old,'new':new,'same_actual_PM':pm})
 def test_PG18_S_CONNECTION1_005_actual_early_birth_arrays_gate(self):
  p=self.cluster(mode=None,birth=True);birth=json.loads(p.sql('SELECT test_dsg_birth_arrays18()').stdout);self.assertTrue(birth['enabled']);self.assertTrue(birth['imported']);self.assertEqual(birth['maximum_buffers'],2048);self.assertEqual(int(birth['address_space_max_bytes']),2<<30);self.assertFalse(birth['source_bound']);self.assertFalse(birth['root_enrolled']);self.assertEqual(len(birth['ranges']),4)
  status=Path('/proc',str(p.process.pid),'status').read_text();nnp=next(x.split()[1]for x in status.splitlines()if x.startswith('NoNewPrivs:'));self.assertEqual(nnp,'1');limit=next(x for x in Path('/proc',str(p.process.pid),'limits').read_text().splitlines()if x.startswith('Max address space'));self.assertEqual(limit.split()[-3:-1],[str(2<<30),str(2<<30)])
  initial=self.wait(p,lambda s:s['acknowledged']and s['public_connections_blocked']);self.blocked(p);opened=self.gate(p,True);w=self.wire(p);self.assertFalse(w.query('SELECT 18')[0]);closed=self.gate(p,False);self.blocked(p);self.assertFalse(closed['normal_permit']);self.fact('connection1-actual-early-birth-gate',{'birth':birth,'actual_NNP':nnp,'actual_proc_limits':limit,'initial':initial,'opened':opened,'closed':closed,'Source':False,'normal':False})
if __name__=='__main__':unittest.main(verbosity=2)
