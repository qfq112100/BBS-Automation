"""Build a local Rose dependency compatibility candidate from unchanged vendor files.
No executable is run. Do not publish signed binaries without the required license grant.
"""
from __future__ import annotations
import argparse, copy, hashlib, json, shutil, subprocess
from pathlib import Path

def digest(p):
    h=hashlib.sha256()
    with Path(p).open('rb') as s:
        for b in iter(lambda:s.read(1048576),b''): h.update(b)
    return h.hexdigest()

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--previous',type=Path,required=True)
    ap.add_argument('--pair',type=Path,required=True)
    ap.add_argument('--work',type=Path,required=True)
    ap.add_argument('--patcher-version',choices=('1.29.0','1.29.1'),default='1.29.0')
    ap.add_argument('--output',type=Path,default=Path(__file__).resolve().parents[1]/'resources'/'integrations')
    a=ap.parse_args()
    if a.work.exists(): raise ValueError('Use a fresh staging directory')
    a.work.mkdir(parents=True)
    old=json.loads((a.previous/'manifest.json').read_text(encoding='utf-8-sig'))
    expected={p.name:p for p in a.pair.iterdir() if p.is_file()}
    names=('ltk_patcher_host.exe','ltk_patcher_dll.dll','LTK-PATCHER-LICENSE.md')
    for n in names:
        if n not in expected: raise ValueError('Missing reviewed candidate '+n)
    pins={'1.29.0':('23fa1aaeda1a0c743da44227f179084e1fb81c7cd262144a3c8604ffc49d3bc1','6d419057e6667994ba752ad0fb089b363db98267618644d7f7b6632441a21d74'),'1.29.1':('23fa1aaeda1a0c743da44227f179084e1fb81c7cd262144a3c8604ffc49d3bc1','26aa7f8d5790c5bb39b45cfeef8251149d47d5dfb7fe43648aa774d5e6bb42fa')}
    for n,pin in zip(names[:2],pins[a.patcher_version]):
        if digest(expected[n])!=pin: raise ValueError('Candidate pin mismatch '+n)
    if digest(expected[names[2]])!='fe4ee9f68082b73082a04764fe1d09a0177f69b9168db240f9e026b91ba44bc4': raise ValueError('License pin mismatch')
    result=[]
    for original in old['tools']:
        d=copy.deepcopy(original)
        if d['id']=='akari':
            if digest(a.output/'akari.cab')!=d['cab_sha256']: raise ValueError('Akari cabinet changed')
            result.append(d); continue
        key=d['id']; stage=a.work/key; data=stage/'data'; cabdir=stage/'cab'
        data.mkdir(parents=True); cabdir.mkdir()
        source=a.previous/(key+'.cab')
        if digest(source)!=d['cab_sha256']: raise ValueError('Original CAB hash changed')
        with (stage/'expand.log').open('wb') as log:
            subprocess.run(['expand.exe','-F:*',str(source),str(data)],check=True,stdout=log,stderr=subprocess.STDOUT)
        for f in d['files']:
            target=data/f['cab']
            if target.stat().st_size!=f['size'] or digest(target)!=f['sha256']: raise ValueError('Original token mismatch '+f['path'])
            name=Path(f['path']).name
            if name in names and (key=='ltk' or f['path'].startswith('_internal/injection/tools/')):
                shutil.copyfile(expected[name],target)
                f['size']=target.stat().st_size; f['sha256']=digest(target)
        if key=='rose': d['storage_version']='1.4.9-ltk'+a.patcher_version
        else:
            d['version']=a.patcher_version; d['source']='https://github.com/LeagueToolkit/ltk-manager/tree/v'+a.patcher_version
        ddf=['.OPTION EXPLICIT','.Set Cabinet=ON','.Set Compress=ON','.Set CompressionType=LZX','.Set CompressionMemory=21','.Set MaxDiskSize=0','.Set MaxCabinetSize=0','.Set CabinetNameTemplate='+key+'.cab','.Set DiskDirectoryTemplate="'+str(cabdir)+'"']
        for f in d['files']: ddf.append('"'+str(data/f['cab'])+'" "'+f['cab']+'"')
        path=stage/'payload.ddf'; path.write_text('\n'.join(ddf)+'\n',encoding='ascii')
        with (stage/'makecab.log').open('wb') as log:
            subprocess.run(['makecab.exe','/F',str(path)],check=True,stdout=log,stderr=subprocess.STDOUT)
        archive=cabdir/(key+'.cab'); shutil.copyfile(archive,a.output/archive.name)
        d['cab_size']=archive.stat().st_size; d['cab_sha256']=digest(archive)
        d['total_bytes']=sum(f['size'] for f in d['files'])
        result.append(d)
    final=dict(format=1,tools=result)
    (a.output/'manifest.json').write_text(json.dumps(final,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'rose':'1.4.9','storage':'1.4.9-ltk'+a.patcher_version,'patcher':a.patcher_version,'akari_unchanged':True,'pair':{n:digest(expected[n]) for n in names}}),flush=True)
if __name__=='__main__': main()
