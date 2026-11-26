#!/usr/bin/env python3
"""Fetch a bounded MS-SNSD noise subset, preserving upstream train/test separation."""
import argparse,concurrent.futures,hashlib,json,urllib.request
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
root=args.output;root.mkdir(parents=True,exist_ok=True)
rev='fe61c4ba0d9ac8dd7e23d719cc79f8947e1dc742'
tree=root/'tree.json'
if not tree.exists():
 with urllib.request.urlopen(f'https://api.github.com/repos/microsoft/MS-SNSD/git/trees/{rev}?recursive=1',timeout=60) as response: tree.write_bytes(response.read())
j=json.loads(tree.read_text())
assert j['sha']==rev
categories=['AirConditioner','Babble','ShuttingDoor','SqueakyChair','Typing','VacuumCleaner','WasherDryer']
items=[]
for t in j['tree']:
 path=Path(t['path']); stem=path.stem
 if path.suffix!='.wav' or '_' not in stem:continue
 category,num=stem.rsplit('_',1)
 if category not in categories:continue
 if path.parts[0]=='noise_train' and num in ('1','2'):split='training'
 elif path.parts[0]=='noise_test' and num=='1':split='validation'
 elif path.parts[0]=='noise_test' and num=='2':split='testing'
 else:continue
 items.append((t,split))
def get(item):
 t,split=item;p=root/split/Path(t['path']).name;p.parent.mkdir(exist_ok=True)
 url=f'https://raw.githubusercontent.com/microsoft/MS-SNSD/{rev}/{t["path"]}'
 if not p.exists():
  with urllib.request.urlopen(url,timeout=60) as r:data=r.read()
  assert len(data)==t['size'];p.write_bytes(data)
 data=p.read_bytes();print(split,p.name,len(data),flush=True)
 return dict(file=str(p.relative_to(root)),source=url,sha256=hashlib.sha256(data).hexdigest(),upstream_git_blob=t['sha'])
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool: records=list(pool.map(get,items))
assert len({r['sha256'] for r in records})==len(records),'Duplicate audio across splits'
(root/'sources.json').write_text(json.dumps(dict(dataset='Microsoft MS-SNSD',revision=rev,license_reference='https://github.com/microsoft/MS-SNSD#dataset-licenses',records=records),indent=2))
print('DONE',len(records),flush=True)
