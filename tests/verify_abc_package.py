"""Pure file-only release checks; never run ABC, scripts, UI or the backup service."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sqlite3

p=argparse.ArgumentParser()
p.add_argument('--manifest',type=Path,required=True)
p.add_argument('--baseline',type=Path,required=True)
p.add_argument('--stage',type=Path,required=True)
p.add_argument('--source',type=Path,required=True)
a=p.parse_args()
current=json.loads(a.manifest.read_text(encoding='utf-8-sig'))
old=json.loads(a.baseline.read_text(encoding='utf-8-sig'))
assert current['tools'][:3]==old['tools'], 'Existing component specification changed'
assert len(current['tools'])==4
abc=current['tools'][3]
assert abc['id']=='abc' and abc['entry']=='ABC.exe' and abc['cab_resource']==3104 and abc['elevated']
files={x['path']:x for x in abc['files']}
assert len(files)==18
assert {'dzHPSocket4C.dll','RapidJSON.dll','lib/dzHPSocket4C.dll','lib/RapidJSON.dll'}<=files.keys()
assert not any(x.startswith(('Log/','lib/logs/','汉化缓存/')) for x in files)
mutable={'data/config.json','data/user.db','data/abc.ini','data/updates.ini','lib/config.toml'}
assert {x['path'] for x in abc['files'] if x.get('mutable')}==mutable
for record in abc['files']:
    assert re.fullmatch(r'f[0-9]{6}',record['cab'])
    path=a.stage/'data'/record['cab']
    assert path.stat().st_size==record['size']
    assert hashlib.sha256(path.read_bytes()).hexdigest()==record['sha256']
assert len({x['cab'] for x in abc['files']})==18
assert files['ABC.exe']['sha256']=='8cd5726913c926e28699a4c0caa239d8e834af3bb64f37232508da5586ca7f11'
assert files['data/config.json']['sha256']=='8c26fab41d53a655030e8f1a567137f0b338e0ad9af7c80debfCf4b09a4da91d'.lower()
config=json.loads((a.stage/'data'/files['data/config.json']['cab']).read_text(encoding='utf-8-sig'))
blank=['APIKEY','打码Email','打码代理信息','游戏目录','命令行']
assert all(config[x]=='' for x in blank)
original_config=json.loads((a.source/'data/config.json').read_text(encoding='utf-8-sig'))
for key,value in config.items():
    if key.startswith('模拟登录_') and key.endswith(('X','Y')):
        assert key in original_config and type(value) is type(original_config[key]) and value==original_config[key], 'Coordinate fields were changed'
db=a.stage/'data'/files['data/user.db']['cab']
with sqlite3.connect(db.as_uri()+'?mode=ro&immutable=1',uri=True) as connection:
    assert connection.execute('SELECT COUNT(*) FROM ABC').fetchone()[0]==0
    assert [x[1] for x in connection.execute('PRAGMA table_info(ABC)')]==['user','password','cookies']
    assert connection.execute('PRAGMA user_version').fetchone()[0]==0
for tool in old['tools']:
    cab=a.manifest.parent/(tool['id']+'.cab')
    assert hashlib.sha256(cab.read_bytes()).hexdigest()==tool['cab_sha256']
for width in (940,983,1012,1240):
    for dpi in (96,120,144,192):
        scale=lambda v:(v*dpi+48)//96
        rects=[(scale(width-offset),scale(88),scale(width-offset+112),scale(120)) for offset in (384,264,144)]
        assert all(r[0]>=scale(224) and r[1]>=scale(56) and r[3]<scale(160) for r in rects)
        assert all(rects[i][2]<rects[i+1][0] for i in (0,1))
        assert rects[-1][2]==scale(width-32)
print(json.dumps({'passed':True,'files':18,'account_rows':0,'private_fields_blank':blank,
                  'original_tool_specs_and_cabs_unchanged':True,'header_geometry_model_cases':16,
                  'actual_gui_or_startup_tested':False,'scripts_executed':False},ensure_ascii=False))
