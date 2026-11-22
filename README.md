# ssp16as: SSP1601 Assembler and Disassembler

**Two-pass assembler and disassembler for the Samsung SSP1601 DSP of the Sega Virtua Processor (SVP, Sega 315-5750). Full SSP1601 instruction set as decoded by the MAME disassembler, labels, equ, org, dw, expressions, the espp preprocessor (.include, .equ, .macro, .rept, .if), listing file, three output formats. Strict C99, POSIX getopt(), libc only. Project svp-lara, Mike Wolak, EPROM Foundry Inc.**

---

## 1 Features

| Item | Specification |
|---|---|
| Instruction set | 28 instruction forms: ld (11 addressing forms), ldi (3), sub/cmp/add/and/or/eor (7 forms each), mod (8 accumulator operations), 8 flag operations, call, bra, mld, mpya, mpys, nop, ret |
| Registers | `-` X Y A ST STACK PC P EXT0..EXT6 AL; aliases PM0 PM1 PM2 XST PM4 PMC |
| Pointer registers | r0..r7 with the modifiers none, `+!`, `-`, `+`; r3 and r7 with `\|00` `\|01` `\|10` `\|11` |
| Conditions | always, gpi0, gpi1, l, z, ov, n, diof, gpi2, gpi3 with `=0` or `=1`; reserved codes c1, c11..c15 |
| Source | labels `name:`, `NAME equ EXPR`, `org EXPR` (word address), `dw EXPR,...`; espp directives (section 4) |
| Expressions | 0x/$ hexadecimal, decimal, symbols, `+ - * / % << >> & \| ^ ~`, parentheses; 32-bit host `long` |
| Output | `image`: a cartridge image with the words placed at byte 2w (base image with `-b`); `raw`: big-endian words from the lowest to the highest address; `s`: GNU as `.org`/`.word` source for the 68000 side |
| Listing | address, words, canonical disassembly and source line, with build time and word range |
| Disassembler | canonical text that re-assembles to the same words; a word with no canonical encoding is written as `dw` |
| Errors | `FILE:LINE: error: message`, provenance through `.include` and macro expansion |
| Build | `make`: `cc -std=c99 -O2 -Wall -Wextra -pedantic`, no warnings; 883 lines of C plus espp (671 lines) |

## 2 Usage

```
ssp16as asm  [-o OUT] [-f image|raw|s] [-b BASE] [-s BYTES] [-l LST] [-I DIR] [-D N[=V]] FILE.s
ssp16as dasm [-o OUT] [-r FIRST:END] FILE.bin
ssp16as isa
```

