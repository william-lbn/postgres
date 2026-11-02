from pathlib import Path
import json,subprocess,os,time,hashlib,socket,struct,sys
CFG = json.loads(Path(sys.argv[1]).read_text())
p=Path(CFG['run']);begin=time.monotonic();name=CFG['container'];result={'nonce':CFG['nonce'],'Source':False,'normal':False,'E':0,'test_only_shim_not_production_Neon':True};started=False
sha=lambda q:hashlib.sha256(Path(q).read_bytes()).hexdigest()
def save(n,v):(p/n).write_text(json.dumps(v,indent=2)+'\n')
def run(cmd,n,check=True,timeout=60):
 r=subprocess.run(cmd,capture_output=True,timeout=timeout)
 for ext,b in [('stdout',r.stdout),('stderr',r.stderr)]:
  q=p/(n+'.'+ext);q.write_bytes(b);os.chmod(q,0o600)
 if check and r.returncode!=0:raise RuntimeError(n+' failed exit '+str(r.returncode))
 return r
pg='/usr/local/pgsql';sock='/data/socket'
def sql(s,n,check=True):return run(['/usr/bin/docker','exec',name,pg+'/bin/psql','-X','-A','-t','-v','ON_ERROR_STOP=1','-v','VERBOSITY=verbose','-h',sock,'-p','55432','-U','postgres','-d','postgres','-c',s],n,check)
def physical_startup_denied():
 r=run(['/usr/bin/docker','exec',name,'/usr/bin/python3','-c',"import socket,struct,json\ns=socket.socket(socket.AF_UNIX);s.settimeout(10);s.connect('/data/socket/.s.PGSQL.55432')\nbody=struct.pack('!I',196608)+bytes([0]).join([b'user',b'boundary_rep',b'database',b'postgres',b'replication',b'true'])+bytes([0,0])\ns.sendall(struct.pack('!I',len(body)+4)+body)\ndef exact(n):\n b=b''\n while len(b)<n:\n  x=s.recv(n-len(b))\n  if not x:raise RuntimeError('startup response EOF')\n  b+=x\n return b\nfor _ in range(16):\n h=exact(5);n=struct.unpack('!I',h[1:])[0];assert 4<=n<=65536;b=exact(n-4)\n if h[:1]==b'E':\n  f={};i=0\n  while i<len(b) and b[i]:\n   k=chr(b[i]);j=b.index(0,i+1);f[k]=b[i+1:j].decode();i=j+1\n  assert f.get('C')=='42501' and f.get('R')=='DsgBufferCheckPhysicalReplication'\n  print(json.dumps({'SQLSTATE':f['C'],'routine':f['R'],'actual_ErrorResponse':True}));s.close();break\n assert h[:1]!=b'Z'\nelse:raise RuntimeError('startup error not received')\n"],'physical-startup-wire')
 v=json.loads(r.stdout);assert v['SQLSTATE']=='42501' and v['routine']=='DsgBufferCheckPhysicalReplication'
 save('physical-startup-typed.json',v)
