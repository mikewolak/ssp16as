#!/usr/bin/env python3
"""cmpdasm.py -- compares ssp16as dasm output with MAME's ssp1601d.cpp output (build/mame_dasm) over the same range.

Each ssp16as line is rewritten into MAME's notation before the comparison:
  hexadecimal without 0x and leading zeros; 'ldi d, imm' -> 'ld d, imm'; 'ramB[aa]' -> the 9-bit address;
  'always=1' -> 'always'; 'ldi rN, imm' kept ('ldi' in MAME as well); the reserved condition codes c1, c11..c15
  -> 'RESERVED' (MAME's cond[] table).
MAME's disassembler prints 'add' for sub (arith_ops[1]) and 'mpya' for mpys (opcode group 0x1B); the rewrite applies
the same names so that only real disagreements remain.  Words that ssp16as emits as 'dw' are counted, not compared.
  cmpdasm.py ssp16as.s mame.txt
"""
import re, sys

def norm(t):
    t = re.sub(r'ram([01])\[0x([0-9A-F]+)\]', lambda m: '%X' % (int(m.group(1)) * 256 + int(m.group(2), 16)), t)
    t = re.sub(r'0x([0-9A-F]+)', lambda m: '%X' % int(m.group(1), 16), t)
    t = re.sub(r'^ldi (?!r[0-7],)', 'ld ', t)
    t = t.replace('always=1', 'always')
    t = re.sub(r'\b(c1|c11|c12|c13|c14|c15)=', 'RESERVED=', t)
    t = re.sub(r'^sub(i?) ', r'add\1 ', t)
    t = re.sub(r'^mpys ', 'mpya ', t)
    return t

mine = {}
for line in open(sys.argv[1]):
    m = re.match(r'\t(.*?)\s*; ([0-9A-F]{4,5}): ', line)
    if m: mine[int(m.group(2), 16)] = m.group(1).strip()
mame = {}
for line in open(sys.argv[2]):
    a, sz, txt = line.rstrip('\n').split(' ', 2)
    mame[int(a, 16)] = txt
same = diff = dw = dw_mame = unaligned = 0
for a, t in sorted(mine.items()):
    if t.startswith('dw '):
        dw += 1
        if a in mame and not mame[a].startswith('OP ='): dw_mame += 1
        continue
    if a not in mame:
        unaligned += 1; continue
    if norm(t) == mame[a]:
        same += 1
    else:
        diff += 1
        if diff <= 20: print('DIFF %04X  ssp16as: %-32s  mame: %s' % (a, t, mame[a]))
print('instructions compared %d: equal %d, different %d; dw words %d (%d of them decoded by MAME: non-canonical '
      'encodings with don\'t-care bits set); not at a MAME instruction start %d' % (same + diff, same, diff, dw, dw_mame, unaligned))
sys.exit(1 if diff else 0)
