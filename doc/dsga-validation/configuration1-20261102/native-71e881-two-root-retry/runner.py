from pathlib import Path
import json,sys,unittest
sys.path.insert(0,'/src/src/test/modules/test_dsg_region18/t')
import test_configuration18
class Exact(unittest.TextTestResult):
 def __init__(self,*a,**k):super().__init__(*a,**k);self.passed=[];self.subtests=[]
 def addSuccess(self,t):self.passed.append(t.id());super().addSuccess(t)
 def addSubTest(self,t,s,e):self.subtests.append({'id':str(s),'passed':e is None});super().addSubTest(t,s,e)
suite=unittest.TestSuite([test_configuration18.Configuration('test_PG18_S_CONFIGURATION1_001_matching_native_and_early_profile_maximum'),test_configuration18.Configuration('test_PG18_S_CONFIGURATION1_002_no_profile_and_conflicting_original_maximum_refused')])
r=unittest.TextTestRunner(verbosity=2,resultclass=Exact).run(suite)
Path('/evidence/test-result.json').write_text(json.dumps({'testsRun':r.testsRun,'passed':r.passed,'subtests':r.subtests,'failures':[{'id':str(t),'traceback':e}for t,e in r.failures],'errors':[{'id':str(t),'traceback':e}for t,e in r.errors],'skips':r.skipped,'Source':False,'normal':False,'E':0},indent=2)+'\n')
raise SystemExit(0 if r.wasSuccessful()and r.testsRun==2 and len(r.passed)==2 and not r.skipped else 1)
