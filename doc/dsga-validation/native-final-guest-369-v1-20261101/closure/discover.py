import sys,sysconfig,os,json,subprocess,hashlib,stat
from pathlib import Path
stdlib=Path(sysconfig.get_path('stdlib'))
elf=[]
for root in [Path('/opt/pg18/bin'),Path('/opt/pg18/lib'),stdlib/'lib-dynload']:
 if root.exists():
  for p in root.rglob('*'):
   if p.is_file():
    try:
     with p.open('rb')as f:magic=f.read(4)
    except OSError:continue
    if magic==b'\x7fELF':elf.append(str(p))
elf+=list(dict.fromkeys([sys.executable,str(Path(sys.executable).resolve())]))
deps=set();observed=[];missing=[]
for name in sorted(set(elf)):
 z=subprocess.run(['/usr/bin/ldd',name],capture_output=True,text=True)
 # trusted TEST_ONLY build/tool ELFs only; ldd is not production pre-main closure proof.
 observed.append({'elf':name,'exit':z.returncode,'stdout':z.stdout,'stderr':z.stderr})
 if 'not found' in z.stdout:missing.append(name)
 for line in z.stdout.splitlines():
  words=line.strip().split()
  if '=>'in words:
   i=words.index('=>')+1
   if i<len(words)and words[i].startswith('/'):deps.add(words[i])
  elif words and words[0].startswith('/'):deps.add(words[0])
out={'python_version':sys.version,'python_executable':sys.executable,'python_realpath':str(Path(sys.executable).resolve()),'stdlib':str(stdlib),'ELF_count':len(set(elf)),'elfs':sorted(set(elf)),'dependencies':sorted(deps),'missing':missing,'ldd_raw':observed,'scope':'TEST_ONLY Guest runtime closure discovery, not authenticated production Source or constructor proof'}
Path('/out/runtime-discovery.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps({k:z for k,z in out.items()if k not in ['ldd_raw','elfs']}))
raise SystemExit(1 if missing else 0)
