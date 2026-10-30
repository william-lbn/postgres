#!/usr/bin/env python3
# 真实PM/child seccomp syscalls；不以此宣称FS/Root/完整normal包络完成。
import importlib.util,json,os,signal,subprocess,time,unittest
from pathlib import Path
from unittest.mock import patch

source=Path(__file__).with_name('test_early_arrays.py')
spec=importlib.util.spec_from_file_location('_early_filter_support',source)
early=importlib.util.module_from_spec(spec);spec.loader.exec_module(early)
FILTER='NEON_DSGA_EARLY_ARRAYS_FILTER'
PROBE='NEON_DSGA_FILTER_PM_PROBE'

class Filter(unittest.TestCase):
    maps=early.EarlyArrays.maps
    covered=early.EarlyArrays.covered
    def cluster(self,filtered=True,probe="exercise-v1"):
        pg=early.Cluster();self.addCleanup(pg.close)
        if filtered:
            original=early.environment
            def env(profile=None,limit=None):
                result=original(profile,limit)
                result.pop(FILTER,None);result.pop(PROBE,None)
                if profile is not None:
                    result[FILTER]='test-v1';result[PROBE]=probe
                return result
            with patch.object(early,'environment',env):pg.start()
        else:pg.start(profile=None,limit=None)
        pg.sql('CREATE EXTENSION test_dsg_region')
        return pg
    def fact(self,pg):return json.loads(pg.sql('SELECT test_dsg_birth_filter()').stdout)
    def probe(self,pg,action):
        return json.loads(pg.sql("SELECT test_dsg_filter_probe('"+action+"')").stdout)
    def test_PG17_U_FILTER_001_explicit_mode_and_missing_r_fail(self):
        for mode,profile,limit in (('', 'test-v1:8192',early.LIMIT),
                    ('root-v1','test-v1:8192',early.LIMIT),
                    ('other','test-v1:8192',early.LIMIT),('test-v1',None,None)):
            with self.subTest(mode=mode,profile=profile):
                env=early.environment(profile,limit);env[FILTER]=mode
                result=subprocess.run([str(early.BIN/'postgres'),'--version'],env=env,
                    preexec_fn=early.nnp,text=True,capture_output=True,timeout=10)
                self.assertEqual(result.returncode,1);self.assertEqual(result.stdout,'')
                expected='DSGA native birth wire:' if mode=='root-v1' else 'DSGA birth filter could not establish'
                self.assertIn(expected,result.stderr)
        pg=self.cluster(filtered=False);self.assertFalse(self.fact(pg)['enabled'])
        self.assertNotEqual(pg.sql("SELECT test_dsg_filter_probe('private-outside')",check=False).returncode,0)
    def test_PG17_S_FILTER_001_actual_syscall_boundaries_and_threads(self):
        pg=self.cluster();fact=self.fact(pg)
        self.assertTrue(all(fact[k] for k in ('enabled','postmaster_installed','child_installed','finalized')))
        self.assertFalse(fact['root_enrolled'])
        self.assertTrue(all(0<fact[k]<4096 for k in ('postmaster_instructions','final_postmaster_instructions','child_instructions')))
        self.assertIn('DSGA_FILTER_PM_TEST_OK exact_rearm_park=4 leader_gate ready_reaped pthread_preserved no_clone3',pg.logs())
        denied=('mprotect-r','mprotect-straddle','pkey-r','munmap-r','munmap-straddle',
                'munmap-wrap','madv-dontfork-r','madv-wipe-r','madv-cold-r',
                'mmap-exact-park','mmap-partial-r','anonymous-shared-outside','shmat-remap',
                'mremap-r','remap-file','fork','clone-nonthread','memfd','splice',
                'zero-copy','set-zero-copy','direct-open','direct-fcntl','io-uring','dumpable')
        for action in denied:
            with self.subTest(action=action):
                result=self.probe(pg,action)
                self.assertEqual(result['result'],-1,result);self.assertEqual(result['errno'],1,result)
        self.assertEqual(self.probe(pg,'clone3'),{'action':'clone3','result':-1,'errno':38})
        for action in ('private-outside','pthread'):
            with self.subTest(action=action):
                result=self.probe(pg,action);self.assertEqual(result['result'],0,result);self.assertEqual(result['errno'],0,result)
        self.assertEqual(pg.sql('SELECT 17').stdout.strip(),'17')
    def test_PG17_S_FILTER_002_real_logger_and_internal_recovery(self):
        pg=self.cluster();pg.functions();peer=pg.peer();logger=pg.logger()
        original,old=early.EarlyArrays.verify(self,pg,peer,logger,'initial-filter')
        pg.sql("CREATE TABLE filter_probe AS SELECT i,md5(i::text) v FROM generate_series(1,100) i;CHECKPOINT")
        expected=pg.sql("SELECT count(*),sum(i),md5(string_agg(v,',' ORDER BY i)) FROM filter_probe").stdout
        self.assertIn('committed high=512',pg.sql('SELECT grow(1,811,1,512,2000,false)').stdout)
        os.kill(logger,signal.SIGKILL);logger=pg.logger(old=logger)
        early.EarlyArrays.verify(self,pg,peer,logger,'filter-logger-restarted')
        for cycle in range(3):
            os.kill(peer.pid,signal.SIGKILL);until=time.monotonic()+15;new=None
            while time.monotonic()<until:
                try:
                    peer=pg.peer();new=peer.query('SELECT arrays()')
                    if new['boot_id']!=old['boot_id']:break
                except (OSError,RuntimeError,StopIteration):pass
                time.sleep(.025)
            self.assertIsNotNone(new);self.assertNotEqual(new['boot_id'],old['boot_id'])
            value,new=early.EarlyArrays.verify(self,pg,peer,logger,'filter-recovery-'+str(cycle))
            self.assertEqual(value,original);self.assertEqual(new['arrays'],old['arrays'])
            self.assertEqual(pg.sql("SELECT count(*),sum(i),md5(string_agg(v,',' ORDER BY i)) FROM filter_probe").stdout,expected)
            self.assertEqual(self.probe(pg,'fork')['errno'],1)
            self.assertEqual(self.probe(pg,'pthread')['result'],0)
            old=new
    def test_PG17_S_FILTER_004_ready_edge_failures_reaped(self):
        started=time.monotonic()
        pg=self.cluster(probe='exercise-negative-v1')
        self.assertGreaterEqual(time.monotonic()-started,10)
        self.assertIn('DSGA_FILTER_READY_NEGATIVE_OK short wrong eof timeout exact_reap=4',pg.logs())
        for mode in ('short','wrong','eof','timeout'):
            self.assertIn('DSGA_READY_CHILD_LIVE mode='+mode,pg.logs())
        self.assertNotIn('DSGA_READY_SETUP_FAILED',pg.logs())
        self.assertTrue(self.fact(pg)['child_installed'])
        self.assertEqual(pg.sql('SELECT 17').stdout.strip(),'17')
    def test_PG17_S_FILTER_003_actual_extension_acl_and_definer(self):
        pg=self.cluster();pg.sql('CREATE ROLE filter_ordinary LOGIN')
        for fn in ('test_dsg_birth_filter()','test_dsg_filter_probe(text)'):
            self.assertEqual(pg.sql("SELECT has_function_privilege('filter_ordinary','"+fn+"','EXECUTE')").stdout.strip(),'f')
        denied=pg.sql('SET ROLE filter_ordinary;SELECT test_dsg_birth_filter()',check=False)
        self.assertNotEqual(denied.returncode,0);self.assertIn('permission denied',denied.stderr)
        pg.sql('GRANT EXECUTE ON FUNCTION test_dsg_birth_filter(),test_dsg_filter_probe(text) TO filter_ordinary')
        pg.sql(f"CREATE FUNCTION filter_definer() RETURNS text AS '{early.LIB}','test_dsg_birth_filter' LANGUAGE C SECURITY DEFINER")
        for sql in ('SET ROLE filter_ordinary;SELECT test_dsg_birth_filter()',
                    "SET ROLE filter_ordinary;SELECT test_dsg_filter_probe('private-outside')",
                    'SET SESSION AUTHORIZATION filter_ordinary;SELECT filter_definer()'):
            denied=pg.sql(sql,check=False);self.assertNotEqual(denied.returncode,0)
            self.assertIn('native filter probes require original administrator',denied.stderr)

if __name__=='__main__':unittest.main(verbosity=2)
