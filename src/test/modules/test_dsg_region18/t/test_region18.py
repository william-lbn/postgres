#!/usr/bin/env python3
# Independent PG18 leaf provider/ABI tests; no resize/Root/normal claims.
import json,os,pwd,signal,subprocess,tempfile,time,unittest
from pathlib import Path
BIN=Path(os.environ['NEON_TEST_PG_BIN'])
LIB=Path(os.environ['NEON_TEST_DSG_LIBRARY'])
USER=pwd.getpwuid(os.geteuid()).pw_name
def env():return {k:v for k,v in os.environ.items() if not k.startswith('NEON_DSGA_')}
class Cluster:
    def __init__(self,method='worker'):
        self.tmp=tempfile.TemporaryDirectory(prefix='pg18-r-',dir='/tmp')
        self.root=Path(self.tmp.name);self.data=self.root/'data';self.sock=self.root/'s';self.sock.mkdir()
        self.process=None;self.log=self.root/'pg.log'
        self.run('initdb','-D',self.data,'-A','trust','--no-locale','--no-sync')
        (self.data/'postgresql.conf').write_text(f"listen_addresses=''\nport=55438\nunix_socket_directories='{self.sock}'\nshared_buffers='4MB'\nmax_connections=20\nio_method='{method}'\nio_workers=2\nhuge_pages=off\n")
    def run(self,binary,*args,check=True):
        p=subprocess.run([str(BIN/binary),*map(str,args)],env=env(),capture_output=True,text=True,timeout=30)
        if check and p.returncode:raise RuntimeError(p.stderr[-5000:])
        return p
    def sql(self,text,check=True):return self.run('psql','-h',self.sock,'-p','55438','-U',USER,'-d','postgres','-XqAt','-v','ON_ERROR_STOP=1','-c',text,check=check)
    def batch(self,script):
        return subprocess.run([str(BIN/'psql'),'-h',str(self.sock),'-p','55438','-U',USER,'-d','postgres','-XqAt','-v','ON_ERROR_STOP=0'],input=script,env=env(),capture_output=True,text=True,timeout=30)
    def start(self):
        self.stream=self.log.open('w');self.process=subprocess.Popen([str(BIN/'postgres'),'-D',str(self.data)],env=env(),stdout=self.stream,stderr=subprocess.STDOUT)
        until=time.monotonic()+20
        while time.monotonic()<until:
            if self.process.poll() is not None:raise RuntimeError(self.log.read_text())
            if self.sql('SELECT 1',check=False).returncode==0:break
            time.sleep(.025)
        else:raise RuntimeError('PG18 startup timed out: '+self.log.read_text())
        self.sql('CREATE EXTENSION test_dsg_region18')
    def close(self):
        if self.process is not None and self.process.poll() is None:
            self.process.send_signal(signal.SIGINT)
            try:self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:self.process.kill();self.process.wait(timeout=10)
        if hasattr(self,'stream'):self.stream.close()
        if os.environ.get('NEON_PG18_EVIDENCE') and self.log.exists():
            target=Path(os.environ['NEON_PG18_EVIDENCE'])/(self.root.name+'.log');target.write_bytes(self.log.read_bytes())
        self.tmp.cleanup()
