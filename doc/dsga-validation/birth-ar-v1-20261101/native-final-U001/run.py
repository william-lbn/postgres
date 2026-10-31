from pathlib import Path
import subprocess,json,time,traceback
v=Path(__file__).parent;cmd=json.loads((v/'command.json').read_text());name=cmd[cmd.index('--name')+1];start=time.monotonic();rc=125
try:
 with(v/'run.log').open('wb')as f:
  try:rc=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,timeout=120).returncode
  except subprocess.TimeoutExpired:rc=124
except BaseException:(v/'driver-error.txt').write_text(traceback.format_exc())
finally:
 result=subprocess.run(['docker','inspect',name],capture_output=True,text=True)
 if result.returncode==0:
  o=json.loads(result.stdout)[0];(v/'container.json').write_text(json.dumps({'Image':o['Image'],'State':o['State'],'HostConfig':{k:o['HostConfig'].get(k)for k in ['Init','Memory','MemorySwap','NanoCpus','NetworkMode','ReadonlyRootfs','CapAdd','CapDrop','SecurityOpt','Tmpfs']}},indent=2)+'\n')
  if o['State'].get('OOMKilled'):rc=137
 subprocess.run(['docker','rm','-f',name],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
(v/'result.json').write_text(json.dumps({'exit':rc,'seconds':time.monotonic()-start},indent=2)+'\n')
(v/'test.rc').write_text(str(rc)+'\n')
raise SystemExit(rc)