| Option | Function |
|---|---|
| `-o OUT` | output file; `out.bin` for asm, standard output for dasm |
| `-f FMT` | asm output format: `image` (default with `-b`), `raw` (default without `-b`), `s` |
| `-b BASE` | base image (the 68000 program); assembled words replace its bytes 2w, 2w+1 |
| `-s BYTES` | minimum image size for `image` |
| `-l LST` | listing file |
| `-I DIR` | include directory for `.include` (after the including file's directory) |
| `-D N[=V]` | predefined espp symbol |
| `-r FIRST:END` | dasm word range, END exclusive; default 0x0400:0x10000 (the SSP's cartridge program space) |

Program memory is 64 K words. Word w is bytes 2w and 2w+1 of the cartridge image, big-endian. The SSP starts at word 0x0400 (byte 0x0800); assembly starts there unless an `org` says otherwise.

## 3 Instruction Encoding

| Encoding | Syntax |
|---|---|
| `0000 0000 dddd ssss` | `ld d, s` (`nop` = 0x0000, `ret` = 0x0065) |
| `0000 001j dddd mmpp` / `0000 010j ssss mmpp` | `ld d, (ri)` / `ld (ri), s` |
| `0000 011j aaaa aaaa` / `0000 111j aaaa aaaa` | `ld A, ramj[a]` / `ld ramj[a], A` |
| `0000 1000 dddd 0000` + imm | `ldi d, imm16` |
| `0000 101j dddd mmpp` | `ld d, ((ri))` |
| `0000 110j 0000 mmpp` + imm | `ldi (ri), imm16` |
| `0001 001j dddd mmpp` / `0001 010j ssss mmpp` | `ld d, ri` / `ld ri, s` |
| `0001 1jpp iiii iiii` | `ldi ri, imm8` |
| `0100 1010 dddd 0000` | `ld d, (A)` |
| `ooo0 0000 0000 ssss`, `ooo0 001j 0000 mmpp`, `ooo0 011j aaaa aaaa`, `ooo0 1000 0000 0000` + imm, `ooo0 101j 0000 mmpp`, `ooo1 001j 0000 mmpp`, `ooo1 1000 iiii iiii` | `OP A, s`, `OP A, (ri)`, `OP A, ramj[a]`, `OPi A, imm16`, `OP A, ((ri))`, `OP A, ri`, `OPi imm8`; ooo 1 sub, 3 cmp, 4 add, 5 and, 6 or, 7 eor |
| `0100 100f cccc 0000` + addr / `0100 110f cccc 0000` + addr | `call cond, addr` / `bra cond, addr` |
| `1001 000f cccc 0ooo` | `mod cond, ror\|rol\|shr\|shl\|inc\|dec\|neg\|abs` |
| `1001 0100 0000 ffff` | `resl setl resie setie resop setop res set` |
| `0011 011b mmjj mmii` / `1001 011b ...` / `1011 011b ...` | `mpys (rj), (ri), b` / `mpya ...` / `mld ...`; rj r4..r7, ri r0..r3, b default 1 |

Sources of the encodings: svpdoc.txt section 2.4 (Grazvydas Ignotas, in Genesis Plus GX `core/cart_hw/svp/`), MAME `src/devices/cpu/ssp1601/ssp1601d.cpp` (Pierpaolo Prazzoli, Grazvydas Ignotas, BSD-3-Clause). The MiSTer SSP160x core (Genesis_MiSTer adc0c42, GPL-3.0) decodes a subset: conditions always, z, n; mod operations shr, shl, neg, abs. The assembler accepts the full set; a program for the MiSTer core uses the subset.

## 4 Preprocessor

espp (section 6) runs before the assembler: `.include "FILE"`, `.equ NAME VALUE` (also `.define`, `.set`, `NAME = VALUE`), `.undef`, `.macro NAME P1, ...` / `.endm`, `.rept N` / `.endr`, `.ifdef`, `.ifndef`, `.if EXPR`, `.else`, `.endif`, `.error`, `.warning`. espp substitutes text; `equ` is the assembler's own symbol definition and takes an expression with labels.

## 5 Gates (`make test`)

| Gate | Check | Result (measured) |
|---|---|---|
| O | 65,536 entries (every opcode word, extension word 0x1234): `dasm` then `asm` gives the same 131,072 words; every entry decoded as an instruction has the same text as MAME's disassembler after the notation map | round trip byte-identical; 13,102 instructions equal, 0 different; 52,434 words emitted as `dw`, 19,666 of which MAME decodes (non-canonical encodings with don't-care bits set) |
| V | Virtua Racing (USA), CRC-32 7E1A324A, words 0x0400-0xFFFF: `dasm` then `asm` gives the same 122,880 bytes; text equal to MAME's at every common instruction start | round trip byte-identical; 18,845 instructions equal, 0 different |
| E | a source using `.include`, `.equ`, `.macro`, `.rept` assembles to the same words as its flat form; 8 faulty lines reported as `FILE:LINE: error` | pass |

Notation map (`tests/cmpdasm.py`): MAME prints hexadecimal without prefix, `ld d, imm` for `ldi d, imm`, the 9-bit address for `ramB[a]`, `always` for `always=1`, `RESERVED` for c1 and c11..c15, `add` for sub (its `arith_ops[1]`) and `mpya` for mpys (opcode group 0x1B). The last two are naming defects of `ssp1601d.cpp`; the gate compiles that file unchanged and maps the names.

Gate inputs: `MAME_SSP` (default `~/H3000/build/mame/src/devices/cpu/ssp1601`, MAME 0.289), `VR_ROM` (the owner's image, read in place, never copied; default `~/Genesis_Roms/Virtua Racing (USA).md`):

```
make test VR_ROM=/path/to/Virtua\ Racing\ \(USA\).md
```

## 6 Provenance

| Component | Origin | Licence | Status |
|---|---|---|---|
| `ssp16as.c`, `ssp1601_isa.c`, `ssp1601_isa.h`, `sspcompat.h`, `tests/` | Mike Wolak, EPROM Foundry Inc., project svp-lara; structure of esdsp (project dp4plusplus, Mike Wolak, EPROM Foundry Inc.) | owner's | new |
| `espp.c`, `espp.h` | esdsp preprocessor, Mike Wolak, EPROM Foundry Inc., project dp4 `de2115/tools/esdsp` | owner's | verbatim, `VENDORED.md` |
| `tests/mame_dasm.cpp`, `tests/mame_shim/emu.h` | Mike Wolak, EPROM Foundry Inc.; compile MAME's `ssp1601d.cpp` outside MAME | owner's | new |
| MAME `ssp1601d.cpp` | MAME 0.289, Pierpaolo Prazzoli, Grazvydas Ignotas | BSD-3-Clause | read in place by the gates, not copied |
