"""Independent Python decoder/standard-library CRC cross-check of C++ bytes."""
import argparse
import json
import struct
import subprocess
import zlib
from pathlib import Path

ROOT=Path(__file__).resolve().parent
p=argparse.ArgumentParser();p.add_argument('--exe',type=Path);p.add_argument('--capture',action='store_true');a=p.parse_args()
path=ROOT/'reference/recovery/fixture.hex'
if a.exe:
    result=subprocess.run([str(a.exe.resolve())],check=True,capture_output=True,text=True)
    line=[x for x in result.stdout.splitlines() if x.startswith('SNAPSHOT_FIXTURE_HEX=')]
    assert len(line)==1
    raw=bytes.fromhex(line[0].split('=',1)[1])
else:raw=bytes.fromhex(path.read_text().strip())
assert len(raw)==163 and raw[:8]==b'QDEVSNAP'
assert zlib.crc32(raw[:-4])==int.from_bytes(raw[-4:],'big')
assert struct.unpack_from('>IBQQ7QQ',raw,8)==(1,1,100,2,2,2,0,0,0,0,0,2)
orders=[struct.unpack_from('>QBqqQ',raw,93+33*i) for i in range(2)]
assert orders==[(1,1,101,5,1),(2,1,101,7,2)]
if a.capture:
    assert not path.exists(),'Do not overwrite a frozen format fixture silently'
    path.parent.mkdir(parents=True,exist_ok=True);path.write_text(raw.hex()+'\n')
else:assert raw.hex()==path.read_text().strip(),'Canonical C++ bytes changed'
print('INDEPENDENT_SNAPSHOT_FORMAT_AND_CRC_PASS')
