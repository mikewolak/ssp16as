#!/bin/sh
# gate_o.sh -- gate O: every 16-bit opcode word.  tests/out/op.bin holds 65,536 entries of two words (opcode W,
# extension 0x1234).  Checks: (1) ssp16as dasm of the image re-assembles to the identical bytes; (2) for every entry
# whose opcode ssp16as decodes as an instruction, ssp16as's text equals MAME's ssp1601d.cpp text after the notation
# map of tests/cmpdasm.py; (3) every opcode MAME decodes as an instruction, ssp16as decodes too, or the difference is
# listed by cmpdasm.py as a non-canonical encoding.
set -eu
: "${SSP16AS:=./ssp16as}" "${MAME_DASM:=tests/mame_dasm}"
O=tests/out; mkdir -p $O
python3 -I -c "
import sys
with open('$O/op.bin', 'wb') as f:
    for w in range(65536): f.write(w.to_bytes(2, 'big') + (0x1234).to_bytes(2, 'big'))"
$SSP16AS dasm -r 0:0x20000 -o $O/op.s $O/op.bin
$SSP16AS asm -o $O/op_re.bin $O/op.s
cmp $O/op.bin $O/op_re.bin
echo "gate O: dasm -> asm round trip of 131,072 words byte-identical"
# per-entry comparison: ssp16as text at every even word (each entry decoded on its own)
python3 -I -c "
import re
ent = {}
for line in open('$O/op.s'):
    m = re.match(r'\t(.*?)\s*; ([0-9A-F]{4,5}): ', line)
    if m: ent[int(m.group(2), 16)] = m.group(1).strip()
with open('$O/op_even.s', 'w') as f:
    for a in range(0, 0x20000, 2):
        f.write('\t%s ; %04X: \n' % (ent.get(a, 'dw ?'), a))"
$MAME_DASM $O/op.bin 0 0x20000 2 > $O/op_mame.txt
python3 -I tests/cmpdasm.py $O/op_even.s $O/op_mame.txt
