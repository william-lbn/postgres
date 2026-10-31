from pathlib import Path
import importlib.util,json,os,time,traceback,unittest,hashlib,subprocess
source=Path('/home/nonroot/vendor/postgres-v17/src/test/modules/test_dsg_region/t/test_filter.py')
spec=importlib.util.spec_from_file_location('native_filter_support',source);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class Case(m.Filter):
 def test_PG17_S_MIGFILTER_001_actual_native_child_denies_migration_and_import(self):
  observations=[]
  for filtered in (False,True):
   pg=self.cluster(filtered=filtered)
   pg.sql("CREATE FUNCTION migration_filter_probe() RETURNS text AS '/probe/migration_filter_probe.so','migration_filter_probe' LANGUAGE C")
   fact=json.loads(pg.sql('SELECT migration_filter_probe()').stdout)
   self.assertFalse(fact['normal_permit']);self.assertEqual(fact['child_installed'],filtered);self.assertEqual(fact['finalized'],filtered)
   observations.append(fact)
  baseline,filtered=observations
  for name,value in filtered['calls'].items():
   self.assertEqual(value,{'result':-1,'errno':1},(name,value))
  self.assertEqual(filtered['calls']['set_mempolicy_home_node'],{'result':-1,'errno':1})
  # Concrete baseline differences exclude attributing every EPERM to the native program.
  changed=[name for name,v in filtered['calls'].items()if v!=baseline['calls'][name]]
  self.assertTrue({'setns','io_uring_enter','io_uring_register','process_madvise','process_mrelease'}.issubset(changed),changed)
  Path('/probe/observations.json').write_text(json.dumps({'baseline':baseline,'actual_native_child':filtered,'different_from_unfiltered':changed,'kernel_release':os.uname().release,'scope':'isolated Linux native PG17 service; not KVM or normal qualification'},indent=2)+'\n')
suite=unittest.TestSuite([Case('test_PG17_S_MIGFILTER_001_actual_native_child_denies_migration_and_import')]);tick=time.monotonic();r=unittest.TextTestRunner(verbosity=2).run(suite)
Path('/probe/result.json').write_text(json.dumps({'roots':r.testsRun,'pass':r.wasSuccessful(),'skip':len(r.skipped),'seconds':time.monotonic()-tick,'errors':[v for _,v in r.errors+r.failures]},indent=2)+'\n');raise SystemExit(0 if r.wasSuccessful() else 1)
