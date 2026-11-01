#!/usr/bin/env python3
# Configuration1只验新命名配置及原profile边界；不授Root/normal，也不回归旧Work/DSM矩阵。
import ctypes,importlib.util,json,os,subprocess,time,unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('configuration18_arrays',Path(__file__).with_name('test_arrays18.py'));arrays=importlib.util.module_from_spec(spec);spec.loader.exec_module(arrays)
region=arrays.region
class Cluster(arrays.Cluster):
 def __init__(self,mode=None,configuration=''):
  super().__init__(mode=mode)
  with(self.data/'postgresql.conf').open('a')as f:f.write(configuration+'\n')
# 当前driver已是UID1000；不调用旧Root helper的setgroups/凭据切换。
# 仍让原PG main实际落实AS，并由early源码核NNP与真实hard/soft值。
libc=ctypes.CDLL(None,use_errno=True)
def configuration_nnp():
 if libc.prctl(38,1,0,0,0)!=0:raise OSError(ctypes.get_errno(),'configuration child PR_SET_NO_NEW_PRIVS')
class EarlyCluster(Cluster):
 def __init__(self,configuration=''):
  super().__init__(mode=None,configuration=configuration)
 def logs(self):
  out=self.log.read_text()if self.log.exists()else''
  for path in(self.data/'logs').glob('*'):out+='\n'+path.read_text()
  return out
 def start(self,check=True):
  env=region.env();env['NEON_DSGA_EARLY_ARRAYS_PROFILE']='test-v1:2048';env['NEON_DSGA_ADDRESS_SPACE_MAX_BYTES']=str(2<<30)
  self.stream=self.log.open('a');self.process=subprocess.Popen([str(region.BIN/'postgres'),'-D',str(self.data)],env=env,stdout=self.stream,stderr=subprocess.STDOUT,preexec_fn=configuration_nnp)
  until=time.monotonic()+15
  while time.monotonic()<until:
   if self.process.poll()is not None:
    if check:raise RuntimeError('actual PG early configuration exit '+self.logs())
    return False
   if self.sql('SELECT 1',check=False).returncode==0:
    self.sql('CREATE EXTENSION IF NOT EXISTS test_dsg_region18');return True
   time.sleep(.025)
  raise RuntimeError('actual PG early configuration original15s deadline '+self.logs())
 def birth(self):return json.loads(self.sql('SELECT test_dsg_birth_arrays18()').stdout)
