#!/usr/bin/env python3
# PG18 TEST_ONLY 四数组ABI1原生组件；无原A/R/NativeFinal、Host预算或normal/VM许可。
# RootFixture从PG17固定Git b95b142190cdc51de0d5603bfb9c46a0e0e7051f提取；
# 不从Mac业务源或PG17活动工作区取代码。所有PG/psql均真实uid1000。
import ctypes,hashlib,json,mmap,os,pwd,signal,socket,stat,struct,subprocess,tempfile,threading,time,unittest
from pathlib import Path
BIN=Path(os.environ['NEON_TEST_PG_BIN'])
LIB=Path(os.environ['NEON_TEST_DSG_LIBRARY'])
ACCOUNT=pwd.getpwuid(1000)
SO_PEERPIDFD=77
ROOT_PATH=Path('/run/neon-dsga')
SOCKET_PATH=ROOT_PATH/'array-populate.sock'
libc=ctypes.CDLL(None,use_errno=True)
def rounded(n):return (n+4095)//4096*4096
def u64(raw,n):return struct.unpack_from('!Q',raw,n)[0]
def environment():return {k:v for k,v in os.environ.items() if not k.startswith('NEON_DSGA_')}
def credentials():return {'user':ACCOUNT.pw_uid,'group':ACCOUNT.pw_gid,'extra_groups':[]}
def send_pidfd(pid,sig):
    fd=os.pidfd_open(pid,0)
    try:signal.pidfd_send_signal(fd,sig)
    finally:os.close(fd)

