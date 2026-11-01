#!/usr/bin/env python3
# 只验证Work2实际进程/前台观察及其直接准入依赖，不认证Root/normal。
import importlib.util,json,os,select,signal,socket,struct,subprocess,time,unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('arrays18_work_helper',Path(__file__).with_name('test_arrays18.py'));arrays=importlib.util.module_from_spec(spec);spec.loader.exec_module(arrays)
region=arrays.region
class Wire:
 def __init__(self,p,user='work18_user',startup=True):
  self.socket=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);self.socket.settimeout(10);self.socket.connect(str(p.sock/'.s.PGSQL.55438'));self.pid=None
  if startup:
   body=struct.pack('!I',196608)+b'user\0'+user.encode()+b'\0database\0postgres\0\0';self.socket.sendall(struct.pack('!I',len(body)+4)+body);self.ready()
 def exact(self,n):
  out=b''
  while len(out)<n:
   b=self.socket.recv(n-len(out))
   if not b:raise RuntimeError('actual PG frontend EOF')
   out+=b
  return out
 def ready(self):
  errors=[];rows=[]
  while True:
   typ=self.exact(1);n=struct.unpack('!I',self.exact(4))[0];body=self.exact(n-4)
   if typ==b'K':self.pid=struct.unpack('!I',body[:4])[0] # 不保留/记录取消密钥。
   if typ==b'E':errors.append(body.decode(errors='replace'))
   if typ==b'D':rows.append(body)
   if typ==b'Z':return errors,rows,body
 def query(self,text):
  body=text.encode()+b'\0';self.socket.sendall(b'Q'+struct.pack('!I',len(body)+4)+body);return self.ready()
 def close(self):self.socket.close()