try:
 assert sha(CFG['new_postgres'])==CFG['postgres_sha256']
 compilecmd=['/usr/bin/docker','run','--rm','--pull=never','--network=none','--read-only','--cap-drop=ALL','--security-opt=no-new-privileges','--user=1000:1000','--cpus=1','--memory=512m','--memory-swap=512m','--tmpfs','/tmp:rw,size=128m','-v',CFG['compiler_source']+':/src:ro','-v',CFG['compiler_objects']+':/build:ro','-v',str(p/'static_origin_probe.c')+':/input/probe.c:ro','-v',str(p/'lib')+':/out:rw','--entrypoint','/usr/bin/gcc',CFG['tools_image'],'-shared','-fPIC','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-I/build/src/include','-I/src/src/include','-o','/out/neon.so','/input/probe.c']
 assert sha(p/'lib/neon.so')==CFG['reused_test_shim_sha256']
 script='set -e\nmkdir -p /data/socket\n/usr/local/pgsql/bin/initdb -D /data/pgdata --no-data-checksums -U postgres --auth=trust >/data/initdb.private.log 2>&1\nulimit -Sv 2097152\nulimit -Hv 2097152\nexport NEON_DSGA_EARLY_ARRAYS_PROFILE=test-v1:2048\nexport NEON_DSGA_ADDRESS_SPACE_MAX_BYTES=2147483648\nexec /usr/local/pgsql/bin/postgres -D /data/pgdata -k /data/socket -p 55432 -c listen_addresses= -c shared_buffers=4MB -c max_connections=16 -c max_wal_senders=8 -c max_replication_slots=8 -c enable_dynamic_shared_buffers=on -c shared_preload_libraries=neon -c log_error_verbosity=verbose\n'
 cmd=['/usr/bin/docker','run','-d','--pull=never','--name',name,'--network=none','--read-only','--cap-drop=ALL','--security-opt=no-new-privileges','--user=1000:1000','--cpus=1.5','--memory=2g','--memory-swap=2g','--pids-limit=128','--tmpfs','/tmp:rw,size=128m','-e','DSG_STATIC_SLOT_TEST_OUT=/evidence','-v',CFG['PG_distribution']+':'+pg+':ro','-v',CFG['new_postgres']+':'+pg+'/bin/postgres:ro','-v',str(p/'lib/neon.so')+':'+pg+'/lib/postgresql/neon.so:ro','-v',str(p/'cluster')+':/data:rw','-v',str(p/'evidence')+':/evidence:rw','--entrypoint','/bin/bash',CFG['runtime_image'],'-c',script]
 run(cmd,'cluster-start');started=True;deadline=time.monotonic()+30
 while True:
  r=run(['/usr/bin/docker','exec',name,pg+'/bin/pg_isready','-h',sock,'-p','55432','-U','postgres'],'readiness',False,10)
  if r.returncode==0:break
  if time.monotonic()>deadline:raise RuntimeError('component cluster startup timeout')
  time.sleep(.15)
 assert sql('SHOW enable_dynamic_shared_buffers','actual-enabled').stdout.strip()==b'on'
 run(['/usr/bin/python3',str(p/'host_probe_driver.py'),name,pg+'/bin',sock,'postgres'],'static-dynamic-original-probe',True,60)
 probe=json.loads((p/'evidence/static-origin-result.json').read_text());assert probe['status']=='PASS_MECHANISM_COMPONENT_ONLY';result['static_dynamic_actual_probe']=probe
 sql('CREATE ROLE boundary_rep LOGIN REPLICATION','role-create');before=sql('SELECT slot_name,slot_type,active FROM pg_replication_slots ORDER BY slot_name','slots-before').stdout;assert b'wal_proposer_slot|physical|f' in before
 mem=run(['/usr/bin/docker','exec',name,'cat','/sys/fs/cgroup/memory.max'],'hardlimit-before').stdout;cases=[]
 for label,stmt in [('SQL_same_slot',"SELECT * FROM pg_create_physical_replication_slot('wal_proposer_slot')"),('SQL_other_slot',"SELECT * FROM pg_create_physical_replication_slot('untrusted_other_slot')"),('SQL_acquire',"SELECT pg_replication_slot_advance('wal_proposer_slot',pg_current_wal_lsn())")]:
  r=sql('SET SESSION AUTHORIZATION boundary_rep; '+stmt,label,False);assert r.returncode!=0 and b'42501' in r.stderr and b'DsgBufferCheckPhysicalReplication' in r.stderr;cases.append({'case':label,'SQLSTATE':'42501','actual_gate_source':True})
 for label,db,stmt in [('physical_startup','replication=true','IDENTIFY_SYSTEM'),('database_BASE_BACKUP','dbname=postgres replication=database','BASE_BACKUP'),('database_CREATE_PHYSICAL','dbname=postgres replication=database','CREATE_REPLICATION_SLOT untrusted_wire_slot PHYSICAL'),('database_START_PHYSICAL','dbname=postgres replication=database','START_REPLICATION SLOT wal_proposer_slot PHYSICAL 0/0')]:
  if label=='physical_startup':
   physical_startup_denied();cases.append({'case':label,'SQLSTATE':'42501','actual_gate_source':True});continue
  r=run(['/usr/bin/docker','exec',name,pg+'/bin/psql','-X','-A','-t','-v','ON_ERROR_STOP=1','-v','VERBOSITY=verbose','-h',sock,'-p','55432','-U','boundary_rep','-d',db,'-c',stmt],label,False);assert r.returncode!=0 and b'42501' in r.stderr and b'DsgBufferCheckPhysicalReplication' in r.stderr;cases.append({'case':label,'SQLSTATE':'42501','actual_gate_source':True})
 after=sql('SELECT slot_name,slot_type,active FROM pg_replication_slots ORDER BY slot_name','slots-after').stdout;memafter=run(['/usr/bin/docker','exec',name,'cat','/sys/fs/cgroup/memory.max'],'hardlimit-after').stdout;assert before==after and mem==memafter
 result.update({'PASS':True,'DSGA_enabled':True,'static_fixed_persistent_slot_create_acquire_before_fact':True,'external_denials':cases,'slot_state_unchanged':True,'hard_limit_unchanged':True,'test_DSO_sha256':sha(p/'lib/neon.so')})
except BaseException as e:result.update({'PASS':False,'failure_class':type(e).__name__,'error':str(e)})
finally:
 if started:
  run(['/usr/bin/docker','logs',name],'cluster-log',False)
  run(['/usr/bin/docker','exec',name,pg+'/bin/pg_ctl','-D','/data/pgdata','stop','-m','fast','-w'],'cluster-stop',False,30)
  term=run(['/usr/bin/docker','inspect','--format','{{json .State}}',name],'cluster-terminal',False)
  if term.returncode==0:
   state=json.loads(term.stdout);save('container-terminal.json',{k:state[k] for k in ['Running','OOMKilled','ExitCode','Status']})
  run(['/usr/bin/docker','wait',name],'cluster-unique-Wait',False,15)
  run(['/usr/bin/docker','rm',name],'cluster-remove',False)
 result['seconds']=time.monotonic()-begin;save('result.json',result)
raise SystemExit(0 if result.get('PASS') else 1)
