| crt0.s -- 68000 vectors, cartridge header and start-up of the SVP demonstration cartridge.
| Header layout: Sega Mega Drive cartridge header at 0x100-0x1FF.  The serial at 0x183 and the "SV" marker at 0x1C8
| are set by the Makefile (HDR_SERIAL); the checksum at 0x18E is written by tools/mkrom.py.
| Byte range 0x800-0x1FFFF is the SSP's program memory (words 0x400-0xFFFF) and stays free of 68000 code.

        .section .text.boot, "ax"
        .globl  _start
vectors:
        .long   0x00FFFE00              | initial SSP
        .long   _start                  | reset PC
        .rept   26
        .long   trap_loop
        .endr
        .long   hint                    | 0x70: level 4 autovector, VDP H-interrupt
        .long   trap_loop               | 0x74
        .long   vint                    | 0x78: level 6 autovector, VDP V-interrupt
        .rept   33
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

| H-interrupt (VDP register 10 = 0: once a line).  The CRAM data write is placed in horizontal blank, never in active
| display (CRAM writes there show as dots on the hardware): the handler waits for the active part of a line (H counter
| < 0x80), reads its V, sets the CRAM write address of entry 13 (byte 0x1A; an address write shows nothing), waits
| for horizontal blank (VDP status bit 2, HBLANK), then writes floor_grad[V + 1], the colour of the next line.  V == 207 (the last viewport line) and dma_armed: in its blank, display off (register 1 = 0x34, V-interrupt
| kept), blank_flag = 1; lines 208..261 are then blank for the 68000's DMA.  V > 207: nothing.
        .globl  hint, vint
hint:
        move.l  %d0, -(%sp)
        move.l  %d1, -(%sp)
        move.l  %a0, -(%sp)
        move.l  %a1, -(%sp)
        lea     0xC00004, %a0           | VDP control / status
1:      move.b  0xC00009, %d0           | H counter: wait for the active part of a line
        cmpi.b  #0x80, %d0
        bcc.s   1b
        moveq   #0, %d0
        move.b  0xC00008, %d0           | V of that line
        cmpi.w  #207, %d0
        bcc.s   8f
        add.w   %d0, %d0
        lea     floor_grad + 2, %a1
        move.w  0(%a1, %d0.w), %d1      | colour of line V + 1
        move.l  #0xC01A0000, (%a0)      | CRAM write address, entry 13
2:      btst    #2, 1(%a0)              | wait for horizontal blank: status bit 2 (HBLANK), 26-cycle loop
        beq.s   2b
        move.w  %d1, -4(%a0)            | CRAM data write, 12 cycles after the flag is seen
#ifdef HINT_DEBUG
        move.w  (%a0), %d1              | status 12 cycles after the write: bit 2 set = the write fell inside HBLANK
        movea.l hint_dbg, %a1
        move.w  0xC00008, (%a1)+        | HV counter after that
        move.w  %d1, (%a1)+
        move.l  %a1, hint_dbg
#endif
        bra.s   9f
8:      bne.s   9f                      | V > 207: nothing
        tst.w   dma_armed
        beq.s   9f
3:      btst    #2, 1(%a0)              | V == 207: display off in its horizontal blank
        beq.s   3b
        move.w  #0x8134, (%a0)
        move.w  #1, blank_flag
9:      movea.l (%sp)+, %a1
        movea.l (%sp)+, %a0
        move.l  (%sp)+, %d1
        move.l  (%sp)+, %d0
        rte

| V-interrupt (line 224): vertical blank count; CRAM entry 13 = floor_grad[0] for line 0.
vint:
        addq.l  #1, vbl_total
        move.l  #0xC01A0000, 0xC00004
        move.w  floor_grad, 0xC00000
#ifdef HINT_DEBUG
        move.l  #0xFF8100, hint_dbg
#endif
        rte
