"""Black-box CSV replay acceptance and rejection tests; no frozen research rerun."""
from __future__ import annotations
import pathlib
import subprocess
import sys
import tempfile

binary = pathlib.Path(sys.argv[1]).resolve()
header = "ts_ns,sequence,type,side,order_id,price_ticks,quantity"
valid = "100,1,ADD,BUY,1,99,10"

def invoke(payload: bytes):
    with tempfile.TemporaryDirectory() as d:
        path = pathlib.Path(d) / "events.csv"
        path.write_bytes(payload)
        return subprocess.run([str(binary), str(path)], capture_output=True, text=True, check=False)

for payload in [
    (header + "\n" + valid + "\n").encode(),
    (header + "\r\n" + valid + "\r\n").encode(),
    (valid + "\n").encode(),
    (header + "\n\n" + valid + "\n").encode(),
]:
    result = invoke(payload)
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert "events_processed=1" in result.stdout, result.stdout

bad_cases = [
    "100,1,ADD,BUY,1,99,10junk",           # trailing junk in integer
    "100,1,ADD,BUY,1,99,10,EXTRA",          # eighth field
    "100,1,ADD,BUY,1,99",                   # missing seventh field
    "100,1,ADD,BUY,1,99,",                  # empty required value
    "100,1,ADD,BUY,-1,99,10",               # negative unsigned order ID
    "-100,1,ADD,BUY,1,99,10",               # negative unsigned time
    "100,18446744073709551616,ADD,BUY,1,99,10",  # uint64 overflow
    "100,1,ADD,BUY,1,99,9223372036854775808",   # int64 overflow
    "100,1,ADD,BUY,1,99, 10",               # leading whitespace
    "100,1,ADD,BUY,1,99,10 ",               # trailing whitespace
    "100,1,BAD,BUY,1,99,10",                # invalid type
    "100,1,ADD,BAD,1,99,10",                # invalid side
    "100,1,ADD,BUY,1,99,-1",                # engine invariant rejects quantity
]
for bad in bad_cases:
    result = invoke((header + "\n" + bad + "\n").encode())
    assert result.returncode != 0, (bad, result.stdout)
    assert "row 2:" in result.stderr, (bad, result.stderr)

print("CSV replay acceptance and malformed-input checks passed")
