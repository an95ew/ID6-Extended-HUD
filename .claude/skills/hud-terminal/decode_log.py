#!/usr/bin/env python3
"""Summarises a HUD worker log taken with --log (every terminal call and answer on stderr), so a car result is read from the
raw lines and not guessed (run 1 blamed the car for our sender; run 4's T0 "unanswered call" was an op F without a handle).

    python3 .claude/skills/hud-terminal/decode_log.py LOG [LOG ...]

Per function ID: calls sent, answers by OpType (D ResultAck = OK on this car, C Result, 9 ErrorAck, F Error, A ProcessingAck),
answers that carry no handle (the sender cannot match them: strays, then a 1.5 s timeout), and the worker's last stats line.
Exit status 1 when any error, unmatched answer, timeout or dropped batch shows up."""
import re
import sys
from collections import Counter, defaultdict

NAMES = {0x204: 'SetVisibility', 0x20E: 'SwitchVisibility', 0x210: 'TextareaSetText', 0x218: 'ImageSetLineColor',
         0x219: 'ImageSetLineWidth', 0x21B: 'ImageSetFillColor', 0x21C: 'ImageSetFillMode', 0x21D: 'ImageAddPolygon',
         0x21E: 'ImageAddCircle', 0x220: 'ImageRemoveAllItems', 0xC04: 'ImageGetSize', 0xC09: 'ImageAddCircleDiameter'}
TX = re.compile(r'\[hud\] tx ([0-9A-Fa-f]+) \S+ (\d+) ')
RX = re.compile(r'\[hud\] rx ([0-9A-Fa-f]+) op ([0-9A-Fa-f]) len (\d+): ?([0-9a-f ]*)')
STATS = re.compile(r'errors (\d+) timeouts (\d+) strays (\d+)')


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    bad = False
    for path in sys.argv[1:]:
        sent, handles = Counter(), defaultdict(set)
        ops = defaultdict(Counter)
        nohandle = Counter()
        last = None
        for line in open(path, errors='replace'):
            m = TX.search(line)
            if m:
                f = int(m.group(1), 16)
                sent[f] += 1
                handles[f].add(int(m.group(2)))
                continue
            m = RX.search(line)
            if m:
                f, op, n = int(m.group(1), 16), m.group(2).upper(), int(m.group(3))
                ops[f][op] += 1
                b = bytes.fromhex(m.group(4).replace(' ', '')) if m.group(4).strip() else b''
                h = b[0] << 8 | b[1] if n >= 2 and len(b) >= 2 else None
                if h is None or h not in handles[f]:
                    nohandle[f] += 1
                continue
            if '[hud] rpm' in line or 'batches' in line:
                last = line.strip()
        print('== %s' % path)
        for f in sorted(set(sent) | set(ops)):
            o = ' '.join('%s:%d' % kv for kv in sorted(ops[f].items()))
            flag = ''
            if ops[f].get('F') or ops[f].get('9'):
                flag += '  ERROR-ANSWERS'
            if nohandle[f]:
                flag += '  %d without a sent handle (unmatchable)' % nohandle[f]
            print('  %03X %-22s sent %4d  answers %s%s' % (f, NAMES.get(f, '?'), sent[f], o or '-', flag))
            bad |= bool(flag)
        if last:
            print('  last: ' + last)
            m = STATS.search(last)
            if m and any(int(x) for x in m.groups()):
                bad = True
        if 'answers dropped' in open(path, errors='replace').read():
            print('  answers dropped')
            bad = True
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
