from pathlib import Path
import subprocess,json,time,traceback,hashlib,re
p=Path(__file__).parent;target=json.loads((p/'target-source-lock.json').read_text());build=Path(target['parent_build']);built=json.loads((build/'result.json').read_text());start=time.monotonic();name=p.name.lower();result={'Source':False,'normal':False,'E':0,'stable_id':'PG17-S-DSM-STANDALONE-AND-CAP-001'}
assert built['status']=='PASS_BUILD_NOT_RUNTIME' and built['new_PG_ELF_sha256']!='f88365d24889478e4ff88abb91f40cc735b565fd7078a2495fa84fb017812794'
cmd=['docker','run','--name',name,'--pull=never','--network=none','--read-only','--cap-drop=ALL','--security-opt=no-new-privileges','--cpus=1','--memory=512m','--memory-swap=512m','--pids-limit=128','--shm-size=64m','--ulimit','core=0','--user','1000:1000','--tmpfs','/tmp:rw,nosuid,nodev,size=128m,mode=1777','-v',str(p/'probe')+':/probe:ro','-v',str(p/'evidence')+':/evidence:rw','-v',str(p/'run.sh')+':/run-targets.sh:ro','--entrypoint','/bin/sh',built['image'],'/run-targets.sh']
(p/'command.json').write_text(json.dumps(cmd,indent=2)+'\n');(p/'actual-artifact-input.json').write_text(json.dumps({'image':built['image'],'PG_ELF_sha256':built['new_PG_ELF_sha256'],'build_source_lock_sha256':hashlib.sha256((build/'source-lock.json').read_bytes()).hexdigest(),'target_lock_sha256':hashlib.sha256((p/'target-source-lock.json').read_bytes()).hexdigest()},indent=2)+'\n')
try:
 with(p/'run.log').open('w')as h:rc=subprocess.run(cmd,stdout=h,stderr=subprocess.STDOUT,timeout=150).returncode
 result['container_unique_Wait_exit']=rc
 o=json.loads(subprocess.check_output(['docker','inspect',name],text=True))[0];result['OOMKilled']=o['State']['OOMKilled'];(p/'container.json').write_text(json.dumps({'ID':o['Id'],'Image':o['Image'],'State':o['State'],'HostConfig':{k:o['HostConfig'].get(k)for k in ['Memory','MemorySwap','NanoCpus','ReadonlyRootfs','NetworkMode','SecurityOpt','CapDrop','CapAdd','PidsLimit','Ulimits','ShmSize']}},indent=2)+'\n')
 if rc==0:
  ev=p/'evidence';standalone=json.loads(re.search(r'\{[^\n]+\}',(ev/'standalone-DSM.log').read_text()).group(0));assert not standalone['accounting_valid'] and not standalone['limit_enforced'] and standalone['reserved_upper_bytes']==0 and standalone['external_segments']==0 and standalone['pending_creations']==0 and standalone['unknown_segments']==0
  base=json.loads((ev/'baseline.json').read_text());initial=json.loads((ev/'positive-initial.json').read_text());held=json.loads((ev/'positive-held.json').read_text());rejected=json.loads((ev/'positive-rejected.json').read_text());released=json.loads((ev/'positive-released.json').read_text());baseline=base['reserved_upper_bytes'];cap=int((ev/'fixed-positive-cap-kB.txt').read_text())*1024
  assert not base['limit_enforced'] and base['accounting_valid'] and base['pending_creations']==0 and base['unknown_segments']==0
  assert initial['limit_enforced'] and initial['accounting_valid'] and initial['reserved_upper_bytes']==baseline and initial['external_budget_bytes']==cap
  assert held['reserved_upper_bytes']==baseline+8192 and held['external_segments']==initial['external_segments']+1
  assert rejected['reserved_upper_bytes']==held['reserved_upper_bytes'] and rejected['external_segments']==held['external_segments'] and rejected['pending_creations']==0 and rejected['unknown_segments']==0
  assert released['reserved_upper_bytes']==baseline and released['external_segments']==initial['external_segments']
  assert (ev/'OS-before-rejected-create.txt').read_bytes()==(ev/'OS-after-rejected-create.txt').read_bytes()
  result.update({'checks':{'zero_cap_standalone_actual_cycle':True,'positive_standalone_startup_rejected':True,'positive_nativeoff_startup_rejected':True,'positive_fork_actual_OS_before_cap_rejection':True},'zero_cap_standalone_snapshot':standalone,'measured_native_baseline':base,'positive_initial':initial,'actual_held':held,'actual_after_rejected':rejected,'actual_released':released,'actual_absolute_test_cap_bytes':cap,'no_guessed_zero':True,'status':'PASS_TARGETED_COMPONENT_ONLY'})
 else:result['status']='FAIL_NATIVE_OR_FIXTURE'
except BaseException:
 result['driver_error']=True;result['status']='FAIL_DRIVER_OR_ASSERTION';(p/'driver-error.txt').write_text(traceback.format_exc())
finally:
 r=subprocess.run(['docker','rm','-f',name],capture_output=True,text=True);(p/'cleanup.json').write_text(json.dumps({'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr},indent=2)+'\n')
result['seconds']=time.monotonic()-start;(p/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n');print(json.dumps(result),flush=True);raise SystemExit(0 if result.get('status')=='PASS_TARGETED_COMPONENT_ONLY' and not result.get('OOMKilled')else 1)
