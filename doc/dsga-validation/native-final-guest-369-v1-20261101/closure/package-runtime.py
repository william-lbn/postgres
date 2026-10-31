from pathlib import Path
import os,json,hashlib,tarfile,stat,sys
v=Path('/out');facts=json.loads((v/'runtime-discovery.json').read_text());stdlib=Path(facts['stdlib'])
files=set();symlink_paths={}
def add(p):
 p=Path(os.path.normpath(str(p)))
 if str(p).startswith('/opt/pg18/'):return
 if str(p)in files:return
 assert p.exists()or p.is_symlink(),p
 files.add(str(p))
 if p.is_symlink():
  target=os.readlink(p);symlink_paths[str(p)]=target
  add(target if target.startswith('/')else str(p.parent/target))
for x in [facts['python_executable'],facts['python_realpath']]+facts['dependencies']:add(x)
for p in stdlib.rglob('*'):
 if p.is_dir():continue
 rel=p.relative_to(stdlib)
 if '__pycache__'in rel.parts or any(x in rel.parts for x in ['test','tests','ensurepip','idlelib']):continue
 if any(x.startswith('config-3.11-')for x in rel.parts):continue
 add(p)
# exact original cache for this TEST_ONLY runtime, not an authenticated pre-main producer.
if Path('/etc/ld.so.cache').is_file():add('/etc/ld.so.cache')
manifest={}
with tarfile.open(v/'runtime-closure.tar','w',dereference=False)as out:
 for name in sorted(files):
  p=Path(name);s=p.lstat();entry={'mode':oct(stat.S_IMODE(s.st_mode)),'uid':s.st_uid,'gid':s.st_gid,'size':s.st_size}
  if p.is_symlink():entry.update({'kind':'symlink','target':os.readlink(p),'resolved_realpath':str(p.resolve())})
  else:entry.update({'kind':'regular','sha256':hashlib.sha256(p.read_bytes()).hexdigest()})
  manifest[name]=entry;out.add(p,arcname=name.lstrip('/'),recursive=False)
result={'scope':'TEST_ONLY Guest runtime; preserves logical leaf symlink chains; parent usrmerge directories materialized as direct dirs, not production alias/RO Source qualification','files':manifest,'file_count':len(manifest),'runtime_tar_sha256':hashlib.sha256((v/'runtime-closure.tar').read_bytes()).hexdigest(),'runtime_tar_bytes':(v/'runtime-closure.tar').stat().st_size,'discovery_sha256':hashlib.sha256((v/'runtime-discovery.json').read_bytes()).hexdigest()}
(v/'runtime-closure-manifest.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({k:z for k,z in result.items()if k!='files'}))
