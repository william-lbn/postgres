from pathlib import Path
import json,sys,unittest,os,signal
sys.path.insert(0,'/src/src/test/modules/test_dsg_region18/t')
import test_work18 as work
class Baseline(unittest.TestCase):
 def check(self,checkpoint):
  p=work.arrays.Cluster('test-v1:2048','worker');self.addCleanup(p.close);p.start();p.sql('CREATE ROLE work18_user LOGIN')
  if checkpoint:p.sql('CHECKPOINT')
  initial=json.loads(p.sql('SELECT test_dsg_resize18_state()').stdout);w=work.Wire(p);self.addCleanup(w.close);self.assertFalse(w.query('BEGIN')[0]);pm=p.process.pid
  pf=os.pidfd_open(w.pid);self.addCleanup(os.close,pf);signal.pidfd_send_signal(pf,signal.SIGKILL);w.close()
  if checkpoint:
   p.ready();after=json.loads(p.sql('SELECT test_dsg_resize18_state()').stdout);self.assertEqual(p.process.pid,pm);self.assertNotEqual(initial['boot'],after['boot']);result={'checkpoint':True,'same_PM':pm,'old_boot':initial['boot'],'new_boot':after['boot'],'recovery':'PASS'}
  else:
   with self.assertRaises(RuntimeError):p.ready()
   text=''.join(q.read_text()for q in(p.data/'logs').glob('*'));self.assertIn('resource manager with ID 134 not registered',text);result={'checkpoint':False,'PM':pm,'old_boot':initial['boot'],'recovery':'EXACT_BASELINE_RM134_FAILURE_REPRODUCED'}
  Path('/evidence/baseline-'+str(checkpoint)+'.json').write_text(json.dumps(result,indent=2)+'\n')
 def test_PG18_ENV_WORK2_BASELINE_001_without_initial_checkpoint_fails_RM134(self):self.check(False)
 def test_PG18_ENV_WORK2_BASELINE_002_with_checkpoint_same_PM_new_boot(self):self.check(True)
class Exact(unittest.TextTestResult):
 def __init__(self,*a,**k):super().__init__(*a,**k);self.passed=[]
 def addSuccess(self,t):self.passed.append(t.id());super().addSuccess(t)
r=unittest.TextTestRunner(verbosity=2,resultclass=Exact).run(unittest.defaultTestLoader.loadTestsFromTestCase(Baseline))
Path('/evidence/test-result.json').write_text(json.dumps({'testsRun':r.testsRun,'passed':r.passed,'failures':[{'id':str(t),'traceback':e}for t,e in r.failures],'errors':[{'id':str(t),'traceback':e}for t,e in r.errors],'skips':r.skipped,'scope':'OLD_UNMODIFIED_PG18_ELF_BASELINE_FIXTURE_CONTROLS_ONLY_NOT_WORK2_ACCEPTANCE','Source':False,'normal':False,'E':0},indent=2)+'\n')
raise SystemExit(0 if r.wasSuccessful()and r.testsRun==2 and len(r.passed)==2 and not r.skipped else 1)