class RootFixture:
    """仅测试Root responder；读取真实peer/四inode，不产生产Root授权来源。"""
    def __init__(self, fault=None):
        ROOT_PATH.mkdir(mode=0o755,exist_ok=True);os.chmod(ROOT_PATH,0o755)
        if SOCKET_PATH.exists():raise RuntimeError('fresh container fixture socket already exists')
        self.s=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET|socket.SOCK_CLOEXEC)
        self.s.bind(str(SOCKET_PATH));os.chmod(SOCKET_PATH,0o666);self.s.listen(8);self.s.settimeout(.1)
        self.stop=threading.Event();self.events=[];self.failures=[];self.generation=None;self.fds=[];self.maps=[]
        self.regnonce=bytes.fromhex('44'*32);self.fault=fault;self.fault_used=False
        self.worker=threading.Thread(target=self.run);self.worker.start()
    def run(self):
        while not self.stop.is_set():
            try:conn,_=self.s.accept()
            except TimeoutError:continue
            except OSError:
                if self.stop.is_set():return
                raise
            try:
                with conn:self.handle(conn)
            except BaseException as e:self.failures.append(repr(e))
    def handle(self,conn):
        raw=conn.recv(513)
        if len(raw)!=512 or raw[:8]!=b'NDSGARR1':raise RuntimeError('wrong exact array request')
        if struct.unpack_from('!I',raw,8)[0]!=1 or any(raw[480:]):raise RuntimeError('noncanonical request')
        pid,uid,gid=struct.unpack('3i',conn.getsockopt(socket.SOL_SOCKET,socket.SO_PEERCRED,12))
        if uid!=ACCOUNT.pw_uid:raise RuntimeError('unexpected PG peer uid')
        # 内核原socket peer pidfd，不以readlink文本或客户端传FD代替。
        peerfd=conn.getsockopt(socket.SOL_SOCKET,SO_PEERPIDFD)
        try:
            os.set_inheritable(peerfd,False)
            if raw[112:144]!=b'\x11'*32 or raw[144:176]!=b'\x22'*32 or raw[176:208]!=b'\x33'*32:raise RuntimeError('test profile source differs')
            op=struct.unpack_from('!I',raw,12)[0];gen=u64(raw,104);boot=u64(raw,16)
            ranges=[tuple(u64(raw,272+i*40+j*8) for j in range(5)) for i in range(4)]
            event={'op':op,'pid':pid,'uid':uid,'boot':str(boot),'mapping_generation':gen,'array':u64(raw,80),'offset':u64(raw,88),'bytes':u64(raw,96),'high':u64(raw,472),'target':u64(raw,464)}
            if op==1:
                if self.generation is not None and (gen<=self.generation or not any(e['op']==3 and e['mapping_generation']==self.generation for e in self.events)):raise RuntimeError('new mapping without old retire begin')
                opened=[];maps=[];identities=[]
                try:
                    rows=Path(f'/proc/{pid}/maps').read_text().splitlines()
                    for base,logical,span,element,page in ranges:
                        if page!=4096 or logical!=u64(raw,32)*element or span!=rounded(logical):raise RuntimeError('wrong compiled geometry')
                        name=f'{base:x}-{base+span:x}'
                        row=next(x.split() for x in rows if x.split()[0]==name)
                        if row[1]!='rw-s' or int(row[2],16)!=0 or int(row[4])==0:raise RuntimeError('not exact shared anonymous shmem VMA')
                        fd=os.open(f'/proc/{pid}/map_files/{name}',os.O_RDWR|os.O_CLOEXEC)
                        st=os.fstat(fd)
                        if not stat.S_ISREG(st.st_mode) or st.st_ino!=int(row[4]) or st.st_size!=span:raise RuntimeError('actual backing inode/span differs')
                        dev=[int(v,16) for v in row[3].split(':')]
                        if (os.major(st.st_dev),os.minor(st.st_dev))!=tuple(dev):raise RuntimeError('actual backing dev differs')
                        fs=ctypes.create_string_buffer(256)
                        if libc.fstatfs(fd,fs)!=0 or ctypes.c_long.from_buffer(fs).value!=0x01021994:raise RuntimeError('not kernel shmem backing')
                        opened.append(fd);maps.append(mmap.mmap(fd,span,flags=mmap.MAP_SHARED,prot=mmap.PROT_READ|mmap.PROT_WRITE));identities.append([st.st_dev,st.st_ino,st.st_size])
                    if self.fds:
                        old={(os.fstat(f).st_dev,os.fstat(f).st_ino) for f in self.fds}
                        if old & {tuple(x[:2]) for x in identities}:raise RuntimeError('Rearm reused old backing inode')
                    # 仅见完整新四inode才退旧Root FD；retire_begin本身不清旧账。
                    for m in self.maps:m.close()
                    for f in self.fds:os.close(f)
                    self.fds,self.maps,self.ranges=opened,maps,ranges;opened=[];maps=[];self.generation=gen;self.boot=boot
                    event['actual_backing']=identities
                finally:
                    for m in maps:m.close()
                    for f in opened:os.close(f)
            else:
                if gen!=self.generation or boot!=self.boot or raw[432:464]!=self.regnonce or ranges!=self.ranges:raise RuntimeError('old generation/FD ticket changed')
                # backend也须精确映射同四inode；普通peer不能替换任意shmem。
                for (base,logical,span,element,page),fd in zip(ranges,self.fds):
                    f=os.open(f'/proc/{pid}/map_files/{base:x}-{base+span:x}',os.O_RDWR|os.O_CLOEXEC)
                    try:
                        a,b=os.fstat(f),os.fstat(fd)
                        if (a.st_dev,a.st_ino,a.st_size)!=(b.st_dev,b.st_ino,b.st_size):raise RuntimeError('request peer has different backing')
                    finally:os.close(f)
                if op==2:
                    i,off,n=u64(raw,80),u64(raw,88),u64(raw,96)
                    if i>=4 or off%4096 or n==0 or n%4096 or off+n>ranges[i][2]:raise RuntimeError('populate outside original geometry')
                    # 真VMA MADV_POPULATE_WRITE，不写业务字节，不用RSS判断预触结果。
                    address=ctypes.addressof(ctypes.c_char.from_buffer(self.maps[i]))+off
                    if libc.madvise(ctypes.c_void_p(address),ctypes.c_size_t(n),23)!=0:raise OSError(ctypes.get_errno(),'Root fixture MADV_POPULATE_WRITE')
                    event['actual_populated']=True
                elif op!=3:raise RuntimeError('unknown operation')
            self.events.append(event)
            if self.fault and not self.fault_used and (self.fault=='startup_drop' and op==2 and u64(raw,464)==0 or self.fault=='grow_drop' and op==2 and u64(raw,464)>0):
                self.fault_used=True;event['dropped_reply']=True;return
            reply=bytearray(160);reply[:8]=b'NDSGARA1';struct.pack_into('!IIIIQQ',reply,8,1,0,0,0,u64(raw,88),u64(raw,96));reply[40:72]=hashlib.sha256(raw).digest();reply[72:104]=self.regnonce;struct.pack_into('!QQ',reply,104,1,gen)
            if self.fault=='bad_digest' and not self.fault_used:self.fault_used=True;reply[40]^=1
            if conn.send(reply)!=160:raise RuntimeError('short fixture reply')
        finally:os.close(peerfd)
    def assert_clean(self):
        if self.failures:raise AssertionError(self.failures)
    def close(self):
        self.stop.set();self.s.close();self.worker.join(timeout=5)
        if self.worker.is_alive():raise RuntimeError("test Root worker did not reach terminal state")
        for m in self.maps:m.close()
        for f in self.fds:os.close(f)
        SOCKET_PATH.unlink(missing_ok=True)

