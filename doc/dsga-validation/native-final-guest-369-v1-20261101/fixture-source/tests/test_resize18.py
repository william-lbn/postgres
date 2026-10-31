#!/usr/bin/env python3
# Real PG18 buffer kernel only; no Root/normal/uring/K8s qualification.
import importlib.util,json,os,subprocess,time,unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('array18_base',Path(__file__).with_name('test_arrays18.py'))
arrays=importlib.util.module_from_spec(spec);spec.loader.exec_module(arrays)
class Cluster(arrays.Cluster):
 def __init__(self,aio=False):
  super().__init__();self.aio=aio
  if aio:
   with (self.data/'postgresql.conf').open('a')as f:f.write("shared_preload_libraries='test_aio'\n")
 def start(self):
  super().start();self.sql('SELECT test_dsg_resize18_prepare()')
  if self.aio:self.sql('CREATE EXTENSION test_aio;CREATE EXTENSION injection_points')
 def close(self):
  evidence=os.environ.get('NEON_PG18_EVIDENCE')
  if evidence and self.data.exists():
   logs=self.data/'logs'
   if logs.exists():
    for source in logs.rglob('*'):
     if source.is_file():Path(evidence,self.root.name+'-collector-'+source.name).write_bytes(source.read_bytes())
  super().close()
 def state(self):return json.loads(self.sql('SELECT test_dsg_resize18_state()').stdout)
 def resize(self,kind,target,revision,generation=None,timeout=2500):
  if generation is None:generation=int(self.state()['generation'])
  return json.loads(self.sql(f"SELECT test_dsg_resize18_request('{kind}',{target},{revision},{generation},{timeout})").stdout)
