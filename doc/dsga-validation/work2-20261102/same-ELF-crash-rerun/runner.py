from pathlib import Path
import json,sys,unittest
sys.path.insert(0,'/src/src/test/modules/test_dsg_region18/t')
import test_work18
class Exact(unittest.TextTestResult):
 def __init__(self,*a,**k):super().__init__(*a,**k);self.passed=[];self.subtests=[]
 def addSuccess(self,t):self.passed.append(t.id());super().addSuccess(t)
 def addSubTest(self,t,s,e):self.subtests.append({'id':str(s),'passed':e is None});super().addSubTest(t,s,e)
suite=unittest.TestSuite([test_work18.Work('test_PG18_S_WORK2_004_actual_crash_new_boot_discards_old_slots')])
r=unittest.TextTestRunner(verbosity=2,resultclass=Exact).run(suite)
Path('/evidence/test-result.json').write_text(json.dumps({'testsRun':r.testsRun,'passed':r.passed,'subtests':r.subtests,'failures':[{'id':str(t),'traceback':e}for t,e in r.failures],'errors':[{'id':str(t),'traceback':e}for t,e in r.errors],'skips':r.skipped,'Source':False,'normal':False,'E':0},indent=2)+'\n')
raise SystemExit(0 if r.wasSuccessful()and r.testsRun==1 and len(r.passed)==1 and not r.skipped else 1)
