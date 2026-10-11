#!/usr/bin/env python3
"""Independent stdlib frame/CRC decoder and vector price-time matching oracle.

Not linked to or generated from the C++ serializer/matcher. Hand-specified
struct fields/fixture, complete input ledger, counters, quantities and FIFO are
compared to all 64 exported canonical recovery states. Never trusts a claimed
zero mismatch count in place of recomputation.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zlib

FRAME = struct.Struct('>8sIIQQQBB6sQqqII')
HEADER = struct.Struct('>8sIBQQ8Q')
ORDER = struct.Struct('>QBqqQ')
ROOT = Path(__file__).resolve().parent
SEEDS = [7, 41, 2027, 20261011]
STAGES = ['before_write', 'partial_1', 'partial_7', 'partial_40',
          'partial_79', 'after_write_before_sync', 'after_sync_before_apply',
          'after_apply_before_ack', 'after_ack']


def require(ok, message):
    if not ok:
        raise AssertionError(message)


def fixed_fixture():
    records = []
    previous = 0
    for index, e in enumerate([(100,10,0,1,1,101,5), (100,20,0,1,2,101,7),
                               (101,30,2,0,0,0,6)], 1):
        ts, sequence, kind, side, oid, price, quantity = e
        prefix = struct.pack('>8sIIQQQBB6sQqqI', b'QDEVREC1',1,80,index,
                             ts,sequence,kind,side,bytes(6),oid,price,quantity,previous)
        previous = zlib.crc32(prefix)
        records.append(prefix + struct.pack('>I', previous))
    return b''.join(records)


def decode_frames(data):
    require(len(data) % 80 == 0, 'unexpected incomplete reference journal')
    previous = 0
    rows = []
    for i, start in enumerate(range(0, len(data), 80), 1):
        record = data[start:start+80]
        magic, version, length, index, ts, seq, kind, side, reserved, oid, price, qty, prev, crc = FRAME.unpack(record)
        require((magic,version,length,index,reserved,prev,crc) ==
                (b'QDEVREC1',1,80,i,bytes(6),previous,zlib.crc32(record[:76])),
                f'independent frame verification at {i}')
        previous = crc
        rows.append((ts,seq,kind,side,oid,price,qty))
    return rows


class Oracle:
    def __init__(self):
        self.orders = []
        self.stats = [0]*7
        self.last = (0,0)

    def apply(self, event):
        ts, seq, kind, side, oid, price, qty = event
        require(kind in (0,1,2) and side in (0,1) and qty > 0, 'oracle event domain')
        require(not self.stats[0] or ts >= self.last[0] and seq > self.last[1], 'oracle chronology')
        self.stats[0] += 1
        self.stats[1+kind] += 1  # event/Add/Cancel/Market/fills/quantity/rejected
        if kind == 1:
            order = next((o for o in self.orders if o[0] == oid), None)
            if order is None:
                self.stats[6] += 1
            else:
                order[3] -= min(qty,order[3])
                if order[3] == 0:
                    self.orders.remove(order)
        else:
            require(kind != 0 or oid > 0 and price > 0 and all(o[0] != oid for o in self.orders), 'oracle Add')
            eligible = [o for o in self.orders if o[1] != side and
                        (kind == 2 or (o[2] <= price if side == 0 else o[2] >= price))]
            for maker in sorted(eligible, key=lambda o: (o[2] if side == 0 else -o[2],o[4])):
                take = min(qty,maker[3])
                maker[3] -= take
                qty -= take
                self.stats[4] += 1
                self.stats[5] += take
                if maker[3] == 0:
                    self.orders.remove(maker)
                if qty == 0:
                    break
            if kind == 0 and qty > 0:
                self.orders.append([oid,side,price,qty,seq])
        self.last = (ts,seq)

    def snapshot(self):
        data = HEADER.pack(b'QDEVSNAP',1,int(self.stats[0]>0),*self.last,*self.stats,len(self.orders))
        for order in sorted(self.orders, key=lambda o: o[4]):
            data += ORDER.pack(*order)
        return data + struct.pack('>I',zlib.crc32(data))


def policy_model():
    # Explicit idealized volatile/durable-prefix model, not a physical power test.
    rows = []
    for written in range(33):
        for policy in ['write_only_ack', 'per_record_sync_ack', 'group_16_sync_ack']:
            durable = 0 if policy == 'write_only_ack' else written if policy == 'per_record_sync_ack' else written//16*16
            ack = written if policy == 'write_only_ack' else durable
            rows.append(dict(policy=policy,written=written,durable=durable,ack=ack,
                             acknowledged_loss=max(0,ack-durable),unacknowledged=written-ack))
    require(max(r['acknowledged_loss'] for r in rows if r['policy']=='write_only_ack')==32, 'negative model control')
    require(all(r['acknowledged_loss']==0 for r in rows if r['policy']!='write_only_ack'), 'sync model contract')
    return rows


def verify(study, executable, replay=None):
    fixture = fixed_fixture()
    require(bytes.fromhex((ROOT/'reference/journal/fixture.hex').read_text())==fixture, 'registered independent fixture')
    if executable:
        text = subprocess.check_output([str(executable),'--fixture'], text=True)
        require(bytes.fromhex(text.strip().split('JOURNAL_FIXTURE_HEX=')[1])==fixture, 'C++ fixture differs')
        unit = subprocess.check_output([str(executable)], text=True)
        actual = json.loads(unit.split('JOURNAL_UNIT_JSON=')[1])
        require(actual == dict(single_bit_refusals=1920,truncation_prefixes=240,
                               resealed_invalid_records=13,splice_refusals=3,
                               io_fault_scenarios=14,invalid_append_scenarios=8,
                               path_and_size_scenarios=5,exclusive_lock=True), 'unit case counts')
    else:
        actual = None
    inputs = list(csv.DictReader((study/'events.csv').open()))
    states = list(csv.DictReader((study/'states.csv').open()))
    require(len(inputs)==4096 and len(states)==64, 'registered event/state budgets')
    state_by_key = {(int(r['seed']),int(r['ordinal'])):bytes.fromhex(r['snapshot_hex']) for r in states}
    require(len(state_by_key)==64, 'duplicate state key')
    frame_count = state_count = 0
    for seed in SEEDS:
        source = [r for r in inputs if int(r['seed'])==seed]
        expected = [tuple(int(r[k]) for k in ['ts','sequence','type','side','id','price','quantity']) for r in source]
        require([int(r['ordinal']) for r in source]==list(range(1,1025)), 'input ordinal')
        decoded = decode_frames((study/f'journal_{seed}.bin').read_bytes())
        require(decoded==expected, 'journal input ledger differs')
        frame_count += len(decoded)
        oracle = Oracle()
        for ordinal, event in enumerate(decoded,1):
            oracle.apply(event)
            if ordinal % 64 == 0:
                require(oracle.snapshot()==state_by_key[(seed,ordinal)], 'independent canonical FIFO/counter state differs')
                state_count += 1
    crashes = list(csv.DictReader((study/'crashes.csv').open()))
    require(len(crashes)==288 and len({(int(r['stage']),int(r['replicate'])) for r in crashes})==288, 'crash budget/keys')
    for stage in range(9):
        rows = [r for r in crashes if int(r['stage'])==stage]
        require(sorted(int(r['replicate']) for r in rows)==list(range(32)), 'crash repetitions')
        for row in rows:
            tail = [1,7,40,79][stage-1] if 1 <= stage <= 4 else 0
            require({k:int(row[k]) for k in ['notified','killed','recovered_records','tail_bytes','canonical_match']} ==
                    dict(notified=1,killed=1,recovered_records=4 if stage>=5 else 3,tail_bytes=tail,canonical_match=1), 'crash outcome')
    summary = json.loads((study/'summary.json').read_text())
    require({k:summary[k] for k in ['events','seeds','reopens','process_crash_cases','crash_stages','replicates_per_stage','fork_guards','record_bytes','mismatches','actual_power_loss_tested']} ==
            dict(events=4096,seeds=SEEDS,reopens=64,process_crash_cases=288,crash_stages=9,
                 replicates_per_stage=32,fork_guards=2,record_bytes=80,mismatches=0,actual_power_loss_tested=False), 'summary scope')
    policies = policy_model()
    if replay:
        for name in ['events.csv','states.csv','crashes.csv']+[f'journal_{s}.bin' for s in SEEDS]:
            require((study/name).read_bytes()==(replay/name).read_bytes(),f'fixed study replay differs: {name}')
        other = json.loads((replay/'summary.json').read_text())
        # Filesystem identity legitimately differs between local and hosted CI.
        require({k:v for k,v in summary.items() if k!='filesystem_magic_decimal'} ==
                {k:v for k,v in other.items() if k!='filesystem_magic_decimal'}, 'replay summary')
    manifest = json.loads((ROOT/'reference/journal/registration_receipt.json').read_text())
    for path, digest in manifest['method_sha256'].items():
        require(hashlib.sha256((ROOT/path).read_bytes()).hexdigest()==digest, f'registered method changed: {path}')
    return dict(independent_frames=frame_count,independent_vector_oracle_states=state_count,
                process_crash_rows=288,unit=actual,abstract_policy_rows=len(policies),
                reference_files_equal=bool(replay),actual_power_loss_tested=False,
                filesystem_magic_decimal=summary['filesystem_magic_decimal'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--study',type=Path,default=ROOT/'reference/journal/study')
    parser.add_argument('--exe',type=Path)
    parser.add_argument('--replay',type=Path)
    args = parser.parse_args()
    result = verify(args.study,args.exe.resolve() if args.exe else None,args.replay)
    print('JOURNAL_REFERENCE_JSON='+json.dumps(result,sort_keys=True))
