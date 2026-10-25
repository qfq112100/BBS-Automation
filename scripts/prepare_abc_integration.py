"""Pack a reviewed local ABC program, never its credentials, logs or 4GB cache.
Runs only Windows makecab, never ABC, DLLs, scripts or plugins.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

RUNTIME = (
    'ABC.exe', 'dzHPSocket4C.dll', 'RapidJSON.dll', 'data/areaname.json',
    'lib/abclogin.dll', 'lib/config.toml', 'lib/dzHPSocket4C.dll',
    'lib/killLOL.bat', 'lib/libcurl.dll', 'lib/RapidJSON.dll',
    'lib/sqlite3.dll', 'lib/YamlHelper.dll',
    'lib/卸载客户端插件.bat', 'lib/安装客户端插件.bat',
)
SEEDS = ('data/config.json', 'data/user.db', 'data/abc.ini', 'data/updates.ini')
ENTRY_SHA = '8cd5726913c926e28699a4c0caa239d8e834af3bb64f37232508da5586ca7f11'

def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024*1024), b''):
            h.update(block)
    return h.hexdigest()

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,required=True)
    p.add_argument('--seed',type=Path,required=True)
    p.add_argument('--baseline-manifest',type=Path,required=True)
    p.add_argument('--work',type=Path,required=True)
    p.add_argument('--output',type=Path,default=Path(__file__).resolve().parents[1]/'resources'/'integrations')
    a=p.parse_args()
    if a.work.exists(): raise ValueError('Use fresh staging directory')
    if digest(a.source/'ABC.exe') != ENTRY_SHA: raise ValueError('Reviewed ABC executable changed')
    a.work.mkdir(parents=True)
    data=a.work/'data'; data.mkdir()
    outcab=a.work/'cab'; outcab.mkdir()
    records=[]
    for n,name in enumerate(RUNTIME+SEEDS):
        source=(a.seed if name in SEEDS else a.source)/name
        if source.is_symlink() or not source.is_file(): raise ValueError('Missing/unsafe reviewed input '+name)
        h=digest(source)
        token=f'f{n:06d}'
        target=data/token
        shutil.copyfile(source,target)
        if digest(target)!=h: raise ValueError('Input changed while staging '+name)
        item={'path':name,'cab':token,'size':target.stat().st_size,'sha256':h}
        if name in SEEDS or name=='lib/config.toml': item['mutable']=True
        records.append(item)
    # Verify old CABs against the captured pre-integration manifest; never repack them.
    old=json.loads(a.baseline_manifest.read_text(encoding='utf-8-sig'))
    if len(old['tools'])!=3 or any(x['id']=='abc' for x in old['tools']):
        raise ValueError('Expected reviewed three-tool baseline')
    for tool in old['tools']:
        if digest(a.output/(tool['id']+'.cab'))!=tool['cab_sha256']:
            raise ValueError('Existing tool payload changed')
    payload_hash=hashlib.sha256(json.dumps(records,ensure_ascii=False,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    ddf=['.OPTION EXPLICIT','.Set Cabinet=ON','.Set Compress=ON','.Set CompressionType=LZX',
         '.Set CompressionMemory=21','.Set MaxDiskSize=0','.Set MaxCabinetSize=0',
         '.Set CabinetNameTemplate=abc.cab','.Set DiskDirectoryTemplate="'+str(outcab)+'"']
    for f in records: ddf.append('"'+str(data/f['cab'])+'" "'+f['cab']+'"')
    ddf_path=a.work/'payload.ddf'; ddf_path.write_text('\n'.join(ddf)+'\n',encoding='ascii')
    with (a.work/'makecab.log').open('wb') as log:
        subprocess.run(['makecab.exe','/F',str(ddf_path)],stdout=log,stderr=subprocess.STDOUT,check=True)
    cab=outcab/'abc.cab'
    shutil.copyfile(cab,a.output/'abc.cab')
    abc={'id':'abc','name':'ABC','version':'local-8cd5726913c9',
         'storage_version':'local-'+payload_hash[:12], 'entry':'ABC.exe',
         'cab_resource':3104,'elevated':True,'source':'user-provided local ABC program',
         'bindings':[], 'files':records,'cab_size':cab.stat().st_size,
         'cab_sha256':digest(cab),'total_bytes':sum(f['size'] for f in records)}
    old['tools'].append(abc)
    (a.output/'manifest.json').write_text(json.dumps(old,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    report={'files':len(records),'program_bytes':abc['total_bytes'],'cab_bytes':abc['cab_size'],
            'cab_sha256':abc['cab_sha256'],'storage_version':abc['storage_version'],
            'entry_sha256':ENTRY_SHA,'old_tool_specs_and_cabs_unchanged':True,
            'original_credentials_logs_translation_cache_embedded':False,
            'scripts_or_app_executed':False}
    (a.work/'pack-report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report),flush=True)

if __name__=='__main__': main()