class Region(unittest.TestCase):
    def cluster(self,method='worker'):
        p=Cluster(method);self.addCleanup(p.close);p.start();return p
    def test_PG18_U_ABI_001_actual_native_sizes(self):
        p=self.cluster();fact=json.loads(p.sql('SELECT test_dsg_region18_abi()').stdout)
        self.assertEqual(fact['pg_version_num']//10000,18)
        self.assertEqual(fact['state_bytes'],'4');self.assertEqual(fact['aio_wait_ref_bytes'],'12')
        self.assertLess(int(fact['io_wref_offset']),int(fact['descriptor_bytes']))
        self.assertLessEqual(int(fact['descriptor_bytes']),int(fact['descriptor_padded_bytes']))
        self.assertTrue(fact['cleared_wref_invalid']);self.assertTrue(fact['io_uring_compiled'])
        self.assertFalse(fact['production_resize_hooked'])
        if os.environ.get('NEON_PG18_EVIDENCE'):Path(os.environ['NEON_PG18_EVIDENCE'],'actual-abi.json').write_text(json.dumps(fact,indent=2)+'\n')
    def test_PG18_U_REGION_001_invalid_geometry_cleanup(self):
        p=self.cluster()
        for action in ('invalid-zero','invalid-offset','invalid-overflow','unknown'):
            with self.subTest(action=action):
                # 同一真实backend跨ERROR继续分配/共享，不让换连接掩盖清理错误。
                result=p.batch("SELECT pg_backend_pid();\nSELECT test_dsg_region18_run('"+action+"');\nSELECT pg_backend_pid();\nSELECT test_dsg_region18_run('share-fork');\n")
                self.assertEqual(result.returncode,0)
                self.assertIn('ERROR:',result.stderr)
                lines=result.stdout.strip().splitlines()
                self.assertEqual(len(lines),3,result.stdout)
                self.assertEqual(lines[0],lines[1])
                self.assertTrue(json.loads(lines[2])['ok'])
    def test_PG18_S_REGION_001_true_shared_fork(self):
        p=self.cluster();self.assertEqual(json.loads(p.sql("SELECT test_dsg_region18_run('share-fork')").stdout),{'ok':True,'normal_permit':False})
    def test_PG18_S_REGION_002_boundary_and_padding(self):
        p=self.cluster();self.assertTrue(json.loads(p.sql("SELECT test_dsg_region18_run('release-boundary')").stdout)['ok'])
    def test_PG18_S_REGION_003_park_failure_rearm_three_cycles(self):
        p=self.cluster();self.assertTrue(json.loads(p.sql("SELECT test_dsg_region18_run('park-rearm')").stdout)['ok'])
    def test_PG18_S_REGION_004_original_session_acl(self):
        p=self.cluster();p.sql('CREATE ROLE region18_ordinary LOGIN')
        for fn in ('test_dsg_region18_abi()','test_dsg_region18_run(text)'):
            self.assertEqual(p.sql("SELECT has_function_privilege('region18_ordinary','"+fn+"','EXECUTE')").stdout.strip(),'f')
        result=p.sql('SET ROLE region18_ordinary;SELECT test_dsg_region18_abi()',check=False)
        self.assertNotEqual(result.returncode,0);self.assertIn('permission denied',result.stderr)
        p.sql('GRANT EXECUTE ON FUNCTION test_dsg_region18_abi(),test_dsg_region18_run(text) TO region18_ordinary')
        p.sql(f"CREATE FUNCTION region18_definer() RETURNS text AS '{LIB}','test_dsg_region18_abi' LANGUAGE C SECURITY DEFINER")
        for sql in ('SET ROLE region18_ordinary;SELECT test_dsg_region18_abi()',"SET ROLE region18_ordinary;SELECT test_dsg_region18_run('share-fork')",'SET SESSION AUTHORIZATION region18_ordinary;SELECT region18_definer()'):
            result=p.sql(sql,check=False);self.assertNotEqual(result.returncode,0);self.assertIn('original administrator',result.stderr)
    def test_PG18_S_AIO_001_stock_sync_and_worker_data(self):
        for method in ('sync','worker'):
            with self.subTest(method=method):
                p=self.cluster(method)
                self.assertEqual(p.sql('SHOW io_method').stdout.strip(),method)
                p.sql('CREATE TABLE region18_data AS SELECT i,repeat(md5(i::text),2) v FROM generate_series(1,50000) i;CHECKPOINT')
                self.assertEqual(p.sql('SELECT count(*),sum(i),min(length(v)),max(length(v)) FROM region18_data').stdout.strip(),'50000|1250025000|64|64')
                if method=='worker':
                    count=int(p.sql("SELECT count(*) FROM pg_stat_activity WHERE lower(backend_type)='io worker'").stdout.strip())
                    self.assertEqual(count,2)
if __name__=='__main__':unittest.main(verbosity=2)