class Cluster:
    def __init__(self):
        self.temp=tempfile.TemporaryDirectory(prefix='pg18-array-root-',dir='/tmp')
        self.root=Path(self.temp.name);os.chown(self.root,ACCOUNT.pw_uid,ACCOUNT.pw_gid)
        self.data=self.root/'data';self.sock=self.root/'s';self.sock.mkdir();os.chown(self.sock,ACCOUNT.pw_uid,ACCOUNT.pw_gid)
        self.log=self.root/'postmaster.log';self.process=None
        self.run('initdb','-D',self.data,'-A','trust','--no-locale','--no-sync')
        config=self.data/'postgresql.conf'
        config.write_text(f"listen_addresses=''\nport=55438\nunix_socket_directories='{self.sock}'\nshared_buffers='4MB'\nmax_connections=20\nio_method='worker'\nio_workers=2\nhuge_pages=off\nlogging_collector=on\nlog_directory='logs'\nlog_filename='pg.log'\n")
        os.chown(config,ACCOUNT.pw_uid,ACCOUNT.pw_gid)
    def run(self,binary,*args,check=True):
        result=subprocess.run([str(BIN/binary),*map(str,args)],env=environment(),capture_output=True,text=True,timeout=30,**credentials())
        if check and result.returncode:raise RuntimeError(result.stderr[-8000:])
        return result
    def sql(self,text,check=True):
        return self.run('psql','-h',self.sock,'-p','55438','-U',ACCOUNT.pw_name,'-d','postgres','-XqAt','-v','ON_ERROR_STOP=1','-c',text,check=check)
    def start(self,check=True,profile='test-v1',arrays='test-v1:2048'):
        env=environment()
        if arrays is not None:env['NEON_DSGA_PG18_NATIVE_ARRAYS']=arrays
        if profile is not None:env['NEON_DSGA_ARRAY_ROOT_PROFILE']=profile
        self.stream=self.log.open('a')
        self.process=subprocess.Popen([str(BIN/'postgres'),'-D',str(self.data)],env=env,stdout=self.stream,stderr=subprocess.STDOUT,**credentials())
        until=time.monotonic()+15
        while time.monotonic()<until:
            if self.process.poll() is not None:
                if check:raise RuntimeError('real PG18 exit='+str(self.process.returncode)+' '+self.logs()[-9000:])
                return False
            if self.sql('SELECT 1',check=False).returncode==0:
                self.sql('CREATE EXTENSION test_dsg_region18;SELECT test_dsg_resize18_prepare()')
                return True
            time.sleep(.025)
        raise RuntimeError('PG18 Root startup original15s deadline '+self.logs()[-9000:])
    def state(self):return json.loads(self.sql('SELECT test_dsg_resize18_state()').stdout)
    def resize(self,kind,target,revision,generation=None):
        if generation is None:generation=int(self.state()['generation'])
        return json.loads(self.sql(f"SELECT test_dsg_resize18_request('{kind}',{target},{revision},{generation},5000)").stdout)
    def asynchronous(self,text):
        return subprocess.Popen([str(BIN/'psql'),'-h',str(self.sock),'-p','55438','-U',ACCOUNT.pw_name,'-d','postgres','-XqAt','-v','ON_ERROR_STOP=1','-c',text],env=environment(),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,**credentials())
    def logger(self,old=None):
        until=time.monotonic()+8
        while time.monotonic()<until:
            for raw in Path(f'/proc/{self.process.pid}/task/{self.process.pid}/children').read_text().split():
                try:cmd=Path(f'/proc/{raw}/cmdline').read_bytes()
                except FileNotFoundError:continue
                if b'logger' in cmd and int(raw)!=old:return int(raw)
            time.sleep(.025)
        raise RuntimeError('real PG18 logger did not start/replace')
    def logs(self):
        result=self.log.read_text() if self.log.exists() else ''
        if self.data.exists():
            for p in sorted((self.data/'logs').glob('*')):
                if p.is_file():result+='\n'+p.read_text()
        return result
    def close(self):
        if self.process is not None and self.process.poll() is None:
            self.process.send_signal(signal.SIGINT)
            try:self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:self.process.kill();self.process.wait(timeout=10)
        if hasattr(self,'stream'):self.stream.close()
        self.temp.cleanup()

