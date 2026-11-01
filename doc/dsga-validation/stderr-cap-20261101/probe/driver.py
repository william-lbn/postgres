#!/usr/bin/env python3
"""独立PG17实际header/ELF pipe probe；不改PG17源码，不代表Source。"""
import ctypes, errno, hashlib, json, os
from pathlib import Path
import select, signal, struct, subprocess, tempfile, time, unittest
from pipe_kernel_programs import run_kernel_worker

BIN=Path('/usr/local/pgsql/bin')
OUT=Path('/evidence')
LIB='/probe/pipe_probe17.so'
libc=ctypes.CDLL(None,use_errno=True)

def environment(filtered=False):
    result={k:v for k,v in os.environ.items()if not k.startswith('NEON_DSGA_')and not k.startswith('LD_')}
    # 仅组件工具运行；最终Source的pre-main闭包不能由此测试环境替代。
    result['LD_LIBRARY_PATH']='/usr/local/pgsql/lib'
    if filtered:
        result.update({'NEON_DSGA_EARLY_ARRAYS_PROFILE':'test-v1:8192','NEON_DSGA_ADDRESS_SPACE_MAX_BYTES':str(2<<30),'NEON_DSGA_EARLY_ARRAYS_FILTER':'test-v1','NEON_TEST_PIPE_PM':'1'})
    return result

def unprivileged():
    os.setgroups([]);os.setgid(1000);os.setuid(1000)
    if libc.prctl(38,1,0,0,0)!=0:raise OSError(ctypes.get_errno(),'NNP')

def decode(raw):
    assert raw[:8]==b'NDSGFP01'and struct.unpack_from('>IIII',raw,8)==(1,0xc000003e,3,0)
    at=144;out=[]
    for kind in (1,2,3):
        got,count=struct.unpack_from('>II',raw,at);assert got==kind and 0<count<=1024;at+=8
        program=raw[at:at+count*8];assert len(program)==count*8;out.append(program);at+=count*8
    assert at==len(raw);return out

class Cluster:
    def __init__(self,filtered):
        self.filtered=filtered;self.tmp=tempfile.TemporaryDirectory(prefix='pg17-pipe-',dir='/tmp');self.root=Path(self.tmp.name);os.chown(self.root,1000,1000)
        self.data=self.root/'data';self.sock=self.root/'s';self.sock.mkdir();os.chown(self.sock,1000,1000);self.log=self.root/'postmaster.log';self.process=None
        p=subprocess.run([str(BIN/'initdb'),'-D',str(self.data),'-A','trust','--no-locale','--no-sync'],env=environment(),preexec_fn=unprivileged,capture_output=True,text=True,timeout=30)
        assert p.returncode==0,p.stderr
        config=self.data/'postgresql.conf';config.write_text("listen_addresses=''\nport=55439\nunix_socket_directories='"+str(self.sock)+"'\nshared_buffers='4MB'\nmax_connections=20\nhuge_pages=off\nlogging_collector=on\nlog_directory='logs'\nlog_filename='pipe.log'\nshared_preload_libraries='"+LIB+"'\n");os.chown(config,1000,1000)
        if filtered:
            with config.open('a')as f:f.write("enable_dynamic_shared_buffers=on\nmax_shared_buffers='64MB'\n")
    def sql(self,sql,check=True):
        p=subprocess.run([str(BIN/'psql'),'-XqAt','-h',str(self.sock),'-p','55439','-U','nonroot','-d','postgres','-v','ON_ERROR_STOP=1','-c',sql],env=environment(),capture_output=True,text=True,timeout=20)
        if check:assert p.returncode==0,p.stderr
        return p
    def logs(self):
        text=self.log.read_text(errors='replace')if self.log.exists()else''
        for p in sorted((self.data/'logs').glob('*')):
            if p.is_file():text+='\n'+p.read_text(errors='replace')
        return text
    def start(self):
        self.stream=self.log.open('w');self.process=subprocess.Popen([str(BIN/'postgres'),'-D',str(self.data)],env=environment(self.filtered),preexec_fn=unprivileged,stdout=self.stream,stderr=subprocess.STDOUT)
        until=time.monotonic()+20
        while time.monotonic()<until:
            assert self.process.poll()is None,self.logs()
            if self.sql('SELECT 1',False).returncode==0:
                self.sql("CREATE FUNCTION test_pg17_pipe() RETURNS text AS '"+LIB+"','test_pg17_pipe' LANGUAGE C;REVOKE ALL ON FUNCTION test_pg17_pipe() FROM PUBLIC")
                return
            time.sleep(.03)
        raise AssertionError('PG17 actual startup deadline '+self.logs())
    def close(self):
        if self.process is not None and self.process.poll()is None:
            pidfd=os.pidfd_open(self.process.pid,0)
            try:
                signal.pidfd_send_signal(pidfd,signal.SIGINT)
                try:self.process.wait(timeout=10)
                except subprocess.TimeoutExpired:signal.pidfd_send_signal(pidfd,signal.SIGKILL);self.process.wait(timeout=10)
            finally:os.close(pidfd)
        if hasattr(self,'stream'):self.stream.close()
        if self.log.exists():(OUT/(self.root.name+'.log')).write_text(self.logs())
        self.tmp.cleanup()

