from pathlib import Path
import subprocess,json,time,socket,re,hashlib,traceback
v=Path(__file__).parent;cmd=json.loads((v/'qemu-command.json').read_text());name=cmd[cmd.index('--name')+1];start=time.monotonic();errors=[];rc=125;kvm=None;qmpfacts=[]
def qmp():
 global kvm
 until=time.monotonic()+15
 while time.monotonic()<until:
  try:
   s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);s.settimeout(2);s.connect(str(v/'qmp.sock'));break
  except OSError:
   s.close();time.sleep(.05)
 else:raise RuntimeError('QMP deadline')
 with s:
  f=s.makefile('rwb',buffering=0)
  def recv():
   while True:
    line=f.readline()
    if not line:raise RuntimeError('QMP EOF')
    obj=json.loads(line);qmpfacts.append(obj)
    if 'event'not in obj:return obj
  greeting=recv()
  assert 'QMP'in greeting
  f.write(b'{"execute":"qmp_capabilities","id":"caps"}\r\n')
  assert 'return'in recv()
  f.write(b'{"execute":"query-kvm","id":"kvm"}\r\n')
  answer=recv();kvm=answer.get('return');assert kvm=={'enabled':True,'present':True},answer
try:
 with(v/'console.log').open('wb')as out:
  p=subprocess.Popen(cmd,stdout=out,stderr=subprocess.STDOUT)
  try:qmp()
  except BaseException as e:errors.append('qmp:'+repr(e))
  try:rc=p.wait(timeout=300)
  except subprocess.TimeoutExpired:
   rc=124;errors.append('runtime timeout300s');subprocess.run(['docker','stop','-t','5',name],capture_output=True);p.wait(timeout=15)
finally:
 x=subprocess.run(['docker','inspect',name],capture_output=True,text=True)
 if x.returncode==0:
  o=json.loads(x.stdout)[0];(v/'qemu-container.json').write_text(json.dumps({'Image':o['Image'],'State':o['State'],'HostConfig':{k:o['HostConfig'].get(k)for k in ['Memory','MemorySwap','NanoCpus','NetworkMode','ReadonlyRootfs','CapAdd','CapDrop','SecurityOpt','Devices','PidsLimit','Tmpfs']}},indent=2)+'\n')
  if o['State'].get('OOMKilled'):errors.append('outer cgroup OOMKilled')
 subprocess.run(['docker','rm','-f',name],capture_output=True)
(v/'qmp-transcript.json').write_text(json.dumps(qmpfacts,indent=2)+'\n')
export=v/'export';export.mkdir(exist_ok=True)
extract=subprocess.run(['debugfs','-R','rdump /evidence '+str(export),str(v/'guest.ext4')],capture_output=True,text=True)
(v/'extract-result.json').write_text(json.dumps({'exit':extract.returncode,'stdout':extract.stdout,'stderr':extract.stderr,'mode':'debugfs readonly after QEMU terminal'},indent=2)+'\n')
raw=(v/'console.log').read_text(errors='replace');markers=re.findall(r'^PG18_GUEST_RESULT=(\d+)\s*$',raw,re.MULTILINE)
records={}
for f in export.rglob('test-result.json'):
 data=json.loads(f.read_text());records[data['cohort']]=data
for phase,num in [('filter5',5)]:
 record=records.get(phase)
 if not record or record.get('pass')is not True or record.get('tests_run')!=num or record.get('skip')!=0:errors.append('missing/failed exact Guest '+phase)
artifact_files=list(export.rglob('actual-artifact-hashes.txt'))
if len(artifact_files)!=1:errors.append('missing unique actual Guest artifact hashes')
else:
 actual_hashes=artifact_files[0].read_text()
 for expected,path in [('65107494edf4b376c3dbe7960247d37d84c96b2e546bd791e3d3aefe5344d530','/opt/pg18/bin/postgres'),('3ac1cab277327eb5fe75637d7bb30810d3a2e80c9ca0affbbfdaf1f1e5aa74e1','/opt/pg18/lib/postgresql/test_dsg_region18.so'),('7cf45ced24c800ec8390fc7f04f07b8dc9e73ac0efa4bf8abd11d8513c8eb828','/sys/kernel/notes')]:
  if expected+'  '+path not in actual_hashes:errors.append('Guest artifact drift '+path)
passed=rc==0 and markers==['0'] and kvm=={'enabled':True,'present':True} and extract.returncode==0 and not errors
result={'status':'PASS_COMPONENT_ONLY'if passed else'FAIL_COMPONENT','host_qemu_exit':rc,'Guest_markers':markers,'actual_kvm':kvm,'Guest_roots':{p:{'pass':d.get('pass'),'tests_run':d.get('tests_run'),'skip':d.get('skip')}for p,d in records.items()},'expected_same_stable_roots':5,'new_coverage_count':0,'E':0,'Source_or_normal_qualified':False,'seconds':time.monotonic()-start,'errors':errors,'console_sha256':hashlib.sha256((v/'console.log').read_bytes()).hexdigest(),'scope':'actual accurate369 Guest C native component only; same5 filter IDs multi-platform; earlier9 Root Guest receipts retained; no production Source/System/FI/normal'}
(v/'result.json').write_text(json.dumps(result,indent=2)+'\n')
(v/'test.rc').write_text('0\n'if passed else'1\n')
raise SystemExit(0 if passed else 1)
