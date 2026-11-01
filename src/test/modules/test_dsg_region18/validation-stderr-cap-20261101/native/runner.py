from pathlib import Path
import unittest,json,sys,time,os
sys.path.insert(0,'/src/src/test/modules/test_dsg_region18/t')
import test_pipe_capacity18 as pipe
import test_root_birth18 as root
suite=unittest.TestSuite([unittest.defaultTestLoader.loadTestsFromTestCase(pipe.Pipe18),root.RootBirth18('test_PG18_S_ROOTWIRE_001_real_exact_chain_detach')])
class ExactResult(unittest.TextTestResult):
 def __init__(self,*a,**k):super().__init__(*a,**k);self.passed=[];self.subtests=[]
 def addSuccess(self,test):self.passed.append(test.id());super().addSuccess(test)
 def addSubTest(self,test,subtest,err):self.subtests.append({'id':str(subtest),'passed':err is None});super().addSubTest(test,subtest,err)
result=unittest.TextTestRunner(verbosity=2,resultclass=ExactResult).run(suite)
summary={'testsRun':result.testsRun,'passed':result.passed,'subtests':result.subtests,'failures':[{'id':str(t),'traceback':x}for t,x in result.failures],'errors':[{'id':str(t),'traceback':x}for t,x in result.errors],'skips':result.skipped,'Source':False,'normal':False,'PG_version':18,'new_stable_roots':2,'directly_affected_old_root_rerun':1,'E':0}
Path('/evidence/test-result.json').write_text(json.dumps(summary,indent=2)+'\n')
raise SystemExit(0 if result.wasSuccessful()and result.testsRun==3 and len(result.passed)==3 and not result.skipped else 1)
