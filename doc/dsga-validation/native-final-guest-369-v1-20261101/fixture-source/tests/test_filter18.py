#!/usr/bin/env python3
# 真实PG18 PM/child cBPF与AIO组件；无原Source/FS/I或normal授权。
import json,os,signal,subprocess,time,unittest
from pathlib import Path
import test_birth18 as birth
import test_array_root18 as core
FILTER='NEON_DSGA_EARLY_ARRAYS_FILTER'
PROBE='NEON_DSGA_FILTER_PM_PROBE'
class Cluster(birth.Cluster):
    def __init__(self,filtered=True,probe='exercise-v1',method='worker'):
        super().__init__(early='test-v1:2048'if filtered else None,address_space=str(birth.AS)if filtered else None)
        self.filtered=filtered;self.probe_mode=probe
        with(self.data/'postgresql.conf').open('a')as f:f.write(f"shared_preload_libraries='test_dsg_region18'\nio_method='{method}'\n")
    def start(self,check=True):
        env=core.environment()
        if self.filtered:
            env['NEON_DSGA_EARLY_ARRAYS_PROFILE']='test-v1:2048';env['NEON_DSGA_ADDRESS_SPACE_MAX_BYTES']=str(birth.AS);env[FILTER]='test-v1'
            if self.probe_mode is not None:env[PROBE]=self.probe_mode
        self.stream=self.log.open('a')
        self.process=subprocess.Popen([str(core.BIN/'postgres'),'-D',str(self.data)],env=env,stdout=self.stream,stderr=subprocess.STDOUT,preexec_fn=birth.nnp,**core.credentials())
        until=time.monotonic()+15
        while time.monotonic()<until:
            if self.process.poll() is not None:
                if check:raise RuntimeError('PG18 native filter startup '+self.logs()[-12000:])
                return False
            if self.sql('SELECT 1',check=False).returncode==0:
                self.sql('CREATE EXTENSION test_dsg_region18');return True
            time.sleep(.025)
        raise RuntimeError('PG18 native filter original15s startup deadline '+self.logs()[-12000:])
