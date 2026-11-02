from pathlib import Path
import sys,subprocess,json,time,hashlib,os
p=Path(__file__).parent;container,pg,sock,admin=sys.argv[1:];out=p/'evidence';result={'status':'RUNNING','classification':'test-shim component exercising actual core source bit/API, not real Neon','normal':False,'Source':False,'E':0}
def save():(out/'static-origin-result.json').write_text(json.dumps(result,indent=2)+'\n')
def query(sql):
 r=subprocess.run(['docker','exec',container,pg+'/psql','-X','-A','-t','-v','ON_ERROR_STOP=1','-h',sock,'-p','55432','-U',admin,'-d','postgres','-c',sql],capture_output=True,text=True,timeout=12)
 if r.returncode:raise AssertionError('native SQL helper failed')
 return r.stdout.strip()
def wait_file(path,seconds=5):
 t=time.monotonic()
 while time.monotonic()-t<seconds:
  if path.exists():
   try:return json.loads(path.read_text())
   except ValueError:pass
  time.sleep(.01)
 raise TimeoutError('bounded native probe fact missing')
save()
try:
 t=time.monotonic();facts=[]
 while time.monotonic()-t<5:
  facts=list(out.glob('origin-*.json'))
  if facts:break
  time.sleep(.01)
 assert len(facts)==1;static=wait_file(facts[0]);assert static['getter'] is True and static['database_invalid'] and static['under_postmaster'];pid=static['pid'];pm=query("SELECT split_part(pg_read_file('postmaster.pid',0,64),chr(10),1)");assert pm.isdecimal()
 status=subprocess.run(['docker','exec',container,'cat','/proc/'+str(pid)+'/status'],capture_output=True,text=True,timeout=3);assert status.returncode==0
 fields=dict(line.split(':',1) for line in status.stdout.splitlines() if ':' in line)
 assert int(fields['Pid'].strip())==pid and int(fields['PPid'].strip())==int(pm)
 result['static_PM_probe']={**static,'actual_proc_pid_matched':True,'actual_parent_is_postmaster':True};save();(out/('release-'+str(pid))).write_text('release\n')
 query("CREATE FUNCTION dsg_static_probe_register(boolean) RETURNS bigint AS '$libdir/neon','dsg_static_probe_register' LANGUAGE C STRICT")
 command=['docker','exec',container,pg+'/psql','-X','-A','-t','-v','ON_ERROR_STOP=1','-h',sock,'-p','55432','-U',admin,'-d','postgres','-c','SELECT dsg_static_probe_register(false)']
 with (out/'dynamic-sql-private.log').open('wb') as f:
  proc=subprocess.Popen(command,stdout=f,stderr=subprocess.STDOUT)
  try:
   parent=wait_file(out/'dynamic-parent.json');dynamic=wait_file(out/('origin-'+str(parent['pid'])+'.json'));assert dynamic['pid']==parent['pid'] and dynamic['pid']!=pid and dynamic['getter'] is False and dynamic['database_invalid'];result['dynamic_same_descriptor_probe']={**dynamic,'original_core_handle_pid_matched':True};save();(out/('release-'+str(dynamic['pid']))).write_text('release\n');assert proc.wait(timeout=8)==0
  finally:
   if proc.poll() is None:proc.terminate();proc.wait(timeout=3)
 result['dynamic_same_descriptor_probe']['original_handle_reached_STOPPED']=True
 before=set(x.name for x in out.glob('origin-*.json'));value=query('SELECT dsg_static_probe_register(true)');after=set(x.name for x in out.glob('origin-*.json'));assert value=='-1' and before==after
 result['dynamic_forged_private_bit']={'actual_RegisterDynamicBackgroundWorker_return':False,'new_child_fact_count':len(after-before),'no_child_probe_created':True}
 assert len(before)==2;result['status']='PASS_MECHANISM_COMPONENT_ONLY';save()
except Exception as e:result['status']='FAIL';result['failure_class']=type(e).__name__;save();raise SystemExit(1)
