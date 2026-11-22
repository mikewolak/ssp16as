#!/bin/sh
# gate_e.sh -- gate E: the preprocessor and the error path.  A source using .include, .equ, .macro and .rept must
# assemble to the same words as its flat form; each faulty line must be reported as FILE:LINE: error.
set -eu
: "${SSP16AS:=./ssp16as}"
O=tests/out; mkdir -p $O
cat > $O/inc.s <<'X'
STEP equ 0x80
X
cat > $O/pp.s <<'X'
        .include "inc.s"
        .equ LOOPS 3
        .macro wr4 REG
        .rept 4
        ld EXT4, REG
        .endr
        .endm
        org 0x400
start:  ldi A, LOOPS
        wr4 Y
        addi STEP
        bra n=0, start
X
cat > $O/flat.s <<'X'
STEP equ 0x80
        org 0x400
start:  ldi A, 3
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        addi STEP
        bra n=0, start
X
$SSP16AS asm -o $O/pp.bin $O/pp.s
$SSP16AS asm -o $O/flat.bin $O/flat.s
cmp $O/pp.bin $O/flat.bin
n=0
check() {   # $1 source line, $2 expected message fragment
    printf '        org 0x400\n        nop\n%s\n' "$1" > $O/bad.s
    if $SSP16AS asm -o $O/bad.bin $O/bad.s 2> $O/bad.err; then echo "gate E: FAIL accepted: $1"; exit 1; fi
    grep -q "^$O/bad.s:3: error: .*$2" $O/bad.err || { echo "gate E: FAIL message for '$1':"; cat $O/bad.err; exit 1; }
    n=$((n + 1))
}
check '        ld A, ram0[0x100]' 'RAM address 256'
check '        ldi r0, 0x1FF' 'immediate 511'
check '        bra z=2, 0x400' 'no form'
check '        mpya (r0), (r4)' 'operands are'
check '        ld A, undefined_label' 'no form'
check '        ldi A, undefined_label' 'undefined symbol'
check '        mod always, rotate' 'unknown accumulator operation'
check '        frob A' 'no form'
echo "gate E: preprocessed source byte-identical to its flat form; $n faulty lines reported as FILE:LINE"