class Configuration(unittest.TestCase):
 def keep(self,p):self.addCleanup(p.close);return p
 def snapshot(self,p):return json.loads(p.sql('SELECT test_dsg_arrays18_snapshot()').stdout)
 def settings(self,p):return json.loads(p.sql("SELECT json_object_agg(name,json_build_object('setting',setting,'context',context,'vartype',vartype,'unit',unit,'boot',boot_val,'min',min_val,'max',max_val)) FROM pg_settings WHERE name IN ('enable_dynamic_shared_buffers','max_shared_buffers')").stdout)
 def fact(self,name,value):
  if os.environ.get('NEON_PG18_EVIDENCE'):Path(os.environ['NEON_PG18_EVIDENCE'],name+'.json').write_text(json.dumps(value,indent=2)+'\n')
 def test_PG18_U_CONFIGURATION1_001_same_GUC_metadata_and_disabled_default(self):
  p=self.keep(Cluster());p.start();v=self.settings(p)
  self.assertEqual(set(v),{'enable_dynamic_shared_buffers','max_shared_buffers'})
  enabled=v['enable_dynamic_shared_buffers'];maximum=v['max_shared_buffers']
  self.assertEqual((enabled['setting'],enabled['boot'],enabled['context'],enabled['vartype']),('off','off','postmaster','bool'))
  self.assertEqual((maximum['setting'],maximum['boot'],maximum['context'],maximum['vartype'],maximum['unit'],maximum['min'],maximum['max']),('0','0','postmaster','integer','8kB','0','1073741823'))
  self.assertFalse(self.snapshot(p)['enabled']);self.fact('configuration1-original-disabled-metadata',v)
 def test_PG18_S_CONFIGURATION1_001_matching_native_and_early_profile_maximum(self):
  facts=[]
  for kind in ['native','early']:
   with self.subTest(profile=kind):
    config="enable_dynamic_shared_buffers=on\nmax_shared_buffers=2048"
    p=self.keep(Cluster('test-v1:2048',config)if kind=='native'else EarlyCluster(config));p.start();s=self.snapshot(p);v=self.settings(p)
    self.assertEqual(v['enable_dynamic_shared_buffers']['setting'],'on');self.assertEqual(v['max_shared_buffers']['setting'],'2048');self.assertTrue(s['enabled']);self.assertEqual((s['maximum_buffers'],s['startup_buffers']),(2048,512));self.assertFalse(s['normal_permit']);self.assertEqual(p.sql('SELECT 17+18').stdout.strip(),'35')
    if kind=='early':
     b=p.birth();self.assertTrue(b['enabled']);self.assertFalse(b['source_bound']);self.assertFalse(b['root_enrolled']);self.assertEqual(b['maximum_buffers'],2048)
    facts.append({'profile':kind,'settings':v,'arrays':s,'Source':False,'normal':False})
  self.fact('configuration1-matching-actual-profiles',facts)
 def test_PG18_S_CONFIGURATION1_002_no_profile_and_conflicting_original_maximum_refused(self):
  facts=[]
  for kind in ['no-profile','native-conflict','early-conflict']:
   with self.subTest(profile=kind):
    config='enable_dynamic_shared_buffers=on\nmax_shared_buffers='+('2048'if kind=='no-profile'else'1024')
    if kind=='early-conflict':
     p=self.keep(EarlyCluster(config));self.assertFalse(p.start(check=False));rc=p.process.returncode;log=p.logs()
    else:
     p=self.keep(Cluster(None if kind=='no-profile'else'test-v1:2048',config));env=region.env()
     if p.mode is not None:env['NEON_DSGA_PG18_NATIVE_ARRAYS']=p.mode
     x=subprocess.run([str(region.BIN/'postgres'),'-D',str(p.data)],env=env,capture_output=True,text=True,timeout=15);rc=x.returncode;log=x.stderr+x.stdout
    self.assertNotEqual(rc,0)
    self.assertIn('require an explicit PG18 native or early birth profile'if kind=='no-profile'else'differs from the original PG18 array profile maximum',log)
    self.assertNotIn('database system is ready to accept connections',log);facts.append({'case':kind,'exit':rc,'actual_error':log,'Source':False,'normal':False})
  self.fact('configuration1-real-startup-refusals',facts)
 def test_PG18_S_CONFIGURATION1_003_postmaster_setting_cannot_change_running_geometry(self):
  p=self.keep(Cluster('test-v1:2048','enable_dynamic_shared_buffers=on\nmax_shared_buffers=2048'));p.start();before=self.snapshot(p)
  for sql in ['SET enable_dynamic_shared_buffers=off','SET max_shared_buffers=1024']:
   bad=p.sql(sql,check=False);self.assertNotEqual(bad.returncode,0);self.assertIn('cannot be changed without restarting the server',bad.stderr)
  p.sql('ALTER SYSTEM SET max_shared_buffers=4096');self.assertEqual(p.sql('SELECT pg_reload_conf()').stdout.strip(),'t');until=time.monotonic()+5;pending=''
  while time.monotonic()<until:
   pending=p.sql("SELECT setting,pending_restart FROM pg_settings WHERE name='max_shared_buffers'").stdout.strip()
   if pending=='2048|t':break
   time.sleep(.02)
  self.assertEqual(pending,'2048|t');after=self.snapshot(p);self.assertEqual(after['maximum_buffers'],before['maximum_buffers']);self.assertEqual([x['base']for x in after['arrays']],[x['base']for x in before['arrays']]);self.assertFalse(after['normal_permit']);self.fact('configuration1-postmaster-runtime-immutable',{'before':before,'after':after,'pending':pending,'Source':False,'normal':False})
if __name__=='__main__':unittest.main(verbosity=2)
