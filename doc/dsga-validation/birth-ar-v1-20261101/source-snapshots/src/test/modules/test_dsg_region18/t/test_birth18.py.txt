#!/usr/bin/env python3
# PG18 原生AS/R组件；没有filter/NativeFinal/原Source或normal资格。
import ctypes,hashlib,json,os,re,signal,subprocess,time,unittest
from pathlib import Path
import test_array_root18 as core
libc=ctypes.CDLL(None,use_errno=True)
AS=2<<30
def nnp():
    if libc.prctl(38,1,0,0,0)!=0:raise OSError(ctypes.get_errno(),'test child PR_SET_NO_NEW_PRIVS')
class Cluster(core.Cluster):
    def __init__(self,early='test-v1:2048',address_space=str(AS),legacy=None,no_new_privileges=True):
        self.early=early;self.address_space=address_space;self.legacy=legacy;self.nnp=no_new_privileges
        super().__init__()
    def start(self,check=True):
        env=core.environment()
        for key,value in [('NEON_DSGA_EARLY_ARRAYS_PROFILE',self.early),('NEON_DSGA_ADDRESS_SPACE_MAX_BYTES',self.address_space),('NEON_DSGA_PG18_NATIVE_ARRAYS',self.legacy)]:
            if value is not None:env[key]=value
        self.stream=self.log.open('a')
        self.process=subprocess.Popen([str(core.BIN/'postgres'),'-D',str(self.data)],env=env,stdout=self.stream,stderr=subprocess.STDOUT,preexec_fn=nnp if self.nnp else None,**core.credentials())
        until=time.monotonic()+15
        while time.monotonic()<until:
            if self.process.poll() is not None:
                if check:raise RuntimeError('real PG18 AS/R exit='+str(self.process.returncode)+' '+self.logs()[-9000:])
                return False
            if self.sql('SELECT 1',check=False).returncode==0:
                self.sql('CREATE EXTENSION test_dsg_region18');return True
            time.sleep(.025)
        raise RuntimeError('PG18 AS/R original15s startup deadline '+self.logs()[-9000:])
    def birth(self):return json.loads(self.sql('SELECT test_dsg_birth_arrays18()').stdout)
