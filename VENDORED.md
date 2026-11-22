# tools/ssp16as -- vendored and ported parts

Project svp-lara, Mike Wolak, EPROM Foundry Inc.

## 1 Source

esdsp, the ES5510 assembler, disassembler and ROM tool, Mike Wolak, EPROM Foundry Inc.: the extended copy in project
dp4 (`de2115/tools/esdsp`), commit caf9a99c (directory last changed in 49cf7726).  The reference copy is project
dp4plusplus `tools/esdsp`.

| file here | source (dp4 git blob) | status |
|---|---|---|
| `espp.c` | `de2115/tools/esdsp/espp.c` 7198f5b5 | verbatim |
| `espp.h` | `de2115/tools/esdsp/espp.h` f2df5480 | verbatim |
| `sspcompat.h` | `escompat.h` d19a6197 | ported: types and big-endian byte-lane helpers reduced to the 16-bit word (rd16be, wr16be) |
| `ssp1601_isa.c`, `ssp1601_isa.h` | `es5510_isa.c` 1a4b0acd, `es5510_isa.h` 2d57dbfa | ported: the module layout (name tables, decode to canonical text, encode, one-line round trip); the encoding tables are the SSP1601's |
| `ssp16as.c` | `esdsp.c` 069f373d | ported: subcommand layout (asm, dasm, isa), espp front end with file:line provenance, listing header; options through POSIX getopt() in place of esdsp's hand-parsed argv; the rom subcommand is not carried |
| `Makefile` | `Makefile` bf6e6c7a | ported: suffix rules, `make test` gates; C99 flags |
| `tests/gate_o.sh`, `tests/gate_v.sh`, `tests/gate_e.sh` | `tests/gate_b.sh`, `tests/gate_rom.sh`, `tests/gate_pp.sh` | ported: corpus round trip, ROM round trip, preprocessor and error gate |

## 2 Gates (`make test`, measured on the commit that added this file)

| gate | result |
|---|---|
| O | 131,072 words round trip byte-identical; 13,102 instructions equal to MAME, 0 different |
| V | Virtua Racing (USA) words 0x0400-0xFFFF, 122,880 bytes round trip byte-identical; 18,845 instructions equal to MAME, 0 different |
| E | preprocessed source byte-identical to its flat form; 8 faulty lines reported as FILE:LINE |
