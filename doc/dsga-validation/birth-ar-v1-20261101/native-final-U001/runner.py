import sys,os,unittest,json,hashlib,traceback
from pathlib import Path
sys.path.insert(0,'/src/src/test/modules/test_dsg_region18/t')
name=sys.argv[1]
specs={'birth_uid1':['test_birth18.Birth18.test_PG18_U_BIRTH_001_explicit_invalid_modes_and_original_as'],'new4':['test_array_root18.ArrayRootNative18'],'old30':['test_region18.Region','test_arrays18.Arrays','test_resize18.Resize','test_resize_failures18.Failures'],'uring1':['test_resize_failures18.Uring.test_PG18_S_URING_001_real_pin_and_post_unpin_drain']}
expected={'birth_uid1':1,'new4':4,'old30':30,'uring1':1}[name]
rows=[]
class Result(unittest.TextTestResult):
 def addSuccess(self,test):super().addSuccess(test);rows.append({'root':test.id(),'result':'PASS'})
 def addFailure(self,test,err):super().addFailure(test,err);rows.append({'root':test.id(),'result':'FAIL','detail':self._exc_info_to_string(err,test)})
 def addError(self,test,err):super().addError(test,err);rows.append({'root':test.id(),'result':'ERROR','detail':self._exc_info_to_string(err,test)})
 def addSkip(self,test,reason):super().addSkip(test,reason);rows.append({'root':test.id(),'result':'SKIP','reason':reason})
suite=unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromName(s)for s in specs[name])
result=unittest.TextTestRunner(verbosity=2,resultclass=Result).run(suite)
passed=result.wasSuccessful() and not result.skipped and result.testsRun==expected and len(rows)==expected
out={'pass':passed,'scope':'TEST_ONLY actual PG18 native component; AS/R component only; no filter/NativeFinal/Source/normal/VM/K8s qualification','cohort':name,'expected_roots':expected,'tests_run':result.testsRun,'skip':len(result.skipped),'rows':rows,'test_inputs':{str(p.relative_to('/src')):hashlib.sha256(p.read_bytes()).hexdigest()for p in Path('/src/src/test/modules/test_dsg_region18/t').glob('test*.py')}}
Path('/evidence/test-result.json').write_text(json.dumps(out,indent=2,ensure_ascii=False)+'\n')
raise SystemExit(0 if passed else 1)
