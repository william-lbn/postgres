from pathlib import Path
import subprocess,json,shlex,time,hashlib
p=Path(__file__).parent;nonce=p.name.removeprefix('pg17-dsm-standalone-fix-build-');tag='127.0.0.1:5001/neon/pg17-dsm-standalone-fix:'+nonce.lower()+'-TEST_ONLY'
cmd=['docker','buildx','build','--builder','neon-data','--resource','memory=8g','--resource','memory-swap=8g','--resource','cpu-quota=600000','--network','none','--progress','plain','--metadata-file',str(p/'build-metadata.json'),'--output','type=image,name='+tag+',push=true,registry.insecure=true','-f',str(p/'Dockerfile'),str(p/'context')];(p/'build-command.json').write_text(json.dumps(cmd,indent=2)+'\n');start=time.monotonic()
with(p/'build.log').open('wb')as log:rc=subprocess.run(['/bin/bash','-c','set -eu; source /data/neon-build/config/env.sh; exec '+shlex.join(cmd)],stdout=log,stderr=subprocess.STDOUT).returncode
result={'build_exit':rc,'seconds':time.monotonic()-start,'Source':False,'normal':False,'E':0,'test_only_visible_tag':tag}
if rc==0:
 meta=json.loads((p/'build-metadata.json').read_text());image='127.0.0.1:5001/neon/pg17-dsm-standalone-fix@'+meta['containerimage.digest'];result['image']=image
 with(p/'pull.log').open('wb')as log:pr=subprocess.run(['docker','pull',tag],stdout=log,stderr=subprocess.STDOUT).returncode
 result['pull_exit']=pr
 if pr==0:
  out=p/'export';out.mkdir();cid=subprocess.check_output(['docker','create','--entrypoint','/bin/true',image],text=True).strip()
  try:result['copy_exit']=subprocess.run(['docker','cp',cid+':/dsm-fix-v2-proof/.',str(out)],capture_output=True).returncode
  finally:subprocess.run(['docker','rm',cid],stdout=subprocess.DEVNULL,check=True)
  result['new_PG_ELF_sha256']=(out/'source-and-ELF.sha256').read_text().splitlines()[0].split()[0]
  assert result['new_PG_ELF_sha256']!='f88365d24889478e4ff88abb91f40cc735b565fd7078a2495fa84fb017812794'
  text=(out/'dynamic-tags.txt').read_text();result['no_RPATH_RUNPATH']='(RPATH)'not in text and'(RUNPATH)'not in text
 result['status']='PASS_BUILD_NOT_RUNTIME'if pr==0 and result.get('copy_exit')==0 and result.get('no_RPATH_RUNPATH')else'FAIL_EXPORT'
else:result['status']='BUILD_FAIL'
(p/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True)
if result['status']=='PASS_BUILD_NOT_RUNTIME':
 n=Path(json.loads((p/'native-successor.json').read_text())['path']);tool='/data/neon-build/continuous/resource-budget/budget.py';args=['/usr/bin/python3',tool,'reserve','--spec',str(n/'spec.json')];r=subprocess.run(args,capture_output=True,text=True);(p/'native-reserve.json').write_text(json.dumps({'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr},indent=2)+'\n')
 if r.returncode==0:
  args[2]='launch';r=subprocess.run(args,capture_output=True,text=True);(p/'native-launch.json').write_text(json.dumps({'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr},indent=2)+'\n');print('NATIVE_LAUNCH',r.returncode,flush=True)
 else:print('NATIVE_QUEUED',r.returncode,flush=True)
raise SystemExit(0 if result['status']=='PASS_BUILD_NOT_RUNTIME'else 1)