class Resize(unittest.TestCase):
 def cluster(self,aio=False):
  p=Cluster(aio);self.addCleanup(p.close);p.start();return p
 def save(self,name,obj):
  if os.environ.get('NEON_PG18_EVIDENCE'):Path(os.environ['NEON_PG18_EVIDENCE'],name+'.json').write_text(json.dumps(obj,indent=2)+'\n')
 def table(self,p):p.sql('CREATE TABLE resize18_data AS SELECT i,repeat(md5(i::text),4) v FROM generate_series(1,150000) i;CHECKPOINT')
 def data(self,p):self.assertEqual(p.sql('SELECT count(*),sum(i),min(length(v)),max(length(v)) FROM resize18_data').stdout.strip(),'150000|11250075000|128|128')
 def committed(self,r,h):
  self.assertIn(r['disposition'],(0,1));self.assertTrue(r['receipt_has_result']);self.assertEqual(r['result_phase'],2);self.assertEqual(r['result_high'],h);self.assertEqual((r['low'],r['high']),(h,h));self.assertFalse(r['normal_permit'])
 def test_PG18_S_RESIZE_001_real_grow_dirty_shrink_regrow(self):
  p=self.cluster();start=p.state();self.assertEqual(start['high'],512);g=p.resize('grow',2048,1);self.committed(g,2048);self.table(p);self.data(p)
  shr=p.resize('shrink',512,2);self.committed(shr,512);self.assertLess(int(shr['current_main_arrays_upper']),int(g['current_main_arrays_upper']));self.data(p)
  reg=p.resize('grow',2048,3);self.committed(reg,2048);self.data(p);p.sql('CHECKPOINT');again=p.resize('shrink',512,4);self.committed(again,512);self.data(p)
  self.save('actual-grow-shrink-regrow',{'start':start,'grow':g,'shrink':shr,'regrow':reg,'shrinkAgain':again})
 def test_PG18_U_RESIZE_001_replay_is_historical_not_current(self):
  p=self.cluster();first=p.state();g=p.resize('grow',1024,1,int(first['generation']));self.committed(g,1024);s=p.resize('shrink',512,2);self.committed(s,512)
  replay=p.resize('grow',1024,1,int(first['generation']));self.assertEqual(replay['disposition'],1);self.assertTrue(replay['receipt_has_result']);self.assertEqual(replay['result_high'],1024);self.assertEqual(replay['high'],512);self.assertEqual(replay['generation'],s['generation'])
  conflict=p.resize('grow',1536,1,int(first['generation']));self.assertEqual(conflict['disposition'],4);self.assertEqual(conflict['high'],512)
  stale=p.resize('grow',1024,3,int(first['generation']));self.assertEqual(stale['disposition'],3);self.assertEqual(stale['high'],512)
  reject=p.resize('shrink',1,4);self.assertEqual(reject['disposition'],9);self.assertFalse(reject['receipt_has_result']);self.assertEqual(reject['high'],512)
  self.save('actual-replay-separation',{'grow':g,'shrink':s,'replayed':replay,'conflict':conflict,'stale':stale,'rejected':reject})
 def test_PG18_S_RESIZE_002_actual_tail_pin_denies_then_retry(self):
  p=self.cluster();self.committed(p.resize('grow',2048,1),2048);self.table(p)
  holder=p.asynchronous("SELECT test_dsg_resize18_hold_tail('resize18_data',2000)");self.addCleanup(lambda:holder.kill()if holder.poll()is None else None)
  until=time.monotonic()+4
  while time.monotonic()<until:
   observed=p.state()
   if observed['tail_now']['pinned']>0:break
   time.sleep(.02)
  else:self.fail('real tail pin not observed '+json.dumps(observed))
  denied=p.resize('shrink',512,2,timeout=80);self.assertEqual(denied['high'],2048);self.assertTrue(denied['receipt_has_result']);self.assertEqual(denied['result_phase'],3);self.assertGreater(denied['drain_counts'][1],0)
  out,err=holder.communicate(timeout=8);self.assertEqual(holder.returncode,0,err);self.assertGreater(int(out.strip()),512)
  retry=p.resize('shrink',512,3);self.committed(retry,512);self.data(p);self.save('actual-tail-pin-retry',{'observed':observed,'denied':denied,'retry':retry,'heldBufferID':int(out)})
 def test_PG18_S_RESIZE_003_live_worker_read_stream_and_checkpoint(self):
  p=self.cluster();self.committed(p.resize('grow',2048,1),2048);self.table(p)
  reader=p.asynchronous('SELECT count(*),sum(i) FROM resize18_data,generate_series(1,3) AS n')
  for rev,target in [(2,1024),(3,2048),(4,512)]:
   r=p.resize('shrink'if target< p.state()['high']else'grow',target,rev,timeout=5000);self.committed(r,target)
  out,err=reader.communicate(timeout=15);self.assertEqual(reader.returncode,0,err);self.assertEqual(out.strip(),'450000|33750225000');p.sql('CHECKPOINT');self.data(p);self.assertEqual(int(p.sql("SELECT count(*) FROM pg_stat_activity WHERE backend_type='io worker'").stdout),2)
 def test_PG18_S_RESIZE_004_new_API_original_session_ACL(self):
  p=self.cluster();p.sql('CREATE ROLE resize18_ordinary LOGIN')
  functions=('test_dsg_resize18_state()','test_dsg_resize18_prepare()','test_dsg_resize18_request(text,integer,bigint,bigint,integer)','test_dsg_resize18_hold_tail(regclass,integer)','test_dsg_resize18_find_tail(regclass)','test_dsg_arrays18_pause_before_unpin(boolean)')
  for fn in functions:self.assertEqual(p.sql("SELECT has_function_privilege('resize18_ordinary','"+fn+"','EXECUTE')").stdout.strip(),'f')
  p.sql('GRANT EXECUTE ON FUNCTION test_dsg_resize18_state() TO resize18_ordinary')
  for sql in ('SET ROLE resize18_ordinary;SELECT test_dsg_resize18_state()',):
   r=p.sql(sql,check=False);self.assertNotEqual(r.returncode,0);self.assertIn('original administrator',r.stderr)
 def test_PG18_S_RESIZE_005_real_worker_IO_pin_denies_then_retry(self):
  p=self.cluster(aio=True);self.committed(p.resize('grow',2048,1),2048);self.table(p)
  tail=json.loads(p.sql("SELECT test_dsg_resize18_find_tail('resize18_data')").stdout)
  # Low-level official18 helper registers real buffer callback + I/O subsystem pin,
  # then releases its own pins before waiting; no fake BM_IO or wref is written here.
  p.sql(f"SELECT read_rel_block_ll('resize18_data',{tail['block']},1,true)");p.state();p.snapshot()
  p.sql('SELECT test_dsg_arrays18_pause_before_unpin(true)')
  reader=p.asynchronous(f"SELECT read_rel_block_ll('resize18_data',{tail['block']},1,true)")
  self.addCleanup(lambda:reader.kill()if reader.poll()is None else None)
  try:
   until=time.monotonic()+4
   while time.monotonic()<until:
    actual=p.state()
    if actual['tail_now']['io']>0 and actual['tail_now']['wref']>0:break
    time.sleep(.02)
   else:self.fail('actual tail worker IO/wref not observed '+json.dumps(actual))
   denied=p.resize('shrink',512,2,timeout=80);self.assertEqual(denied['high'],2048);self.assertEqual(denied['result_phase'],3);self.assertGreater(denied['drain_counts'][2],0)
  finally:
   p.sql('SELECT test_dsg_arrays18_pause_before_unpin(false)')
  out,err=reader.communicate(timeout=10);self.assertEqual(reader.returncode,0,err)
  retry=p.resize('shrink',512,3);self.committed(retry,512);self.data(p)
  self.save('actual-worker-IO-pin-retry',{'tailFixture':tail,'actualIO':actual,'denied':denied,'retry':retry})
 def test_PG18_S_RESIZE_006_after_unpin_completion_lease_denies_then_retry(self):
  p=self.cluster(aio=True);self.committed(p.resize('grow',2048,1),2048);self.table(p)
  tail=json.loads(p.sql("SELECT test_dsg_resize18_find_tail('resize18_data')").stdout)
  # Prime all exact metadata paths before pausing callbacks; supported worker stays2.
  p.sql(f"SELECT read_rel_block_ll('resize18_data',{tail['block']},1,true)");p.state();p.sql('SELECT test_dsg_arrays18_snapshot()')
  p.sql('SELECT test_dsg_arrays18_pause(true)')
  reader=p.asynchronous(f"SELECT read_rel_block_ll('resize18_data',{tail['block']},1,true)")
  self.addCleanup(lambda:reader.kill()if reader.poll()is None else None)
  try:
   until=time.monotonic()+4
   while time.monotonic()<until:
    completion=p.snapshot();actual=p.state()
    if completion['paused_completions']>0 and actual['tail_now']['io']==0 and actual['tail_now']['wref']==0 and actual['tail_now']['pinned']==0:break
    time.sleep(.02)
   else:self.fail('actual post-AIO-pin tail completion window not observed '+json.dumps({'completion':completion,'tail':actual}))
   denied=p.resize('shrink',512,2,timeout=80);self.assertEqual(denied['high'],2048);self.assertEqual(denied['result_phase'],3);self.assertEqual(denied['generation'],actual['generation']);self.assertEqual(denied['drain_counts'][0],2048-512)
  finally:p.sql('SELECT test_dsg_arrays18_pause(false)')
  out,err=reader.communicate(timeout=10);self.assertEqual(reader.returncode,0,err)
  retry=p.resize('shrink',512,3);self.committed(retry,512);self.data(p)
  self.save('actual-post-unpin-completion-shrink-retry',{'tailFixture':tail,'actualNoIOOrPin':actual,'actualPausedCompletion':completion,'denied':denied,'retry':retry})
 def test_PG18_S_RESIZE_007_checkpoint_lease_retains_backing_across_regrow(self):
  p=self.cluster(aio=True);grown=p.resize('grow',2048,1);self.committed(grown,2048);self.table(p)
  p.sql("UPDATE resize18_data SET v=reverse(v)")
  point='dsg-checkpoint-after-scan';p.sql(f"SELECT injection_points_attach('{point}','wait')")
  checkpoint=p.asynchronous('CHECKPOINT');self.addCleanup(lambda:checkpoint.kill()if checkpoint.poll()is None else None)
  try:
   until=time.monotonic()+5
   while time.monotonic()<until:
    waiting=p.sql("SELECT wait_event_type,wait_event FROM pg_stat_activity WHERE backend_type='checkpointer'").stdout.strip()
    if point in waiting:break
    time.sleep(.02)
   else:self.fail('actual checkpoint scan lease not observed '+waiting)
   shrink=p.resize('shrink',512,2);self.committed(shrink,512)
   self.assertEqual(shrink['arrays'][3]['release_state'],2)
   self.assertFalse(shrink['arrays'][3]['release_verified'])
   self.assertEqual(shrink['arrays'][3]['committed_upper'],grown['arrays'][3]['committed_upper'])
   # 原排序区仍由checkpointer读持有，regrow不得按较小H清零其旧前缀。
   regrow=p.resize('grow',1536,3);self.committed(regrow,1536)
   self.assertEqual(regrow['arrays'][3]['committed_upper'],grown['arrays'][3]['committed_upper'])
  finally:p.sql(f"SELECT injection_points_wakeup('{point}');SELECT injection_points_detach('{point}')")
  out,err=checkpoint.communicate(timeout=15);self.assertEqual(checkpoint.returncode,0,err);self.data(p)
  released=p.resize('shrink',512,4);self.committed(released,512)
  self.assertTrue(released['arrays'][3]['release_verified']);self.assertLess(int(released['arrays'][3]['committed_upper']),int(grown['arrays'][3]['committed_upper']))
  self.save('actual-checkpoint-held-shrink-regrow',{'actualWait':waiting,'grown':grown,'shrink':shrink,'regrow':regrow,'releasedAfterCheckpoint':released})
 def test_PG18_S_RESIZE_008_original18_eviction_entrypoints_keep_scope(self):
  p=self.cluster();self.committed(p.resize('grow',2048,1),2048);self.table(p)
  p.sql('CREATE EXTENSION pg_buffercache')
  # 调用18新增的真实批量驱逐入口；内部不重复释放调用者持有的accessS。
  relation=p.sql("SELECT * FROM pg_buffercache_evict_relation('resize18_data')").stdout.strip()
  all_buffers=p.sql('SELECT * FROM pg_buffercache_evict_all()').stdout.strip()
  self.data(p);self.committed(p.resize('shrink',512,2),512);self.data(p)
  self.save('actual18-eviction-entrypoints',{'relation':relation,'all':all_buffers,'state':p.state()})
 def test_PG18_S_RESIZE_009_unlogged_sequence_releases_relation_scan_scope(self):
  p=self.cluster();self.committed(p.resize('grow',2048,1),2048)
  generation=int(p.state()['generation'])
  # fill_seq_with_data→FlushRelationBuffers，紧接同backend的resize，不能靠会话退出放锁。
  output=p.sql("CREATE UNLOGGED SEQUENCE resize18_unlogged;SELECT test_dsg_resize18_request('shrink',512,2,"+str(generation)+",2500)").stdout
  result=json.loads(output);self.committed(result,512)
  self.assertEqual(p.sql("SELECT nextval('resize18_unlogged')").stdout.strip(),'1')
  self.save('actual-unlogged-sequence-scan-exit',result)
if __name__=='__main__':unittest.main(verbosity=2)
