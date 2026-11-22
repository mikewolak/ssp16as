#!/bin/sh
# gate_v.sh -- gate V: Virtua Racing's SSP program memory (image bytes 0x800-0x1FFFF, words 0x0400-0xFFFF).
# The owner's image is read in place from VR_ROM and never copied.  ssp16as dasm -> ssp16as asm must give the same
# 122,880 bytes; ssp16as's instruction text must equal MAME's at every address both decode as an instruction.
set -eu
: "${SSP16AS:=./ssp16as}" "${MAME_DASM:=tests/mame_dasm}"
O=tests/out; mkdir -p $O
[ -f "$VR_ROM" ] || { echo "gate V: VR_ROM not found: $VR_ROM"; exit 1; }
python3 -I -c "import zlib,sys; c=zlib.crc32(open(sys.argv[1],'rb').read()); print('gate V: %s CRC-32 %08X' % (sys.argv[1], c)); sys.exit(c != 0x7E1A324A)" "$VR_ROM"
$SSP16AS dasm -r 0x400:0x10000 -o $O/vr.s "$VR_ROM"
$SSP16AS asm -o $O/vr_re.bin $O/vr.s
dd if="$VR_ROM" of=$O/vr_ssp.bin bs=2048 skip=1 count=63 2>/dev/null
cmp $O/vr_ssp.bin $O/vr_re.bin
echo "gate V: dasm -> asm round trip of words 0x0400-0xFFFF (122,880 bytes) byte-identical"
$MAME_DASM "$VR_ROM" 0x400 0x10000 > $O/vr_mame.txt
python3 -I tests/cmpdasm.py $O/vr.s $O/vr_mame.txt