class Work(unittest.TestCase):
 def cluster(self,mode='test-v1:2048'):
  p=arrays.Cluster(mode,'worker');self.addCleanup(p.close);p.start();p.sql('CREATE ROLE work18_user LOGIN');return p
 def snap(self,p):return json.loads(p.sql('SELECT test_dsg_work18_snapshot()').stdout)
 def wait(self,p,predicate,seconds=10):
  until=time.monotonic()+seconds
  while time.monotonic()<until:
   s=self.snap(p)
   if predicate(s):return s
   time.sleep(.02)
  self.fail('actual Work snapshot timeout '+json.dumps(s))
 def wire(self,p,startup=True):
  w=Wire(p,startup=startup);self.addCleanup(w.close);return w
 def fact(self,name,value):
  if os.environ.get('NEON_PG18_EVIDENCE'):Path(os.environ['NEON_PG18_EVIDENCE'],name+'.json').write_text(json.dumps(value,indent=2)+'\n')
 def test_PG18_U_WORK2_001_default_off_and_original_session_acl(self):
  off=self.cluster(None);bad=off.sql('SELECT test_dsg_work18_snapshot()',check=False);self.assertNotEqual(bad.returncode,0);self.assertIn('registered running server',bad.stderr)
  p=self.cluster();s=self.snap(p);self.assertEqual(s['api'],2);self.assertTrue(s['observer_excluded']);self.assertEqual(s['client_processes'],0);self.assertFalse(s['normal_permit']);self.assertFalse(s['memory_context_budget'])
  for fn in ['test_dsg_work18_snapshot()','test_dsg_work18_admission(boolean)']:
   self.assertEqual(p.sql("SELECT has_function_privilege('work18_user','"+fn+"','EXECUTE')").stdout.strip(),'f')
  p.sql('GRANT EXECUTE ON FUNCTION test_dsg_work18_snapshot() TO work18_user');p.sql(f"CREATE FUNCTION work18_definer() RETURNS text AS '{region.LIB}','test_dsg_work18_snapshot' LANGUAGE C SECURITY DEFINER")
  for q in ['SET ROLE work18_user;SELECT test_dsg_work18_snapshot()','SET SESSION AUTHORIZATION work18_user;SELECT work18_definer()']:
   bad=p.sql(q,check=False);self.assertNotEqual(bad.returncode,0);self.assertIn('original administrator',bad.stderr)
  self.fact('work2-abi-and-actual-empty',s)
 def test_PG18_S_WORK2_001_pre_pgproc_half_startup_and_actual_exit(self):
  p=self.cluster();before=self.snap(p);self.assertEqual(before['startup_clients'],0)
  for end in ['close','terminate']:
   with self.subTest(end=end):
    children=Path('/proc',str(p.process.pid),'task',str(p.process.pid),'children');old=set(children.read_text().split());w=self.wire(p,False)
    until=time.monotonic()+5;live=set()
    while time.monotonic()<until:
     live=set(children.read_text().split())-old
     if live:break
     time.sleep(.01)
    self.assertEqual(len(live),1)
    self.wait(p,lambda s:s['startup_clients']>=1)
    if end=='terminate':
     pid=int(next(iter(live)));pf=os.pidfd_open(pid);self.addCleanup(os.close,pf);signal.pidfd_send_signal(pf,signal.SIGTERM)
     poll=select.poll();poll.register(pf,select.POLLIN);self.assertTrue(poll.poll(5000))
    else:w.close()
    self.wait(p,lambda s:s['startup_clients']==0)
  self.fact('work2-prestart-and-exit',self.snap(p))
 def test_PG18_S_WORK2_002_partial_message_and_real_execution_cancel(self):
  p=self.cluster();w=self.wire(p);self.wait(p,lambda s:s['idle_clients']==1)
  w.socket.sendall(b'Q');self.wait(p,lambda s:s['frontend_reads']==1)
  body=b'SELECT 17\0';w.socket.sendall(struct.pack('!I',len(body)+4)+body);self.assertFalse(w.ready()[0]);self.wait(p,lambda s:s['idle_clients']==1 and s['frontend_reads']==0)
  body=b'SELECT pg_sleep(30)\0';w.socket.sendall(b'Q'+struct.pack('!I',len(body)+4)+body);self.wait(p,lambda s:s['frontend_executes']==1)
  self.assertEqual(p.sql('SELECT pg_cancel_backend('+str(w.pid)+')').stdout.strip(),'t');self.assertTrue(w.ready()[0]);self.wait(p,lambda s:s['idle_clients']==1 and s['frontend_executes']==0)
  self.fact('work2-actual-cancel',self.snap(p))
 def test_PG18_S_WORK2_003_retained_transaction_and_bounded_closed_input(self):
  p=self.cluster();w=self.wire(p);self.assertFalse(w.query('BEGIN')[0]);self.wait(p,lambda s:s['retained_transactions']==1)
  p.sql('SELECT test_dsg_work18_admission(false)');s=self.snap(p);self.assertFalse(s['foreground_idle']);self.assertFalse(w.query('ROLLBACK')[0]);self.wait(p,lambda s:s['retained_transactions']==0 and s['foreground_idle'])
  total=4<<20;w.socket.sendall(b'Q'+struct.pack('!I',total+4)+b'SELECT 1 ');self.wait(p,lambda s:s['bounded_inputs']==1 and s['frontend_reads']==0)
  w.socket.sendall(b' '*(total-len(b'SELECT 1 ')-1)+b'\0');errors,_,_=w.ready();self.assertTrue(errors);self.assertIn('admission is closed',''.join(errors))
  p.sql('SELECT test_dsg_work18_admission(true)');self.assertFalse(w.query('SELECT 18')[0]);self.wait(p,lambda s:s['idle_clients']==1 and s['bounded_inputs']==0)
  self.fact('work2-bounded-full-frame-and-reopen',self.snap(p))
 def test_PG18_S_WORK2_004_actual_crash_new_boot_discards_old_slots(self):
  p=self.cluster();p.sql('CHECKPOINT');w=self.wire(p);self.assertFalse(w.query('BEGIN')[0]);old=self.wait(p,lambda s:s['retained_transactions']==1);pm=p.process.pid
  os.kill(w.pid,signal.SIGKILL);w.close();p.ready();self.assertEqual(p.process.pid,pm)
  fresh=self.wait(p,lambda s:s['boot']!=old['boot'] and s['startup_clients']==0 and s['client_processes']==0 and s['retained_transactions']==0)
  self.fact('work2-actual-new-boot',{'old':old,'new':fresh,'same_actual_PM':pm})
if __name__=='__main__':unittest.main(verbosity=2)