class Birth18(unittest.TestCase):
    def cluster(self,**kwargs):
        pg=Cluster(**kwargs);self.addCleanup(pg.close);pg.start();return pg
    def save(self,name,data):
        target=os.environ.get('NEON_PG18_EVIDENCE')
        if target:Path(target,name+'.json').write_text(json.dumps(data,indent=2)+'\n')
    def maps(self,pid):
        raw=Path(f'/proc/{pid}/maps').read_text();out=[]
        for line in raw.splitlines():
            x=line.split();low,high=(int(a,16)for a in x[0].split('-'))
            out.append((low,high,x[1],int(x[4])))
        return raw,out
    def original_limits(self,pid):
        limits=Path(f'/proc/{pid}/limits').read_text()
        match=re.search(r'^Max address space\s+(\S+)\s+(\S+)',limits,re.MULTILINE)
        self.assertIsNotNone(match);self.assertEqual((int(match[1]),int(match[2])),(AS,AS))
        status=Path(f'/proc/{pid}/status').read_text()
        self.assertRegex(status,r'(?m)^NoNewPrivs:\s+1$')
        return {'pid':pid,'limits':limits,'status':status}
    def ranges(self,pid,birth,shared):
        raw,rows=self.maps(pid)
        for r in birth['ranges']:
            base=int(r['base_address']);end=base+int(r['reserved_bytes'])
            found=[row for row in rows if row[0]<=base and end<=row[1]]
            self.assertEqual(len(found),1,(pid,r,found))
            self.assertEqual(found[0][2],'rw-s'if shared else'---p',(pid,r,found))
            if shared:self.assertGreater(found[0][3],0)
            else:self.assertEqual(found[0][3],0)
        return raw
    def held_backend(self,pg):
        holder=pg.asynchronous('SELECT pg_backend_pid(),pg_sleep(30)')
        until=time.monotonic()+3
        while time.monotonic()<until:
            x=pg.sql("SELECT pid FROM pg_stat_activity WHERE query LIKE 'SELECT pg_backend_pid(),pg_sleep(30)%'").stdout.strip()
            if x:return holder,int(x)
            time.sleep(.025)
        holder.kill();holder.communicate(timeout=10);self.fail('real held backend absent')
    def test_PG18_U_BIRTH_001_explicit_invalid_modes_and_original_as(self):
        cases=[
            ('bad','2147483648','DSGA early arrays'),
            ('test-v1:0','2147483648','DSGA early arrays'),
            ('test-v1:02048','2147483648','DSGA early arrays'),
            ('test-v1:2147483648','2147483648','DSGA early arrays'),
            ('root-v1:2048','2147483648','PG18 root-v1'),
            ('test-v1:2048',None,'DSGA early arrays'),
            ('test-v1:2048','65536','reserved arrays'),
            ('test-v1:2048','0','invalid DSGA'),
            ('test-v1:2048','02147483648','invalid DSGA'),
            ('test-v1:2048','2147483649','invalid DSGA'),
            ('test-v1:2048','9223372036854775808','invalid DSGA')]
        observations=[]
        for early,space,reason in cases:
            with self.subTest(early=early,address_space=space):
                env=core.environment();env['NEON_DSGA_EARLY_ARRAYS_PROFILE']=early
                if space is not None:env['NEON_DSGA_ADDRESS_SPACE_MAX_BYTES']=space
                r=subprocess.run([str(core.BIN/'postgres'),'--version'],env=env,preexec_fn=nnp,capture_output=True,text=True,timeout=10,**core.credentials())
                self.assertNotEqual(r.returncode,0);self.assertIn(reason,r.stderr)
                observations.append({'early':early,'as':space,'exit':r.returncode,'stderr':r.stderr})
        # 原AS即使NNP1也拒绝真实root；preexec混合UID可在exec降权，
        # 不能把preexec值误当PG入口身份。真实归一诊断另档保留，不签Source资格。
        def root_uid():
            if (os.getuid(),os.geteuid())!=(0,0):raise RuntimeError('real root uid setup failed')
            nnp()
        for label,prepare in [('root',root_uid)]:
            with self.subTest(actual_identity=label):
                env=core.environment();env['NEON_DSGA_EARLY_ARRAYS_PROFILE']='test-v1:2048';env['NEON_DSGA_ADDRESS_SPACE_MAX_BYTES']=str(AS)
                result=subprocess.run([str(core.BIN/'postgres'),'--version'],env=env,preexec_fn=prepare,user=0,group=0,extra_groups=[],capture_output=True,text=True,timeout=10)
                self.assertNotEqual(result.returncode,0);self.assertIn('unprivileged no-new-privileges birth',result.stderr)
                observations.append({'actual_identity':label,'NNP':1,'exit':result.returncode,'stderr':result.stderr})
        self.save('actual-invalid-original-as-r',observations)
    def test_PG18_U_BIRTH_002_no_nnp_cannot_enable(self):
        self.assertEqual(libc.prctl(39,0,0,0,0),0,'fixture parent must actually have NNP0 for this negative')
        env=core.environment();env['NEON_DSGA_EARLY_ARRAYS_PROFILE']='test-v1:2048';env['NEON_DSGA_ADDRESS_SPACE_MAX_BYTES']=str(AS)
        r=subprocess.run([str(core.BIN/'postgres'),'--version'],env=env,capture_output=True,text=True,timeout=10,**core.credentials())
        self.assertNotEqual(r.returncode,0);self.assertIn('no-new-privileges',r.stderr)
        self.save('actual-no-nnp-reject',{'exit':r.returncode,'stderr':r.stderr,'actual_parent_nnp':libc.prctl(39,0,0,0,0)})
    def test_PG18_S_BIRTH_001_default_disabled_real_pg(self):
        pg=self.cluster(early=None,address_space=None);fact=pg.birth()
        self.assertFalse(fact['enabled']);self.assertFalse(fact['imported']);self.assertFalse(fact['source_bound']);self.assertFalse(fact['root_enrolled']);self.assertEqual(fact['ranges'],[])
        self.assertEqual(pg.sql('SELECT 18').stdout.strip(),'18');self.save('actual-default-disabled',fact)
    def test_PG18_S_BIRTH_002_actual_geometry_children_and_stale_inputs(self):
        pg=self.cluster();fact=pg.birth()
        self.assertTrue(fact['enabled']);self.assertTrue(fact['imported']);self.assertFalse(fact['source_bound']);self.assertFalse(fact['root_enrolled'])
        self.assertEqual((fact['maximum_buffers'],int(fact['address_space_max_bytes'])),(2048,AS))
        self.assertEqual(sum(int(r['reserved_bytes'])for r in fact['ranges']),int(fact['reserved_total_bytes']))
        snapshot=json.loads(pg.sql('SELECT test_dsg_arrays18_snapshot()').stdout)
        for a,r in zip(snapshot['arrays'],fact['ranges']):
            self.assertEqual(a['base'],r['base_address']);self.assertEqual(int(a['maximum_bytes']),int(r['element_bytes'])*2048)
        pg.sql('CREATE TABLE birth18_data AS SELECT i FROM generate_series(1,1000)i;CHECKPOINT')
        holder,pid=self.held_backend(pg)
        try:
            pids=[pg.process.pid,pid]+[int(x)for x in pg.sql("SELECT pid FROM pg_stat_activity WHERE backend_type='io worker'").stdout.splitlines()]
            self.assertEqual(len(pids),4)
            logger=pg.logger();limits=[self.original_limits(x)for x in pids+[logger]]
            maps={str(x):self.ranges(x,fact,True)for x in pids};maps[str(logger)]=self.ranges(logger,fact,False)
        finally:holder.kill();holder.communicate(timeout=10)
        self.save('actual-original-as-r-children',{'birth':fact,'arrays':snapshot,'processes':limits,'maps':maps})
        # 两个明确输入都存在时，旧18测试env不能替换原M。
        conflict=Cluster(legacy='test-v1:4096');self.addCleanup(conflict.close)
        self.assertFalse(conflict.start(check=False));self.assertIn('differs from original early reservations',conflict.logs())
        self.save('actual-stale-maximum-reject',{'exit':conflict.process.returncode,'log':conflict.logs()})
    def test_PG18_S_BIRTH_003_real_logger_and_recovery_keep_r(self):
        pg=self.cluster();original=pg.birth();oldboot=pg.state()['boot']
        pg.sql('CREATE TABLE birth18_recovery AS SELECT i FROM generate_series(1,100)i;CHECKPOINT')
        logger=pg.logger();core.send_pidfd(logger,signal.SIGKILL);logger=pg.logger(old=logger)
        evidence=[{'phase':'restarted-logger','facts':self.original_limits(logger),'maps':self.ranges(logger,original,False)}]
        for cycle in range(3):
            holder,victim=self.held_backend(pg);core.send_pidfd(victim,signal.SIGKILL)
            holder.communicate(timeout=10);self.assertNotEqual(holder.returncode,0)
            until=time.monotonic()+15;current=None
            while time.monotonic()<until:
                try:
                    current=pg.state()['boot']
                    if current!=oldboot:break
                except (OSError,RuntimeError):pass
                time.sleep(.025)
            self.assertIsNotNone(current,pg.logs()[-8000:]);self.assertNotEqual(current,oldboot);oldboot=current
            birth=pg.birth();self.assertEqual(birth['ranges'],original['ranges'])
            self.assertEqual(pg.sql('SELECT count(*),sum(i) FROM birth18_recovery').stdout.strip(),'100|5050')
            workers=[int(x)for x in pg.sql("SELECT pid FROM pg_stat_activity WHERE backend_type='io worker'").stdout.splitlines()];self.assertEqual(len(workers),2)
            evidence.append({'cycle':cycle,'boot':current,'birth':birth,'PM':self.original_limits(pg.process.pid),'PMmaps':self.ranges(pg.process.pid,birth,True),'logger':self.original_limits(logger),'loggerMaps':self.ranges(logger,birth,False),'workers':[self.original_limits(x)for x in workers]})
        self.save('actual-three-as-r-recoveries',evidence)
    def test_PG18_S_BIRTH_004_extension_and_native_acl(self):
        pg=self.cluster();pg.sql('CREATE ROLE birth18_ordinary LOGIN')
        self.assertEqual(pg.sql("SELECT has_function_privilege('birth18_ordinary','test_dsg_birth_arrays18()','EXECUTE')").stdout.strip(),'f')
        pg.sql('GRANT EXECUTE ON FUNCTION test_dsg_birth_arrays18() TO birth18_ordinary')
        pg.sql(f"CREATE FUNCTION birth18_definer() RETURNS text AS '{core.LIB}','test_dsg_birth_arrays18' LANGUAGE C SECURITY DEFINER")
        for sql in ['SET ROLE birth18_ordinary;SELECT test_dsg_birth_arrays18()','SET SESSION AUTHORIZATION birth18_ordinary;SELECT birth18_definer()']:
            result=pg.sql(sql,check=False);self.assertNotEqual(result.returncode,0);self.assertIn('original administrator',result.stderr)
        self.assertTrue(pg.birth()['enabled'])
if __name__=='__main__':unittest.main(verbosity=2)
