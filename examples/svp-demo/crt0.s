| crt0.s -- 68000 vectors, cartridge header and start-up of the SVP demonstration cartridge.
| Header layout: Sega Mega Drive cartridge header at 0x100-0x1FF.  The serial at 0x183 and the "SV" marker at 0x1C8
| are set by the Makefile (HDR_SERIAL); the checksum at 0x18E is written by tools/mkrom.py.
| Byte range 0x800-0x1FFFF is the SSP's program memory (words 0x400-0xFFFF) and stays free of 68000 code.

        .section .text.boot, "ax"
        .globl  _start
vectors:
        .long   0x00FFFE00              | initial SSP
        .long   _start                  | reset PC
        .rept   62
        .long   trap_loop
        .endr

        .ascii  "SEGA MEGA DRIVE "      | 0x100
        .ascii  "(C)EPRF 2026.NOV"      | 0x110
        .ascii  "SVP MESH ROOM                                   "   | 0x120 domestic title, 48
        .ascii  "SVP MESH ROOM                                   "   | 0x150 overseas title, 48
        .ascii  "GM "                   | 0x180
        .ascii  HDR_SERIAL              | 0x183, 8 characters
        .ascii  "-00"                   | 0x18B
        .word   0                       | 0x18E checksum
        .ascii  "J               "      | 0x190 I/O
        .long   0x00000000, 0x0003FFFF  | 0x1A0 ROM range
        .long   0x00FF0000, 0x00FFFFFF  | 0x1A8 RAM range
        .ascii  "            "          | 0x1B0 no save RAM
        .ascii  "            "          | 0x1BC modem, 12
        .ascii  "SV"                    | 0x1C8 SVP marker (Genesis Plus GX detection, SVP internal ROM check)
        .ascii  "                                      "             | 0x1CA notes, 38
        .ascii  "JUE             "      | 0x1F0 regions

        .text
_start:
        move.w  #0x2700, %sr
        move.b  0xA10001, %d0           | TMSS: write "SEGA" when the version register's low nibble is non-zero
        andi.b  #0x0F, %d0
        beq.s   1f
        move.l  #0x53454741, 0xA14000
1:      move.w  #0x0100, 0xA11100       | hold the Z80 on its bus and in reset
        move.w  #0x0000, 0xA11200
        lea     _sdata_rom, %a0         | .data to RAM, .bss cleared
        lea     _sdata, %a1
        lea     _edata, %a2
2:      cmpa.l  %a2, %a1
        bcc.s   3f
        move.b  (%a0)+, (%a1)+
        bra.s   2b
3:      lea     _ebss, %a2
4:      cmpa.l  %a2, %a1
        bcc.s   5f
        clr.b   (%a1)+
        bra.s   4b
5:      jsr     main
trap_loop:
        bra.s   trap_loop
