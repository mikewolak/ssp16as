# ssp16as -- SSP1601 (SVP) assembler and disassembler.  Project svp-lara, Mike Wolak, EPROM Foundry Inc.
# Makefile layout of esdsp (project dp4plusplus): suffix rules only, cc and libc.
#   make          ssp16as
#   make test     gate O (every opcode word against MAME's disassembler) and gate V (Virtua Racing round trip)
# Gate inputs: MAME_SSP, the directory of MAME's ssp1601d.cpp (compiled unchanged); VR_ROM, the owner's Virtua Racing
# (USA) image (2,097,152 bytes, CRC-32 7E1A324A), read in place and never copied.

CC = cc
CFLAGS = -std=c99 -O2 -Wall -Wextra -pedantic
CXX = c++
MAME_SSP = $(HOME)/H3000/build/mame/src/devices/cpu/ssp1601
VR_ROM = $(HOME)/Genesis_Roms/Virtua Racing (USA).md

OBJS = ssp16as.o ssp1601_isa.o espp.o

all: ssp16as

ssp16as: $(OBJS)
	$(CC) $(CFLAGS) -o ssp16as $(OBJS)

.c.o:
	$(CC) $(CFLAGS) -c $<

ssp16as.o: ssp16as.c ssp1601_isa.h sspcompat.h espp.h
ssp1601_isa.o: ssp1601_isa.c ssp1601_isa.h sspcompat.h
espp.o: espp.c espp.h

tests/mame_dasm: tests/mame_dasm.cpp tests/mame_shim/emu.h
	$(CXX) -std=c++17 -O2 -Wno-format-security -Itests/mame_shim -I$(MAME_SSP) -o $@ tests/mame_dasm.cpp $(MAME_SSP)/ssp1601d.cpp

test: ssp16as tests/mame_dasm
	SSP16AS=./ssp16as MAME_DASM=tests/mame_dasm sh tests/gate_o.sh
	SSP16AS=./ssp16as MAME_DASM=tests/mame_dasm VR_ROM="$(VR_ROM)" sh tests/gate_v.sh
	SSP16AS=./ssp16as sh tests/gate_e.sh

clean:
	rm -f ssp16as $(OBJS) tests/mame_dasm
	rm -rf tests/out