class ArrayRootNative18(unittest.TestCase):
    def fixture(self,fault=None):
        root=RootFixture(fault);self.addCleanup(root.close)
        pg=Cluster();self.addCleanup(pg.close);return root,pg
    def committed(self,result,high):
        self.assertEqual(result['result_phase'],2,result)
        self.assertEqual((result['low'],result['high']),(high,high))
        self.assertFalse(result['normal_permit'])
    def test_PG18_S_ARRAY_ROOT_001_initial_grow_shrink_regrow_real_backing(self):
        root,pg=self.fixture();pg.start();root.assert_clean()
        self.assertEqual([e['op'] for e in root.events],[1,2,2,2,2])
        self.assertEqual(len(root.events[0]['actual_backing']),4)
        pg.sql("CREATE TABLE payload18 AS SELECT i,md5(i::text) v FROM generate_series(1,5000) i;CHECKPOINT")
        checksum=pg.sql("SELECT count(*),sum(i),md5(string_agg(v,',' ORDER BY i)) FROM payload18").stdout
        for kind,target,rev in [('grow',2048,1),('shrink',512,2),('grow',1536,3)]:
            self.committed(pg.resize(kind,target,rev),target)
        self.assertEqual(pg.sql("SELECT count(*),sum(i),md5(string_agg(v,',' ORDER BY i)) FROM payload18").stdout,checksum)
        self.assertEqual(int(pg.sql("SELECT count(*) FROM pg_stat_activity WHERE backend_type='io worker'").stdout),2)
        self.assertTrue(any(e['high']==512 and e['target']==1536 for e in root.events));root.assert_clean()
    def test_PG18_S_ARRAY_ROOT_002_bad_or_lost_startup_reply_fails_closed(self):
        for fault in ('bad_digest','startup_drop'):
            with self.subTest(fault=fault):
                root=RootFixture(fault);pg=Cluster()
                try:
                    self.assertFalse(pg.start(check=False));self.assertNotEqual(pg.process.returncode,0)
                    self.assertIn('Root',pg.logs());self.assertTrue(root.fault_used);root.assert_clean()
                finally:pg.close();root.close()
        # 没有18原A/R及NativeFinal时，root-v1不能借test-v1来源启用。
        for profile,arrays in [('root-v1','test-v1:2048'),('test-v1',None)]:
            with self.subTest(profile=profile,arrays=arrays):
                root=RootFixture();pg=Cluster()
                try:
                    self.assertFalse(pg.start(check=False,profile=profile,arrays=arrays))
                    self.assertNotEqual(pg.process.returncode,0);self.assertIn('PG18 array Root',pg.logs())
                    self.assertEqual(root.events,[]);root.assert_clean()
                finally:pg.close();root.close()
    def test_PG18_S_ARRAY_ROOT_003_lost_grow_reply_retains_unknown_cost(self):
        root,pg=self.fixture('grow_drop');pg.start();before=pg.state()
        fact=pg.resize('grow',2048,1)
        self.assertEqual(fact['result_phase'],3,fact)
        self.assertEqual((fact['high'],fact['generation']),(before['high'],before['generation']))
        self.assertTrue(any(a['unknown'] for a in fact['arrays']),fact)
        self.assertTrue(root.fault_used)
        self.assertTrue(any(e.get('actual_populated') and e.get('dropped_reply') for e in root.events));root.assert_clean()
    def test_PG18_S_ARRAY_ROOT_004_logger_and_reinit_keep_old_backing_until_new_registration(self):
        root,pg=self.fixture();pg.start();old=pg.state()['boot'];logger=pg.logger()
        pg.sql('CREATE TABLE recovery18 AS SELECT i FROM generate_series(1,100) i;CHECKPOINT')
        send_pidfd(logger,signal.SIGKILL);pg.logger(old=logger)
        self.assertFalse(any(e['op']==3 for e in root.events))
        for cycle in range(3):
            holder=pg.asynchronous('SELECT pg_backend_pid(),pg_sleep(30)')
            try:
                until=time.monotonic()+3;victim=None
                while time.monotonic()<until:
                    text=pg.sql("SELECT pid FROM pg_stat_activity WHERE query LIKE 'SELECT pg_backend_pid(),pg_sleep(30)%'").stdout.strip()
                    if text:victim=int(text);break
                    time.sleep(.025)
                self.assertIsNotNone(victim);send_pidfd(victim,signal.SIGKILL)
                holder.communicate(timeout=10);self.assertNotEqual(holder.returncode,0)
            finally:
                if holder.poll() is None:holder.kill();holder.communicate(timeout=10)
            until=time.monotonic()+15;new=None
            while time.monotonic()<until:
                try:
                    new=pg.state()['boot']
                    if new!=old:break
                except (OSError,RuntimeError):pass
                time.sleep(.025)
            self.assertIsNotNone(new,pg.logs()[-8000:]);self.assertNotEqual(new,old);old=new
            self.assertEqual(pg.sql('SELECT count(*),sum(i) FROM recovery18').stdout.strip(),'100|5050')
            self.assertEqual(int(pg.sql("SELECT count(*) FROM pg_stat_activity WHERE backend_type='io worker'").stdout),2)
        self.assertEqual(len([e for e in root.events if e['op']==1]),4)
        self.assertGreaterEqual(len([e for e in root.events if e['op']==3]),3);root.assert_clean()
    def tearDown(self):
        target=os.environ.get('NEON_ARRAY_ROOT_EVIDENCE')
        if target:
            p=Path(target)/self._testMethodName;p.mkdir(parents=True,exist_ok=True)
            for f in self._cleanups:
                obj=getattr(f[0],'__self__',None)
                if isinstance(obj,RootFixture):(p/'root-events.json').write_text(json.dumps({'events':obj.events,'failures':obj.failures},indent=2))
                if isinstance(obj,Cluster):(p/'postgres.log').write_text(obj.logs())
if __name__=='__main__':unittest.main(verbosity=2)
