"""Static PE-only preparation required by official LTK Patcher License v1. Never executes patcher."""
from pathlib import Path
import struct, hashlib, json, shutil
import argparse
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--inputs', type=Path, required=True,
                    help='Directory containing official-signed/ and LTK-PATCHER-LICENSE.md')
ROOT=parser.parse_args().inputs.resolve()
out=ROOT/'runtime'
out.mkdir(exist_ok=True)
records=[]
for name in ('ltk_patcher_host.exe','ltk_patcher_dll.dll'):
    src=ROOT/'official-signed'/name
    data=bytearray(src.read_bytes())
    assert data[:2]==b'MZ'
    pe=struct.unpack_from('<I',data,0x3c)[0]
    assert data[pe:pe+4]==bytes((80,69,0,0))
    opt=pe+24
    assert struct.unpack_from('<H',data,opt)[0]==0x20b
    security=opt+112+4*8
    cert_offset,cert_size=struct.unpack_from('<II',data,security)
    assert cert_offset>0 and cert_size>0
    assert cert_offset+cert_size==len(data), 'Certificate must be last data'
    assert cert_offset%8==0
    before=hashlib.sha256(data).hexdigest()
    struct.pack_into('<II',data,security,0,0)
    struct.pack_into('<I',data,opt+64,0)
    del data[cert_offset:]
    (out/name).write_bytes(data)
    records.append({'name':name,'source':f'https://raw.githubusercontent.com/LeagueToolkit/ltk-manager/v1.30.4/src-tauri/resources/{name}','original_sha256':before,'original_bytes':src.stat().st_size,'runtime_sha256':hashlib.sha256(data).hexdigest(),'runtime_bytes':len(data),'removed_certificate_bytes':cert_size,'signature_removed':True})
shutil.copyfile(ROOT/'LTK-PATCHER-LICENSE.md',out/'LTK-PATCHER-LICENSE.md')
manifest={'official_release':'v1.30.4','preparation':'Authenticode security directory zeroed, trailing WIN_CERTIFICATE removed, checksum cleared. No game tooling executed.','files':records}
(ROOT/'payload-manifest.json').write_text(json.dumps(manifest,indent=2)+chr(10),encoding='utf-8')
print(json.dumps(manifest,indent=2))