class Pipe17(unittest.TestCase):
    def test_PG17_S_PIPE_001_actual_three_kernel_programs(self):
        args=[str(2<<30),'1',str(0x10000000),'4096',str(0x20000000),'8192',str(0x30000000),'4096',str(0x40000000),'4096']
        p=subprocess.run([str(BIN/'postgres'),'--neon-dsga-audit-filter-v1',*args],env=environment(),capture_output=True,timeout=10)
        self.assertEqual(p.returncode,0,p.stderr);programs=decode(p.stdout);evidence=[]
        for name,code in zip(('PMbase','PMfinal','child'),programs):
            with self.subTest(stage=name):
                fact=run_kernel_worker(code);fact['stage']=name;fact['PG_version']=17;evidence.append(fact)
        (OUT/'actual-pg17-three-kernel-programs.json').write_text(json.dumps(evidence,indent=2)+'\n')
    def good_fcntl(self,pipe):
        self.assertGreater(pipe['pipe_capacity_before'],0,pipe)
        self.assertTrue(all(pipe[k]for k in ('nonblock','cloexec','roundtrip','closed')),pipe)
        self.assertEqual((pipe['setfl'],pipe['setfd'],pipe['dup_errno']),(0,0,0),pipe)
        self.assertEqual(pipe['bad_get_errno'],errno.EBADF,pipe)
    def test_PG17_S_PIPE_002_real_PM_backend_and_default_off(self):
        evidence=[]
        for enabled in (False,True):
            with self.subTest(filtered=enabled):
                pg=Cluster(enabled);self.addCleanup(pg.close);pg.start()
                self.assertEqual(int(pg.sql('SHOW server_version_num').stdout)//10000,17)
                result=json.loads(pg.sql('SELECT test_pg17_pipe()').stdout);self.assertEqual(result['pg_version_num']//10000,17)
                pipe=result['pipe'];self.good_fcntl(pipe);self.assertGreater(pipe['stderr_capacity_before'],0,pipe)
                if enabled:
                    self.assertTrue(all(result[k]for k in ('enabled','postmaster_installed','finalized','child_installed')),result)
                    self.assertEqual((pipe['stderr_same_result'],pipe['stderr_same_errno']),(-1,errno.EPERM),pipe)
                    self.assertEqual(pipe['results'],[-1]*3,pipe);self.assertEqual(pipe['errors'],[errno.EPERM]*3,pipe);self.assertEqual(pipe['pipe_capacity_after'],pipe['pipe_capacity_before'],pipe)
                    marker='PG17_PIPE_PM ';pm=[json.loads(l.split(marker,1)[1])for l in pg.logs().splitlines()if marker in l]
                    self.assertEqual(len(pm),1,pg.logs());self.assertTrue(pm[0]['finalized']);self.assertFalse(pm[0]['child_installed']);self.good_fcntl(pm[0]['pipe']);self.assertEqual(pm[0]['pipe']['errors'],[errno.EPERM]*3,pm)
                    result['actual_PMfinal']=pm[0]
                else:
                    self.assertFalse(result['enabled']);self.assertEqual((pipe['stderr_same_result'],pipe['stderr_same_errno']),(pipe['stderr_capacity_before'],0),pipe)
                    self.assertEqual(pipe['results'][0],pipe['pipe_capacity_before']);self.assertGreater(pipe['results'][1],0);self.assertEqual(pipe['errors'][:2],[0,0])
                pg.sql('CREATE ROLE pipe17_ordinary LOGIN;GRANT EXECUTE ON FUNCTION test_pg17_pipe() TO pipe17_ordinary')
                refused=pg.sql('SET ROLE pipe17_ordinary;SELECT test_pg17_pipe()',False);self.assertNotEqual(refused.returncode,0);self.assertIn('pipe probe requires original administrator',refused.stderr)
                pg.sql('CREATE TABLE pipe17_data AS SELECT i,md5(i::text) FROM generate_series(1,1000)i;CHECKPOINT');self.assertEqual(pg.sql('SELECT count(*),sum(i) FROM pipe17_data').stdout.strip(),'1000|500500')
                result['filtered']=enabled;result['Source']=result['normal']=False;result['logs']=pg.logs();pg.close();self.assertEqual(pg.process.returncode,0);result['actual_unique_PM_Wait_exit']=pg.process.returncode;evidence.append(result)
        (OUT/'actual-pg17-PM-backend-pipe.json').write_text(json.dumps(evidence,indent=2)+'\n')

if __name__=='__main__':
    class ExactResult(unittest.TextTestResult):
        def __init__(self,*a,**k):super().__init__(*a,**k);self.passed=[];self.subtests=[]
        def addSuccess(self,t):self.passed.append(t.id());super().addSuccess(t)
        def addSubTest(self,t,s,e):self.subtests.append({'id':str(s),'passed':e is None});super().addSubTest(t,s,e)
    result=unittest.TextTestRunner(verbosity=2,resultclass=ExactResult).run(unittest.defaultTestLoader.loadTestsFromTestCase(Pipe17))
    (OUT/'test-result.json').write_text(json.dumps({'testsRun':result.testsRun,'passed':result.passed,'subtests':result.subtests,'failures':[{'id':str(t),'traceback':e}for t,e in result.failures],'errors':[{'id':str(t),'traceback':e}for t,e in result.errors],'skips':result.skipped,'Source':False,'normal':False,'E':0},indent=2)+'\n')
    raise SystemExit(0 if result.wasSuccessful()and result.testsRun==2 and len(result.passed)==2 and not result.skipped else 1)