class Filter18(unittest.TestCase):
    maps=birth.Birth18.maps
    ranges=birth.Birth18.ranges
    original_limits=birth.Birth18.original_limits
    held_backend=birth.Birth18.held_backend
    def cluster(self,**kwargs):
        pg=Cluster(**kwargs);self.addCleanup(pg.close);pg.start();return pg
    def fact(self,pg):return json.loads(pg.sql('SELECT test_dsg_birth_filter18()').stdout)
    def probe(self,pg,action):return json.loads(pg.sql("SELECT test_dsg_filter_probe18('"+action+"')").stdout)
    def save(self,name,data):
        target=os.environ.get('NEON_PG18_EVIDENCE')
        if target:Path(target,name+'.json').write_text(json.dumps(data,indent=2)+'\n')
    def test_PG18_U_FILTER_001_explicit_mode_missing_r_and_protected_uring_fail(self):
        for mode,early,space in [('', 'test-v1:2048',str(birth.AS)),('root-v1','test-v1:2048',str(birth.AS)),('other','test-v1:2048',str(birth.AS)),('test-v1',None,None)]:
            with self.subTest(mode=mode,early=early):
                env=core.environment();env[FILTER]=mode
                if early is not None:env['NEON_DSGA_EARLY_ARRAYS_PROFILE']=early
                if space is not None:env['NEON_DSGA_ADDRESS_SPACE_MAX_BYTES']=space
                result=subprocess.run([str(core.BIN/'postgres'),'--version'],env=env,preexec_fn=birth.nnp,capture_output=True,text=True,timeout=10,**core.credentials())
                self.assertEqual(result.returncode,1);self.assertEqual(result.stdout,'')
                self.assertIn('DSGA native birth wire:'if mode=='root-v1'else'DSGA birth filter could not establish',result.stderr)
        pg=self.cluster(filtered=False);self.assertFalse(self.fact(pg)['enabled'])
        self.assertNotEqual(pg.sql("SELECT test_dsg_filter_probe18('private-outside')",check=False).returncode,0)
        unsupported=Cluster(probe=None,method='io_uring');self.addCleanup(unsupported.close)
        self.assertFalse(unsupported.start(check=False));self.assertIn('PG18 protected birth profile does not support io_uring',unsupported.logs())
        self.save('actual-protected-uring-reject',{'exit':unsupported.process.returncode,'logs':unsupported.logs(),'default_off_uring_still_preserved':True})
    def test_PG18_S_FILTER_001_actual_syscall_boundaries_threads_and_AIO(self):
        facts=[]
        for method in ['sync','worker']:
            with self.subTest(method=method):
                pg=self.cluster(method=method);fact=self.fact(pg)
                self.assertTrue(all(fact[k]for k in ['enabled','postmaster_installed','child_installed','finalized']))
                self.assertFalse(fact['root_enrolled'])
                self.assertTrue(all(0<fact[k]<4096 for k in ['postmaster_instructions','final_postmaster_instructions','child_instructions']))
                self.assertIn('DSGA_FILTER_PM_TEST_OK exact_rearm_park=4 leader_gate ready_reaped pthread_preserved no_clone3',pg.logs())
                denied=['mprotect-r','mprotect-straddle','pkey-r','munmap-r','munmap-straddle','munmap-wrap','madv-dontfork-r','madv-wipe-r','madv-cold-r','mmap-exact-park','mmap-partial-r','anonymous-shared-outside','shmat-remap','mremap-r','remap-file','fork','clone-nonthread','memfd','splice','zero-copy','set-zero-copy','direct-open','direct-fcntl','io-uring','dumpable']
                observations=[]
                for action in denied:
                    result=self.probe(pg,action);self.assertEqual((result['result'],result['errno']),(-1,1),result);observations.append(result)
                self.assertEqual(self.probe(pg,'clone3'),{'action':'clone3','result':-1,'errno':38})
                for action in ['private-outside','pthread']:
                    result=self.probe(pg,action);self.assertEqual((result['result'],result['errno']),(0,0),result);observations.append(result)
                pg.sql('CREATE TABLE filter18_data AS SELECT i,md5(i::text) v FROM generate_series(1,5000)i;CHECKPOINT')
                self.assertEqual(pg.sql('SELECT count(*),sum(i) FROM filter18_data').stdout.strip(),'5000|12502500')
                self.assertEqual(pg.sql('SHOW io_method').stdout.strip(),method)
                if method=='worker':self.assertEqual(int(pg.sql("SELECT count(*) FROM pg_stat_activity WHERE backend_type='io worker'").stdout),2)
                facts.append({'method':method,'filter':fact,'syscalls':observations,'logs':pg.logs()})
        self.save('actual-filter-syscalls-AIO',facts)
    def test_PG18_S_FILTER_002_real_logger_internal_recovery_and_IO_workers(self):
        pg=self.cluster();original=pg.birth();old=pg.state()['boot']
        pg.sql('CREATE TABLE filter18_recovery AS SELECT i,md5(i::text) v FROM generate_series(1,100)i;CHECKPOINT;SELECT test_dsg_resize18_prepare()')
        expected=pg.sql("SELECT count(*),sum(i),md5(string_agg(v,',' ORDER BY i)) FROM filter18_recovery").stdout
        grow=pg.resize('grow',1024,1);self.assertEqual(grow['result_phase'],2)
        logger=pg.logger();core.send_pidfd(logger,signal.SIGKILL);logger=pg.logger(old=logger)
        facts=[{'phase':'logger-restarted','maps':self.ranges(logger,original,False),'limits':self.original_limits(logger)}]
        for cycle in range(3):
            holder,victim=self.held_backend(pg);core.send_pidfd(victim,signal.SIGKILL)
            holder.communicate(timeout=10);self.assertNotEqual(holder.returncode,0)
            until=time.monotonic()+15;current=None
            while time.monotonic()<until:
                try:
                    current=pg.state()['boot']
                    if current!=old:break
                except(OSError,RuntimeError):pass
                time.sleep(.025)
            self.assertIsNotNone(current,pg.logs()[-9000:]);self.assertNotEqual(current,old);old=current
            now=pg.birth();self.assertEqual(now['ranges'],original['ranges'])
            self.assertEqual(pg.sql("SELECT count(*),sum(i),md5(string_agg(v,',' ORDER BY i)) FROM filter18_recovery").stdout,expected)
            self.assertEqual(int(pg.sql("SELECT count(*) FROM pg_stat_activity WHERE backend_type='io worker'").stdout),2)
            self.assertEqual(self.probe(pg,'fork')['errno'],1);self.assertEqual(self.probe(pg,'pthread')['result'],0)
            facts.append({'cycle':cycle,'boot':current,'filter':self.fact(pg),'PMmaps':self.ranges(pg.process.pid,now,True),'loggerMaps':self.ranges(logger,now,False),'PMlimits':self.original_limits(pg.process.pid)})
        self.save('actual-filter-three-recoveries',facts)
    def test_PG18_S_FILTER_003_actual_extension_acl_and_definer(self):
        pg=self.cluster();pg.sql('CREATE ROLE filter18_ordinary LOGIN')
        for fn in ['test_dsg_birth_filter18()','test_dsg_filter_probe18(text)']:
            self.assertEqual(pg.sql("SELECT has_function_privilege('filter18_ordinary','"+fn+"','EXECUTE')").stdout.strip(),'f')
        r=pg.sql('SET ROLE filter18_ordinary;SELECT test_dsg_birth_filter18()',check=False)
        self.assertNotEqual(r.returncode,0);self.assertIn('permission denied',r.stderr)
        pg.sql('GRANT EXECUTE ON FUNCTION test_dsg_birth_filter18(),test_dsg_filter_probe18(text) TO filter18_ordinary')
        pg.sql(f"CREATE FUNCTION filter18_definer() RETURNS text AS '{core.LIB}','test_dsg_birth_filter18' LANGUAGE C SECURITY DEFINER")
        for sql in ['SET ROLE filter18_ordinary;SELECT test_dsg_birth_filter18()',"SET ROLE filter18_ordinary;SELECT test_dsg_filter_probe18('private-outside')",'SET SESSION AUTHORIZATION filter18_ordinary;SELECT filter18_definer()']:
            r=pg.sql(sql,check=False);self.assertNotEqual(r.returncode,0);self.assertIn('native filter probes require original administrator',r.stderr)
    def test_PG18_S_FILTER_004_ready_edge_failures_exactly_reaped(self):
        start=time.monotonic();pg=self.cluster(probe='exercise-negative-v1')
        self.assertGreaterEqual(time.monotonic()-start,10)
        self.assertIn('DSGA_FILTER_READY_NEGATIVE_OK short wrong eof timeout exact_reap=4',pg.logs())
        for mode in ['short','wrong','eof','timeout']:self.assertIn('DSGA_READY_CHILD_LIVE mode='+mode,pg.logs())
        self.assertNotIn('DSGA_READY_SETUP_FAILED',pg.logs());self.assertTrue(self.fact(pg)['child_installed'])
        self.save('actual-ready-negative-reap',{'seconds':time.monotonic()-start,'logs':pg.logs()})
if __name__=='__main__':unittest.main(verbosity=2)
