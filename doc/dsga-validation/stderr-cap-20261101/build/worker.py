from pathlib import Path
import subprocess,json,shlex,time
p=Path(__file__).parent
tag='127.0.0.1:5001/neon/pg17-stderr-cap:20261101t133024z-21ad5a-TEST_ONLY'
command=['docker','buildx','build','--builder','neon-data','--resource','memory=8g','--resource','memory-swap=8g','--resource','cpu-quota=600000','--network','none','--progress','plain','--metadata-file',str(p/'build-metadata.json'),'--output','type=image,name='+tag+',push=true,registry.insecure=true','-f',str(p/'Dockerfile'),str(p/'context')]
(p/'build-command.json').write_text(json.dumps(command,indent=2)+'\n');tick=time.monotonic()
with(p/'build.log').open('wb')as log:rc=subprocess.run(['/bin/bash','-c','set -eu; source /data/neon-build/config/env.sh; exec '+shlex.join(command)],stdout=log,stderr=subprocess.STDOUT).returncode
result={'build_exit':rc,'seconds':time.monotonic()-tick,'Source':False,'normal':False}
if rc==0:
 metadata=json.loads((p/'build-metadata.json').read_text());image='127.0.0.1:5001/neon/pg17-stderr-cap@'+metadata['containerimage.digest'];result['image']=image
 with(p/'pull.log').open('wb')as log:pr=subprocess.run(['docker','pull',image],stdout=log,stderr=subprocess.STDOUT).returncode
 result['pull_exit']=pr
 if pr==0:
  export=p/'export';export.mkdir()
  cid=subprocess.check_output(['docker','create','--entrypoint','/bin/true',image],text=True).strip()
  try:
   result['copy_exit']=subprocess.run(['docker','cp',cid+':/stderr-cap-proof/.',str(export)],capture_output=True).returncode
  finally:subprocess.run(['docker','rm',cid],stdout=subprocess.DEVNULL,check=True)
  text=(export/'dynamic-tags.txt').read_text();result['no_RPATH_RUNPATH']='(RPATH)'not in text and'(RUNPATH)'not in text
  result['source_index_lines']=len((export/'compiled-source-index.txt').read_text().splitlines())
 result['status']='PASS_BUILD_NOT_RUNTIME'if pr==0 and result.get('copy_exit')==0 and result.get('no_RPATH_RUNPATH')else'FAIL_EXPORT'
else:result['status']='BUILD_FAIL'
(p/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True);raise SystemExit(0 if result['status']=='PASS_BUILD_NOT_RUNTIME'else 1)
