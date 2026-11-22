# Makefile -- SVP demonstration cartridge: a figure rotating about Y over a scrolling perspective floor,
# assembled with ssp16as (../..).
#   make                          gen/mesh.bin (mannequin figure), gen/mesh_noroom.bin, gen/ssp16sim, gen/lrshot
#   make lara LARA_PHD=FILE       gen/lara_run.bin: the 11-pose running figure from a Tomb Raider (1996) PC level
#                                 file (LEVEL1.PHD, version 0x20); the file is read in place, never copied
#   make verify                   ssp16sim frames 1..64 of every built cartridge against mesh_model.py
#   make shots CORE=FILE          the built cartridges in a Genesis Plus GX libretro core through gen/lrshot,
#                                 screens against mesh_model.py
#   make clean                    removes gen/
SSP16AS  = ../../ssp16as
CORE     =
LARA_PHD =
GRAD     = --grad-pure
M68K     = m68k-elf-
M68KFLAGS = -m68000 -O2 -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -std=c99
B        = gen

all: $(SSP16AS) $(B)/mesh.bin $(B)/mesh_noroom.bin $(B)/ssp16sim $(B)/lrshot

lara: $(SSP16AS) $(B)/lara_run.bin $(B)/ssp16sim

$(SSP16AS):
	$(MAKE) -C ../.. ssp16as

$(B)/room/tables.inc: mkmesh.py
	python3 mkmesh.py $(B)/room $(GRAD)
$(B)/noroom/tables.inc: mkmesh.py
	python3 mkmesh.py $(B)/noroom --no-room
$(B)/run/tables.inc: mkmesh.py svp_model.py extract.py
	@test -n "$(LARA_PHD)" || { echo "make lara: LARA_PHD=FILE (Tomb Raider PC LEVEL1.PHD) required"; exit 1; }
	LARA_PHD="$(LARA_PHD)" python3 mkmesh.py $(B)/run --lara-run $(GRAD)

$(B)/crt0.o: crt0.s
	@mkdir -p $(B)
	$(M68K)gcc -m68000 -c -x assembler-with-cpp -DHDR_SERIAL='"MK-1229 "' -o $@ crt0.s

$(B)/%/main.o: main.c $(B)/%/tables.inc
	$(M68K)gcc $(M68KFLAGS) -I$(B)/$* -c -o $@ main.c

$(B)/%/m68k.bin: $(B)/crt0.o $(B)/%/main.o cart.ld
	$(M68K)gcc -m68000 -nostdlib -T cart.ld -o $(B)/$*/m68k.elf $(B)/crt0.o $(B)/$*/main.o -lgcc
	$(M68K)objcopy -O binary $(B)/$*/m68k.elf $@

$(B)/%/cart.raw: $(B)/%/m68k.bin mesh.s $(B)/%/tables.inc $(SSP16AS)
	$(SSP16AS) asm -I $(B)/$* -b $(B)/$*/m68k.bin -l $(B)/$*/mesh.lst -o $@ mesh.s

$(B)/mesh.bin: $(B)/room/cart.raw mkrom.py
	python3 mkrom.py $< $@
$(B)/mesh_noroom.bin: $(B)/noroom/cart.raw mkrom.py
	python3 mkrom.py $< $@
$(B)/lara_run.bin: $(B)/run/cart.raw mkrom.py
	python3 mkrom.py $< $@

$(B)/ssp16sim: ssp16sim.c
	@mkdir -p $(B)
	cc -std=c99 -O2 -Wall -Wextra -pedantic -o $@ ssp16sim.c

$(B)/lrshot: lrshot.c include/libretro.h
	@mkdir -p $(B)
	cc -std=c99 -O2 -Wall -Wextra -Iinclude -o $@ lrshot.c

verify: $(B)/ssp16sim
	@for r in mesh:room mesh_noroom:noroom lara_run:run; do \
	  c=$${r%%:*}; j=$${r##*:}; \
	  if [ -f $(B)/$$c.bin ]; then python3 verify.py $(B)/$$c.bin $(B)/$$j/mesh.json | tail -1; fi; \
	done

shots: $(B)/lrshot
	@test -n "$(CORE)" || { echo "make shots: CORE=FILE (Genesis Plus GX libretro core) required"; exit 1; }
	@for r in mesh:room lara_run:run; do \
	  c=$${r%%:*}; j=$${r##*:}; \
	  if [ -f $(B)/$$c.bin ]; then mkdir -p $(B)/shots_$$c; \
	    $(B)/lrshot "$(CORE)" $(B)/$$c.bin 266 $(B)/shots_$$c/s $$(seq 121 4 265) > /dev/null; \
	    python3 verify.py $(B)/$$c.bin $(B)/$$j/mesh.json --gpgx $(B)/shots_$$c/s | tail -1; fi; \
	done

clean:
	rm -rf gen

.PRECIOUS: $(B)/%/main.o $(B)/%/m68k.bin $(B)/%/cart.raw
.PHONY: all lara verify shots clean
