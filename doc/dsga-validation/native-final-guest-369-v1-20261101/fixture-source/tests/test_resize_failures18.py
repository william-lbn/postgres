#!/usr/bin/env python3
# PG18 native failure/lease tests only; no Root/normal or io_uring qualification.
import importlib.util,json,os,select,signal,subprocess,time,unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('resize18_support',Path(__file__).with_name('test_resize18.py'));resize=importlib.util.module_from_spec(spec);spec.loader.exec_module(resize)
class Cluster(resize.Cluster):
 def __init__(self,method='worker'):
  super().__init__(aio=True)
  with(self.data/'postgresql.conf').open('a')as f:f.write("io_method='"+method+"'\n")
class Failures(unittest.TestCase):
 def cluster(self,method='worker'):
  p=Cluster(method);self.addCleanup(p.close);p.start();return p
 def save(self,name,obj):
  if os.environ.get('NEON_PG18_EVIDENCE'):Path(os.environ['NEON_PG18_EVIDENCE'],name+'.json').write_text(json.dumps(obj,indent=2)+'\n')
 def committed(self,r,h):resize.Resize.committed(self,r,h)
 def table(self,p):resize.Resize.table(self,p)
 def data(self,p):resize.Resize.data(self,p)
 def same_capacity(self,a,b):
  self.assertEqual((a['low'],a['high'],a['generation']),(b['low'],b['high'],b['generation']))
  self.assertEqual(a['current_main_arrays_upper'],b['current_main_arrays_upper'])
  self.assertEqual([x['committed_upper']for x in a['arrays']],[x['committed_upper']for x in b['arrays']])
 def request_sql(self,kind,target,rev,generation,timeout=15000):return f"SELECT test_dsg_resize18_request('{kind}',{target},{rev},{generation},{timeout})"
 def paused_shrink(self,p,label):
  point='dsg-buffer-shrink-after-fence';p.sql(f"SELECT injection_points_attach('{point}','wait')")
  state=p.state();actor=p.asynchronous("SET application_name='"+label+"';"+self.request_sql('shrink',512,2,int(state['generation'])))
  self.addCleanup(lambda:actor.kill()if actor.poll()is None else None)
  until=time.monotonic()+5
  while time.monotonic()<until:
   output=p.sql("SELECT pid,wait_event FROM pg_stat_activity WHERE application_name='"+label+"'").stdout.strip()
   if output and point in output:return point,actor,int(output.split('|')[0]),state
   time.sleep(.02)
  self.fail('real shrink fence pause not observed '+output)
 def test_PG18_S_FAILURE_001_cancel_shrink_restores_L_and_retry(self):
  p=self.cluster();self.committed(p.resize('grow',2048,1),2048);self.table(p);before=p.state();point='dsg-buffer-shrink-after-fence'
  p.sql(f"SELECT injection_points_attach('{point}','wait')")
  # ON_ERROR_STOP0保同一个真实backend：取消后不得靠连接退出释放锁掩盖清理问题。
  env=resize.arrays.region.env();actor=subprocess.Popen([str(resize.arrays.region.BIN/'psql'),'-h',str(p.sock),'-p','55438','-U',resize.arrays.region.USER,'-d','postgres','-XqAt','-v','ON_ERROR_STOP=0'],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
  self.addCleanup(lambda:actor.kill()if actor.poll()is None else None)
  sql="SET application_name='pg18-cancel';SELECT pg_backend_pid();"+self.request_sql('shrink',512,2,int(before['generation']))+";SELECT pg_backend_pid();SELECT test_dsg_resize18_state();SELECT injection_points_detach('"+point+"');"+self.request_sql('shrink',512,3,int(before['generation']))+";SELECT count(*),sum(i) FROM resize18_data;"
  actor.stdin.write(sql);actor.stdin.close();actor.stdin=None
  until=time.monotonic()+5
  while time.monotonic()<until:
   output=p.sql("SELECT pid,wait_event FROM pg_stat_activity WHERE application_name='pg18-cancel'").stdout.strip()
   if output and point in output:pid=int(output.split('|')[0]);break
   time.sleep(.02)
  else:self.fail('actual same-session cancel fence not reached '+output)
  self.assertEqual(p.sql(f'SELECT pg_cancel_backend({pid})').stdout.strip(),'t')
  out,err=actor.communicate(timeout=10);self.assertEqual(actor.returncode,0,err);self.assertEqual(err.count('ERROR:'),1,err);self.assertIn('canceling statement',err)
  rows=[line for line in out.splitlines()if line.strip()];self.assertEqual(len(rows),5,out);self.assertEqual(rows[0],rows[1]);self.assertEqual(int(rows[0]),pid)
  after=json.loads(rows[2]);retry=json.loads(rows[3]);self.same_capacity(before,after);self.assertEqual(after['phase'],3);self.committed(retry,512);self.assertEqual(rows[4],'150000|11250075000');self.data(p)
  self.save('cancel-shrink-retry',{'before':before,'after':after,'retry':retry,'sameBackendPID':pid,'stderr':err})
 def test_PG18_S_FAILURE_002_ERROR_after_populate_same_backend_retains_cost(self):
  p=self.cluster();self.table(p);before=p.state();point='dsg-buffer-grow-before-publish';p.sql(f"SELECT injection_points_attach('{point}','error')")
  sql="SELECT pg_backend_pid();"+self.request_sql('grow',2048,1,int(before['generation']))+";SELECT pg_backend_pid();SELECT test_dsg_resize18_state();SELECT injection_points_detach('"+point+"');"+self.request_sql('grow',2048,2,int(before['generation']))+';'
  result=p.batch(sql);self.assertEqual(result.returncode,0);self.assertEqual(result.stderr.count('ERROR:'),1,result.stderr);self.assertIn(point,result.stderr)
  rows=result.stdout.strip().splitlines();self.assertEqual(len(rows),5,result.stdout);self.assertEqual(rows[0],rows[1]);after=json.loads(rows[2]);retry=json.loads(rows[4]);self.assertEqual((after['low'],after['high'],after['generation']),(512,512,before['generation']));self.assertEqual(after['phase'],3)
  self.assertGreater(int(after['current_main_arrays_upper']),int(before['current_main_arrays_upper']))
  self.assertTrue(all(int(x['committed_upper'])>=int(y['committed_upper'])for x,y in zip(after['arrays'],before['arrays'])))
  self.committed(retry,2048);self.data(p);self.committed(p.resize('shrink',512,3),512);self.data(p)
  self.save('ERROR-grow-same-backend-cost',{'before':before,'after':after,'retry':retry,'backendPID':rows[0],'stderr':result.stderr})
 def test_PG18_S_FAILURE_003_FATAL_backend_releases_locks_without_PM_reinit(self):
  p=self.cluster();self.committed(p.resize('grow',2048,1),2048);self.table(p);pm=p.process.pid
  point,actor,pid,before=self.paused_shrink(p,'pg18-fatal')
  self.assertEqual(p.sql(f'SELECT pg_terminate_backend({pid})').stdout.strip(),'t');out,err=actor.communicate(timeout=10);self.assertNotEqual(actor.returncode,0)
  p.sql(f"SELECT injection_points_detach('{point}')");after=p.state();self.same_capacity(before,after);self.assertEqual(after['boot'],before['boot']);self.assertEqual(p.process.pid,pm)
  retry=p.resize('shrink',512,3);self.committed(retry,512);self.data(p);self.save('FATAL-shrink-retry',{'before':before,'after':after,'retry':retry,'stderr':err})
 def test_PG18_S_FAILURE_004_SIGKILL_preparing_backend_PM_reinitializes(self):
  p=self.cluster();self.committed(p.resize('grow',2048,1),2048);self.table(p);pm=p.process.pid;mapping=p.snapshot()
  point,actor,pid,before=self.paused_shrink(p,'pg18-crash')
  os.kill(pid,signal.SIGKILL);actor.communicate(timeout=10)
  until=time.monotonic()+15;after=None
  while time.monotonic()<until:
   try:
    after=p.state()
    if after['boot']!=before['boot']:break
   except(RuntimeError,subprocess.TimeoutExpired):pass
   time.sleep(.03)
  self.assertIsNotNone(after);self.assertNotEqual(after['boot'],before['boot']);self.assertEqual((after['low'],after['high']),(512,512));self.assertEqual(after['generation'],'1');self.assertEqual(p.process.pid,pm)
  self.assertEqual([a['base']for a in p.snapshot()['arrays']],[a['base']for a in mapping['arrays']]);self.data(p);p.sql('SELECT test_dsg_resize18_prepare()')
  retry=p.resize('grow',2048,1);self.committed(retry,2048);self.committed(p.resize('shrink',512,2),512);self.data(p)
  self.save('SIGKILL-preparing-PM-recovery',{'before':before,'newBoot':after,'retry':retry,'samePM':pm})
 def test_PG18_S_FAILURE_005_SIGKILL_PM_cold_recovery_then_new_resize(self):
  p=self.cluster();self.committed(p.resize('grow',2048,1),2048);self.table(p);before=p.state();pm=p.process.pid
  # 先停原PM整个线程组，再取子任务pidfd。PM此时不能fork/reap，避免children
  # 枚举到pidfd_open之间发生PM回收/数值PID复用；不靠stat/starttime窗口发信号。
  pmfd=os.pidfd_open(pm);children={};terminal={};killed=[];old_children=[]
  try:
   signal.pidfd_send_signal(pmfd,signal.SIGSTOP)
   until=time.monotonic()+5;stopped=None
   while time.monotonic()<until:
    stopped=os.waitid(os.P_PIDFD,pmfd,os.WSTOPPED|os.WEXITED|os.WNOHANG|os.WNOWAIT)
    if stopped is not None:break
    time.sleep(.01)
   self.assertIsNotNone(stopped,'original PM stop not observed')
   self.assertEqual((stopped.si_pid,stopped.si_code,stopped.si_status),(pm,os.CLD_STOPPED,signal.SIGSTOP))
   old_children=[int(x)for x in Path(f'/proc/{pm}/task/{pm}/children').read_text().split()]
   self.assertTrue(old_children,'cold restart must cover original PM children')
   for pid in old_children:children[os.pidfd_open(pid)]=pid
   # 后续包括失败清理只用原pidfd；旧PID已被init回收/复用也不会signal新任务。
   signal.pidfd_send_signal(pmfd,signal.SIGKILL);p.process.wait(timeout=5)
   pm_poll=select.poll();pm_poll.register(pmfd,select.POLLIN)
   self.assertTrue(any(events&select.POLLIN for fd,events in pm_poll.poll(0)),'original PM pidfd not terminal')
   poller=select.poll();pending=set(children)
   for fd in pending:poller.register(fd,select.POLLIN)
   def drain(timeout):
    until=time.monotonic()+timeout
    while pending and time.monotonic()<until:
     for fd,events in poller.poll(max(1,int((until-time.monotonic())*1000))):
      self.assertFalse(events&select.POLLNVAL,'original pidfd became invalid')
      if events&select.POLLIN:
       terminal[children[fd]]=events;pending.remove(fd);poller.unregister(fd)
   drain(3)
   for fd in pending:
    try:signal.pidfd_send_signal(fd,signal.SIGKILL);killed.append(children[fd])
    except ProcessLookupError:pass
   drain(5)
   live=[children[fd]for fd in pending]
   self.assertEqual(live,[],'original PG pidfds not terminal before cold restart')
  finally:
   # 断言/超时也不泄漏停止的原PM或子任务；只操作仍持有的原pidfd。
   for fd in [pmfd,*children]:
    try:signal.pidfd_send_signal(fd,signal.SIGKILL)
    except ProcessLookupError:pass
    finally:os.close(fd)
  p.stream.close();p.process=None;resize.arrays.Cluster.start(p);p.sql('SELECT test_dsg_resize18_prepare()');after=p.state();self.assertNotEqual(p.process.pid,pm);self.assertNotEqual(after['boot'],before['boot']);self.assertEqual((after['low'],after['high']),(512,512));self.data(p)
  retry=p.resize('grow',2048,1);self.committed(retry,2048);self.committed(p.resize('shrink',512,2),512);self.data(p)
  self.save('SIGKILL-PM-cold-recovery',{'oldPM':pm,'newPM':p.process.pid,'before':before,'newBoot':after,'retry':retry,'oldChildren':old_children,'originalPMStoppedBeforePidfdCapture':True,'oldPidfdTerminalEvents':terminal,'oldPidfdCleanupSIGKILL':killed,'oldLivePidfdsBeforeRestart':live})
 def test_PG18_S_FAILURE_006_ERROR_after_H_publish_keeps_commit_and_pending_cost(self):
  p=self.cluster();grown=p.resize('grow',2048,1);self.committed(grown,2048);self.table(p);before=p.state()
  point='dsg-buffer-shrink-after-publish';p.sql(f"SELECT injection_points_attach('{point}','error')")
  sql="SELECT pg_backend_pid();"+self.request_sql('shrink',512,2,int(before['generation']))+";SELECT pg_backend_pid();SELECT test_dsg_resize18_state();SELECT injection_points_detach('"+point+"');"
  result=p.batch(sql);self.assertEqual(result.returncode,0);self.assertEqual(result.stderr.count('ERROR:'),1,result.stderr);self.assertIn(point,result.stderr)
  rows=result.stdout.strip().splitlines();self.assertEqual(len(rows),3,result.stdout);self.assertEqual(rows[0],rows[1]);after=json.loads(rows[2]);self.assertEqual((after['low'],after['high']),(512,512));self.assertEqual(int(after['generation']),int(before['generation'])+1);self.assertEqual(after['phase'],2)
  self.assertEqual(after['current_main_arrays_upper'],before['current_main_arrays_upper']);self.assertTrue(all(a['release_state']==1 and not a['release_verified'] for a in after['arrays']))
  replay=p.resize('shrink',512,2,int(before['generation']));self.assertEqual(replay['disposition'],1);self.assertEqual(replay['result_phase'],2);self.assertEqual(replay['result_high'],512)
  retry=p.resize('shrink',512,3);self.committed(retry,512);self.assertLess(int(retry['current_main_arrays_upper']),int(after['current_main_arrays_upper']));self.data(p)
  self.save('ERROR-after-H-publish-pending-cost',{'before':before,'afterCommitted':after,'historicalReplay':replay,'newRevisionRelease':retry,'backendPID':rows[0],'stderr':result.stderr})
class Uring(Failures):
 def test_PG18_S_URING_001_real_pin_and_post_unpin_drain(self):
  p=self.cluster('io_uring');self.assertEqual(p.sql('SHOW io_method').stdout.strip(),'io_uring');self.committed(p.resize('grow',2048,1),2048);self.table(p);evidence=[]
  for stage,revision in [('before',2),('after',5)]:
   if stage=='after':self.committed(p.resize('grow',2048,4),2048)
   tail=json.loads(p.sql("SELECT test_dsg_resize18_find_tail('resize18_data')").stdout);read=f"SELECT read_rel_block_ll('resize18_data',{tail['block']},1,true)";p.sql(read);p.state();p.snapshot()
   hook='test_dsg_arrays18_pause_before_unpin'if stage=='before'else'test_dsg_arrays18_pause';p.sql('SELECT '+hook+'(true)');actor=p.asynchronous(read);self.addCleanup(lambda:actor.kill()if actor.poll()is None else None)
   try:
    until=time.monotonic()+4
    while time.monotonic()<until:
     actual=p.state();completion=p.snapshot();tail_state=actual['tail_now']
     valid=(tail_state['io']>0 and tail_state['wref']>0 and tail_state['pinned']>0)if stage=='before'else all(tail_state[k]==0 for k in ['io','wref','pinned'])
     if valid and completion['paused_completions']>0:break
     time.sleep(.02)
    else:self.fail('actual io_uring lease phase not observed '+json.dumps({'stage':stage,'actual':actual,'completion':completion}))
    denied=p.resize('shrink',512,revision,timeout=80);self.assertEqual(denied['result_phase'],3);self.assertEqual(denied['high'],2048);self.assertEqual(denied['generation'],actual['generation']);self.assertEqual(denied['current_main_arrays_upper'],actual['current_main_arrays_upper'])
    if stage=='after':self.assertEqual(denied['drain_counts'][0],1536)
    else:self.assertGreater(denied['drain_counts'][2],0)
   finally:p.sql('SELECT '+hook+'(false)')
   out,err=actor.communicate(timeout=10);self.assertEqual(actor.returncode,0,err);retry=p.resize('shrink',512,revision+1);self.committed(retry,512);self.data(p);evidence.append({'stage':stage,'actual':actual,'completion':completion,'denied':denied,'retry':retry})
  self.save('actual-io-uring-both-drain-phases',evidence)
if __name__=='__main__':unittest.main(defaultTest='Failures',verbosity=2)
