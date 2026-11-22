; mesh.s -- SSP1601 program of the SVP mesh cartridge: a triangle renderer (clear, rotation about Y, tilt about X, perspective by reciprocal table, area cull, 8-level light, top-left-rule
; triangle fill through PM4 over-write) extended to a mesh of parts.  Per command from the 68000 it clears one of two
; frame buffers, draws the static parts (the room) at angle 0 in table order, then the dynamic parts (the figure)
; at angle 4k far to near by the depth of each part's transformed centre (selection sort, ties by part index).
; Arithmetic: mkmesh.py / mesh_model.py (bit-exact model).
;
; Frame buffer: 256 x 192 pixels, 4 bpp, linear rows of 128 DRAM words; buffer b (frame number k & 1) at word offset
; b * 64 of each row; DRAM word address 0x180000 + y * 128 + b * 64 + x / 4, the left pixel in bits 15..12.
;
; Handshake (svpdoc.txt 2.2.1, 2.2.4): the 68000 writes k (1..0xFFFF) to XST (0xA15000); PM0 bit 1 reports the
; write; the SSP renders frame k and writes k back to XST, which sets bit 0 of 0xA15004.
;
; Instruction subset: the MiSTer SSP160x core's (conditions always, z, n; mod shr, shl, neg, abs).  ST stays 0.
; Z is tested only where the low word of A is 0.
;
; RAM0 (r0..r3, direct ram0[]):
K       equ 0x10        ; frame number from the 68000
BUFOFF  equ 0x13        ; (k & 1) * 64
ROWB    equ 0x14        ; DRAM word offset of the current row: y * 128 + BUFOFF
YROW    equ 0x15        ; current row
N0      equ 0x16        ; first word of the span
CNT     equ 0x17        ; last word - first word of the span
SEGN    equ 0x18        ; rows left in the segment
FCNT    equ 0x1A        ; faces left - 1
VCNT    equ 0x1B        ; vertices left - 1
MAT     equ 0x1C        ; material * 8 of the face
ANG     equ 0x1D        ; angle index
TMP     equ 0x1E
TMP2    equ 0x1F
QX      equ 0x30        ; 0x30..0x33: quad x
QY      equ 0x34        ; 0x34..0x37: quad y
STREAM  equ 0x3F        ; program-memory pointer read with ((r2))
TX0     equ 0x40        ; triangle input: TX0 TY0 TX1 TY1 TX2 TY2
TY0     equ 0x41
TX1     equ 0x42
TY1     equ 0x43
TX2     equ 0x44
TY2     equ 0x45
XT      equ 0x46        ; sorted by y: top, middle, bottom
YT      equ 0x47
XM      equ 0x48
YM      equ 0x49
XB      equ 0x4A
YB      equ 0x4B
UX      equ 0x4C        ; edge set-up arguments: upper and lower vertex
UY      equ 0x4D
LX      equ 0x4E
LY      equ 0x4F
EL      equ 0x50        ; left edge: e, 2h (x step), -2w (row step); x in r0
SL      equ 0x51
RL      equ 0x52
ER      equ 0x53        ; right edge: e - 1, 2h, 2w; x in r4
SR      equ 0x54
RR      equ 0x55
VADR    equ 0x56        ; part: VERTS address
FADR    equ 0x57        ; part: FACES address
PART    equ 0x58        ; part record address
PCNT    equ 0x59        ; parts left - 1
BEST    equ 0x5A        ; selection: smallest key so far
BIDX    equ 0x5B        ; selection: its RAM1 address
SCNT    equ 0x5C        ; selection: keys left - 1
POSE    equ 0x5D        ; pose index: k mod NPOSE
DBASE   equ 0x5E        ; address of the pose's first dynamic part record
LX0     equ 0x20        ; treadmill: current x-line centre
LZ0     equ 0x21        ; treadmill: current z-line centre
LCNT    equ 0x22        ; treadmill: front-to-back lines left
LCNT2   equ 0x23        ; treadmill: far cross-lines left
TT      equ 0x24        ; treadmill: cross-line half thickness
LZN     equ 0x25        ; treadmill: first near cross-line
LCNT3   equ 0x26        ; treadmill: near cross-lines left
ZC      equ 0x27        ; xline: near z
HN      equ 0x28        ; xline: near half width
XW      equ 0x29        ; zline: half extent
ZIDX    equ 0x2A        ; zline: table index
COLW    equ 0x2B        ; floor colour word
TMP2Z   equ 0x2C        ; saved first far cross-line
REM     equ 0x2D        ; xline: rows left
BROWS   equ 0x2E        ; xline: rows of the current depth band
PAIRS   equ 0x60        ; 0x60..0xFF: sx, sy of part vertex i at 0x60 + 2i (80 vertices a part)
; RAM0[0..3] (r3|00..|11): scratch; RAM0[3] holds the colour word while a face is filled.
; RAM1[0..3] (r7|00..|11): cos, sin of the angle in use, cos, sin of the tilt; RAM1[4], [5]: cos, sin of frame k.
; RAM1[0x10..]: depth keys of the dynamic parts.  RAM1[0x80..0x83]: left masks, RAM1[0x84..0x87]: right masks,
; RAM1[0x88..0x8B]: one-pixel masks.
FCOS    equ 0x04
FSIN    equ 0x05
KEYS    equ 0x10
LMASK   equ 0x80
RMASK   equ 0x84
PMASK   equ 0x88        ; RAM1[0x88..0x8B]: one-pixel masks
KEY_MAX equ 0x3FFF      ; marks a drawn part; above every key (|zt| < 0x3FFF)
MODE_FILL  equ 0x0C18   ; PMC mode: DRAM (address bits 20..16 = 0x18), increment 1, over-write
MODE_CLEAR equ 0x0818   ; PMC mode: DRAM, increment 1

        org 0x0400
start:
        ldi ST, 0x0000
        ldi r4, LMASK
        ldi (r4+!), 0xFFFF
        ldi (r4+!), 0x0FFF
        ldi (r4+!), 0x00FF
        ldi (r4+!), 0x000F
        ldi (r4+!), 0xF000
        ldi (r4+!), 0xFF00
        ldi (r4+!), 0xFFF0
        ldi (r4+!), 0xFFFF
        ldi (r4+!), 0xF000
        ldi (r4+!), 0x0F00
        ldi (r4+!), 0x00F0
        ldi (r4+!), 0x000F
        ldi X, TILT_C
        ld (r7|10), X
        ldi X, TILT_S
        ld (r7|11), X
        ldi r2, STREAM

; ---------------------------------------------------------------- command
wait_cmd:
        ld A, EXT0
        andi 0x02
        bra z=1, wait_cmd
        ld A, EXT3
        ld ram0[K], A
; pose = k mod NPOSE (k & 0x7FFF; NPOSE * 64 steps, then NPOSE steps); DBASE = POSEBASE[pose]
        andi A, 0x7FFF
pose_big:
        subi A, POSEBIG
        bra n=0, pose_big
        addi A, POSEBIG
pose_small:
        subi A, NPOSE
        bra n=0, pose_small
        addi A, NPOSE
        ld ram0[POSE], A
        addi A, POSEBASE
        ld X, (A)
        ld A, X
        ld ram0[DBASE], A
        ld A, ram0[K]
; angle (4k) & 255, sin and cos of frame k to RAM1[FSIN], RAM1[FCOS]
        andi A, 0x003F
        mod always, shl
        mod always, shl
        ld ram0[ANG], A
        addi A, SIN
        ld X, (A)
        ld A, X
        ld ram1[FSIN], A
        ld A, ram0[ANG]
        addi 0x40
        andi A, 0x00FF
        addi A, SIN
        ld X, (A)
        ld A, X
        ld ram1[FCOS], A
; buffer offset (k & 1) * 64
        ld A, ram0[K]
        andi 0x01
        mod always, shl
        mod always, shl
        mod always, shl
        mod always, shl
        mod always, shl
        mod always, shl
        ld ram0[BUFOFF], A

; ---------------------------------------------------------------- clear 192 rows x 64 words
        ld ram0[ROWB], A
        ldi A, 191
        ld ram0[YROW], A
        ldi Y, 0x0000
clr_row:
        ld A, ram0[ROWB]
        ld -, AL
        ld EXT6, A
        ldi EXT6, MODE_CLEAR
        ld EXT4, -
        ld EXT4, Y
        ldi STACK, clr_next
        ldi A, FILL
        ld PC, A
clr_next:
        ld A, ram0[ROWB]
        addi 0x80
        ld ram0[ROWB], A
        ld A, ram0[YROW]
        subi 0x01
        ld ram0[YROW], A
        bra n=0, clr_row

; ---------------------------------------------------------------- static parts at angle 0 (c = SIN[64], s = SIN[0])
        ldi X, 0x7FFF
        ld (r7|00), X
        ldi X, 0x0000
        ld (r7|01), X
        ldi A, PARTS
        ld ram0[PART], A
        ldi A, NSTATIC
        subi 0x01
        ld ram0[PCNT], A
        bra n=1, st_done
st_loop:
        call always, draw_part
        ld A, ram0[PART]
        addi 0x08
        ld ram0[PART], A
        ld A, ram0[PCNT]
        subi 0x01
        ld ram0[PCNT], A
        bra n=0, st_loop
st_done:

; ---------------------------------------------------------------- treadmill floor (static angle still in RAM1[0], [1])
; mkmesh.py treadmill(): TMTAB[k & 63] (8 words) = first front-to-back line x, count, first far cross-line z (step 2
; cells), count, first near cross-line z (step 1 cell), count.  Optional horizon cross-line at TM_ZF in HORW.
        ldi A, TREAD
        subi 0x01
        bra n=1, tm_done
        ld A, ram0[K]
        andi A, 0x003F
        mod always, shl
        mod always, shl
        mod always, shl
        addi A, TMTAB
        ld ram0[STREAM], A
        ld A, ((r2))
        ld ram0[LX0], A
        ld A, ((r2))
        ld ram0[LCNT], A
        ld A, ((r2))
        ld ram0[LZ0], A
        ld A, ((r2))
        ld ram0[LCNT2], A
        ld A, ((r2))
        ld ram0[LZN], A
        ld A, ((r2))
        ld ram0[LCNT3], A
        ldi A, TM_HOR
        subi 0x01
        bra n=1, tm_x
        ld A, ram0[LZ0]
        ld ram0[TMP2Z], A
        ldi A, TM_ZF
        ld ram0[LZ0], A
        call always, zline
        ld A, ram0[TMP2Z]
        ld ram0[LZ0], A
tm_x:
        ldi A, FLOORW
        ld ram0[COLW], A
tm_xl:
        ld A, ram0[LCNT]
        subi 0x01
        bra n=1, tm_zf
        ld ram0[LCNT], A
        call always, xline
        ld A, ram0[LX0]
        addi A, TM_CELL
        ld ram0[LX0], A
        bra always, tm_xl
tm_zf:
        ld A, ram0[LCNT2]
        subi 0x01
        bra n=1, tm_zn
        ld ram0[LCNT2], A
        call always, zline
        ld A, ram0[LZ0]
        addi A, 2 * TM_CELL
        ld ram0[LZ0], A
        bra always, tm_zf
tm_zn:
        ld A, ram0[LZN]
        ld ram0[LZ0], A
tm_znl:
        ld A, ram0[LCNT3]
        subi 0x01
        bra n=1, tm_done
        ld ram0[LCNT3], A
        call always, zline
        ld A, ram0[LZ0]
        addi A, TM_CELL
        ld ram0[LZ0], A
        bra always, tm_znl
tm_done:

; ---------------------------------------------------------------- dynamic parts: keys = zt of the part centres
        ld A, ram1[FCOS]
        ld (r7|00), A
        ld A, ram1[FSIN]
        ld (r7|01), A
        ld A, ram0[DBASE]
        addi 0x04
        ld ram0[PART], A
        ldi r4, KEYS
        ldi A, NDYN - 1
        ld ram0[PCNT], A
key_loop:
        ld A, ram0[PART]
        ld ram0[STREAM], A
        ld X, ((r2))
        ld (r3|00), X
        ld X, ((r2))
        ld (r3|01), X
        ld X, ((r2))
        ld (r3|10), X
        call always, xform
        ld A, (r3|01)
        ld (r4+!), A
        ld A, ram0[PART]
        addi 0x08
        ld ram0[PART], A
        ld A, ram0[PCNT]
        subi 0x01
        ld ram0[PCNT], A
        bra n=0, key_loop

; selection: NDYN times the smallest key (first of equal keys), drawn and marked KEY_MAX
        ldi A, NDYN - 1
        ld ram0[PCNT], A
sel_loop:
        ldi A, KEY_MAX
        ld ram0[BEST], A
        ldi A, KEYS
        ld ram0[BIDX], A
        ldi r4, KEYS
        ldi A, NDYN - 1
        ld ram0[SCNT], A
scan:
        ld A, (r4)
        sub A, ram0[BEST]
        bra n=0, scan_next
        ld A, (r4)
        ld ram0[BEST], A
        ld A, r4
        ld ram0[BIDX], A
scan_next:
        ld -, (r4+!)
        ld A, ram0[SCNT]
        subi 0x01
        ld ram0[SCNT], A
        bra n=0, scan
        ld A, ram0[BIDX]
        ld r4, A
        ldi A, KEY_MAX
        ld (r4), A
        ld A, ram0[BIDX]
        andi A, 0xFFFF
        subi A, KEYS
        mod always, shl
        mod always, shl
        mod always, shl
        add A, ram0[DBASE]
        ld ram0[PART], A
        call always, draw_part
        ld A, ram0[PCNT]
        subi 0x01
        ld ram0[PCNT], A
        bra n=0, sel_loop

; ---------------------------------------------------------------- done
        ld A, ram0[K]
        ld EXT3, A
        bra always, wait_cmd

; ================================================================ transform
; In: x, y, z in RAM0[0..2]; angle cos, sin in RAM1[0], [1]; tilt in RAM1[2], [3].
; Out: xv in RAM0[3], yt in RAM0[0], zt in RAM0[1].
xform:
        ld X, (r3|00)           ; xv = hi(2xc + 2zs)
        ld Y, (r7|00)
        ld A, P
        ld X, (r3|10)
        ld Y, (r7|01)
        add A, P
        ld (r3|11), A
        ld X, (r3|10)           ; z2 = hi(2zc - 2xs)
        ld Y, (r7|00)
        ld A, P
        ld X, (r3|00)
        ld Y, (r7|01)
        sub A, P
        ld (r3|10), A
        ld X, (r3|01)           ; yt = hi(2y ct - 2 z2 st)
        ld Y, (r7|10)
        ld A, P
        ld X, (r3|10)
        ld Y, (r7|11)
        sub A, P
        ld (r3|00), A
        ld X, (r3|01)           ; zt = hi(2y st + 2 z2 ct)
        ld Y, (r7|11)
        ld A, P
        ld X, (r3|10)
        ld Y, (r7|10)
        add A, P
        ld (r3|01), A
        ret

; ================================================================ floor lines
; xline: front-to-back line at x = LX0 from TM_ZF to ZCT[|x| >> 5], one pixel a row, coloured by depth band
; (FBAND row runs).  zline: colour CLT[(z - TM_ZF) >> 5].
; zline: cross-line at z = LZ0, half thickness THK[(z - TM_ZF) >> 5], half extent XWT[..].  Colour COLW.
xline:
        ld A, ram0[LX0]          ; zc = ZCT[|x| >> 5]
        andi A, 0xFFFF
        mod always, abs
        mod always, shr
        mod always, shr
        mod always, shr
        mod always, shr
        mod always, shr
        addi A, ZCT
        ld X, (A)
        ld A, X
        ld ram0[ZC], A
        ldi r0, PAIRS
        ld A, ram0[LX0]
        ld (r3|00), A
        ldi A, TM_FY
        ld (r3|01), A
        ldi A, TM_ZF
        ld (r3|10), A
        call always, project
        ld A, ram0[LX0]
        ld (r3|00), A
        ldi A, TM_FY
        ld (r3|01), A
        ld A, ram0[ZC]
        ld (r3|10), A
        call always, project
; one pixel a row at the left-edge position of the edge (sx0, sy0) - (sx1, sy1) (setl and seg's left-edge step),
; rows sy0 .. sy1 - 1 (all inside 0..191 by construction of the floor tables)
        ld A, ram0[PAIRS + 1]
        andi A, 0xFFFF
        mod always, shl
        mod always, shl
        mod always, shl
        mod always, shl
        mod always, shl
        mod always, shl
        mod always, shl
        add A, ram0[BUFOFF]
        ld ram0[ROWB], A
        ld A, ram0[PAIRS]
        ld ram0[UX], A
        ld A, ram0[PAIRS + 1]
        ld ram0[UY], A
        ld A, ram0[PAIRS + 2]
        ld ram0[LX], A
        ld A, ram0[PAIRS + 3]
        ld ram0[LY], A
        call always, setl
        ld A, ram0[PAIRS + 3]    ; rows left
        sub A, ram0[PAIRS + 1]
        ld ram0[REM], A
        ldi A, FBAND             ; depth bands far to near: colour word, rows (last 0x7FFF)
        ld ram0[STREAM], A
eb_loop:
        ld A, ((r2))
        ld (r3|11), A
        ld A, ((r2))
        ld ram0[BROWS], A
        ld A, ram0[REM]
        sub A, ram0[BROWS]
        bra n=1, eb_last
        ld ram0[REM], A
        ld A, ram0[BROWS]
        ld ram0[SEGN], A
        call always, el_row
        bra always, eb_loop
eb_last:
        ld A, ram0[REM]
        ld ram0[SEGN], A
        call always, el_row
        ret
el_row:
        ld A, ram0[SEGN]
        subi 0x01
        bra n=1, el_end
        ld ram0[SEGN], A
        ld A, ram0[EL]
        add A, ram0[RL]
        bra n=1, el_inc
el_dec:
        sub A, ram0[SL]
        bra n=1, el_undo
        ld -, (r0-)
        bra always, el_dec
el_undo:
        add A, ram0[SL]
        bra always, el_done
el_inc:
        add A, ram0[SL]
        ld -, (r0+!)
        bra n=1, el_inc
el_done:
        ld ram0[EL], A
        ld A, r0                 ; word ROWB + x / 4, nibble mask PMASK[x & 3]
        andi 0x03
        addi PMASK
        ld r5, A
        ld A, r0
        mod always, shr
        mod always, shr
        add A, ram0[ROWB]
        ld -, AL
        ld EXT6, A
        ldi EXT6, MODE_FILL
        ld EXT4, -
        ld A, (r3|11)
        and A, (r5)
        ld EXT4, A
        ld A, ram0[ROWB]
        addi 0x80
        ld ram0[ROWB], A
        bra always, el_row
el_end:
        ret

zline:
        ld A, ram0[LZ0]          ; t = THK[(z - ZF) >> 5], w = XWT[(z - ZF) >> 5]
        subi A, TM_ZF
        mod always, shr
        mod always, shr
        mod always, shr
        mod always, shr
        mod always, shr
        ld ram0[ZIDX], A
        addi A, CLT              ; colour word of the line's depth band
        ld X, (A)
        ld A, X
        ld ram0[COLW], A
        ld A, ram0[ZIDX]
        addi A, THK
        ld X, (A)
        ld A, X
        ld ram0[TT], A
        ld A, ram0[ZIDX]
        addi A, XWT
        ld X, (A)
        ld A, X
        ld ram0[XW], A
        ldi r0, PAIRS
        ldi A, 0x0000
        sub A, ram0[XW]
        ld (r3|00), A
        ldi A, TM_FY
        ld (r3|01), A
        ld A, ram0[LZ0]
        sub A, ram0[TT]
        ld (r3|10), A
        call always, project
        ld A, ram0[XW]
        ld (r3|00), A
        ldi A, TM_FY
        ld (r3|01), A
        ld A, ram0[LZ0]
        sub A, ram0[TT]
        ld (r3|10), A
        call always, project
        ld A, ram0[XW]
        ld (r3|00), A
        ldi A, TM_FY
        ld (r3|01), A
        ld A, ram0[LZ0]
        add A, ram0[TT]
        ld (r3|10), A
        call always, project
        ldi A, 0x0000
        sub A, ram0[XW]
        ld (r3|00), A
        ldi A, TM_FY
        ld (r3|01), A
        ld A, ram0[LZ0]
        add A, ram0[TT]
        ld (r3|10), A
        call always, project
        call always, quad_fill
        ret

; ================================================================ project one vertex
; In: x, y, z in RAM0[0..2], r0 -> PAIRS slot.  Out: sx, sy at (r0+!), (r0+!) (the vertex loop's arithmetic).
project:
        call always, xform
        ldi A, CAM_D
        sub A, (r3|01)
        mod always, shr
        mod always, shr
        mod always, shr
        mod always, shr
        addi A, RECIP
        ld Y, (A)
        ld X, (r3|11)
        ld A, P
        ld (r3|11), A
        ldi A, 128
        sub A, (r3|11)
        ld (r0+!), A
        ld X, (r3|00)
        ld A, P
        addi SCR_CY
        ld (r0+!), A
        ret

; ================================================================ floor quad: trapezoid fill
; In: PAIRS[0..7] = v0..v3 (sx, sy); v0, v1 on the top row, v2, v3 on the bottom row (a floor line's far and near
; ends project to one row each); colour word COLW.  One pass over the rows with the edges v0-v3 and v1-v2 (the
; tri routine's edge set-up and rows): the same pixels as the triangles (0,1,2), (0,2,3) under the top-left rule,
; whose shared diagonal gives each pixel to exactly one of them, at half the row work.
quad_fill:
        ld A, ram0[COLW]
        ld (r3|11), A
        ld A, ram0[PAIRS + 1]         ; top row: ROWB = y * 128 + BUFOFF
        ld ram0[YROW], A
        andi A, 0xFFFF
        mod always, shl
        mod always, shl
        mod always, shl
        mod always, shl
        mod always, shl
        mod always, shl
        mod always, shl
        add A, ram0[BUFOFF]
        ld ram0[ROWB], A
        ld A, ram0[PAIRS]             ; (sx0 - sx1) + (sx3 - sx2) < 0: v0 - v3 is the left edge
        sub A, ram0[PAIRS + 2]
        add A, ram0[PAIRS + 6]
        sub A, ram0[PAIRS + 4]
        bra n=1, qf_l03
        ld A, ram0[PAIRS + 2]
        ld ram0[UX], A
        ld A, ram0[PAIRS + 3]
        ld ram0[UY], A
        ld A, ram0[PAIRS + 4]
        ld ram0[LX], A
        ld A, ram0[PAIRS + 5]
        ld ram0[LY], A
        call always, setl
        ld A, ram0[PAIRS]
        ld ram0[UX], A
        ld A, ram0[PAIRS + 1]
        ld ram0[UY], A
        ld A, ram0[PAIRS + 6]
        ld ram0[LX], A
        ld A, ram0[PAIRS + 7]
        ld ram0[LY], A
        call always, setr
        bra always, qf_seg
qf_l03:
        ld A, ram0[PAIRS]
        ld ram0[UX], A
        ld A, ram0[PAIRS + 1]
        ld ram0[UY], A
        ld A, ram0[PAIRS + 6]
        ld ram0[LX], A
        ld A, ram0[PAIRS + 7]
        ld ram0[LY], A
        call always, setl
        ld A, ram0[PAIRS + 2]
        ld ram0[UX], A
        ld A, ram0[PAIRS + 3]
        ld ram0[UY], A
        ld A, ram0[PAIRS + 4]
        ld ram0[LX], A
        ld A, ram0[PAIRS + 5]
        ld ram0[LY], A
        call always, setr
qf_seg:
        ld A, ram0[PAIRS + 5]
        sub A, ram0[PAIRS + 1]
        ld ram0[SEGN], A
        call always, seg
        ret

; ================================================================ one part
; In: PART = address of the part record (VERTS address, nv - 1, FACES address, nf - 1, cx, cy, cz, 0).
draw_part:
        ld A, ram0[PART]
        ld ram0[STREAM], A
        ld A, ((r2))
        ld ram0[VADR], A
        ld A, ((r2))
        ld ram0[VCNT], A
        ld A, ((r2))
        ld ram0[FADR], A
        ld A, ((r2))
        ld ram0[FCNT], A
        ld A, ram0[VADR]
        ld ram0[STREAM], A
        ldi r0, PAIRS
vloop:
        ld X, ((r2))
        ld (r3|00), X           ; x
        ld X, ((r2))
        ld (r3|01), X           ; y
        ld X, ((r2))
        ld (r3|10), X           ; z
        call always, xform
        ldi A, CAM_D            ; R = RECIP[(D - zt) >> 4]
        sub A, (r3|01)
        mod always, shr
        mod always, shr
        mod always, shr
        mod always, shr
        addi A, RECIP
        ld Y, (A)
        ld X, (r3|11)           ; sx = 128 - hi(2 xv R)
        ld A, P
        ld (r3|11), A
        ldi A, 128
        sub A, (r3|11)
        ld (r0+!), A
        ld X, (r3|00)           ; sy = SCR_CY + hi(2 yt R)
        ld A, P
        addi SCR_CY
        ld (r0+!), A
        ld A, ram0[VCNT]
        subi 0x01
        ld ram0[VCNT], A
        bra n=0, vloop

        ld A, ram0[FADR]
        ld ram0[STREAM], A
floop:
        ld A, ((r2))            ; four vertex offsets (2i), each to its sx, sy
        addi PAIRS
        ld r0, A
        ld A, (r0+!)
        ld ram0[QX], A
        ld A, (r0)
        ld ram0[QY], A
        ld A, ((r2))
        addi PAIRS
        ld r0, A
        ld A, (r0+!)
        ld ram0[QX + 1], A
        ld A, (r0)
        ld ram0[QY + 1], A
        ld A, ((r2))
        addi PAIRS
        ld r0, A
        ld A, (r0+!)
        ld ram0[QX + 2], A
        ld A, (r0)
        ld ram0[QY + 2], A
        ld A, ((r2))
        addi PAIRS
        ld r0, A
        ld A, (r0+!)
        ld ram0[QX + 3], A
        ld A, (r0)
        ld ram0[QY + 3], A
; twice the area: (x2 - x0)(y3 - y1) - (x3 - x1)(y2 - y0); front when negative (a triangle repeats vertex 2)
        ld A, ram0[QX + 2]
        sub A, ram0[QX]
        ld (r3|00), A
        ld A, ram0[QY + 3]
        sub A, ram0[QY + 1]
        ld (r3|01), A
        ld A, ram0[QX + 3]
        sub A, ram0[QX + 1]
        ld (r3|10), A
        ld A, ram0[QY + 2]
        sub A, ram0[QY]
        ld (r3|11), A
        ld X, (r3|00)
        ld Y, (r3|01)
        ld A, P
        ld X, (r3|10)
        ld Y, (r3|11)
        sub A, P
        bra n=1, front
        ld A, ram0[STREAM]      ; back face: skip normal and material
        addi 0x04
        ld ram0[STREAM], A
        bra always, fnext
front:
        ld X, ((r2))
        ld (r3|00), X           ; nx
        ld X, ((r2))
        ld (r3|01), X           ; ny
        ld X, ((r2))
        ld (r3|10), X           ; nz
        ld A, ((r2))
        ld ram0[MAT], A
        call always, xform      ; nxv, nyt, nzt in RAM0[3], [0], [1]
        ld X, (r3|11)           ; d = hi(2 nxv Lx + 2 nyt Ly + 2 nzt Lz)
        ldi Y, LIGHT_X
        ld A, P
        ld X, (r3|00)
        ldi Y, LIGHT_Y
        add A, P
        ld X, (r3|01)
        ldi Y, LIGHT_Z
        add A, P
        mod always, shr         ; level = d >> 12, 0 when negative
        mod always, shr
        mod always, shr
        mod always, shr
        mod always, shr
        mod always, shr
        mod always, shr
        mod always, shr
        mod always, shr
        mod always, shr
        mod always, shr
        mod always, shr
        bra n=0, lev_ok
        andi 0x00
lev_ok:
        add A, ram0[MAT]
        addi A, LUT
        ld X, (A)
        ld (r3|11), X           ; colour word for the fill
; triangles (0, 1, 2) and, unless vertex 3 projects onto vertex 2, (0, 2, 3)
        ld A, ram0[QX]
        ld ram0[TX0], A
        ld A, ram0[QY]
        ld ram0[TY0], A
        ld A, ram0[QX + 1]
        ld ram0[TX1], A
        ld A, ram0[QY + 1]
        ld ram0[TY1], A
        ld A, ram0[QX + 2]
        ld ram0[TX2], A
        ld A, ram0[QY + 2]
        ld ram0[TY2], A
        call always, tri
        ld A, ram0[QX + 3]
        andi A, 0xFFFF
        sub A, ram0[QX + 2]
        bra z=0, tri2
        ld A, ram0[QY + 3]
        andi A, 0xFFFF
        sub A, ram0[QY + 2]
        bra z=1, fnext
tri2:
        ld A, ram0[QX]
        ld ram0[TX0], A
        ld A, ram0[QY]
        ld ram0[TY0], A
        ld A, ram0[QX + 2]
        ld ram0[TX1], A
        ld A, ram0[QY + 2]
        ld ram0[TY1], A
        ld A, ram0[QX + 3]
        ld ram0[TX2], A
        ld A, ram0[QY + 3]
        ld ram0[TY2], A
        call always, tri
fnext:
        ld A, ram0[FCNT]
        subi 0x01
        ld ram0[FCNT], A
        bra n=0, floop
        ret

; ================================================================ triangle fill
; In: TX0..TY2, colour word in RAM0[3], BUFOFF.  Rows y with 2y+1 strictly between the top and bottom vertex rows
; (doubled) are filled; columns by the left edge (inclusive) and the right edge (exclusive) at the pixel centre.
; Projected x must stay within 0..255 (the edge x are 8-bit pointer registers r0 and r4).
tri:
        ld A, ram0[TX0]
        ld ram0[XT], A
        ld A, ram0[TY0]
        ld ram0[YT], A
        ld A, ram0[TX1]
        ld ram0[XM], A
        ld A, ram0[TY1]
        ld ram0[YM], A
        ld A, ram0[TX2]
        ld ram0[XB], A
        ld A, ram0[TY2]
        ld ram0[YB], A
        call always, sort_tm
        ld A, ram0[YB]          ; middle / bottom
        sub A, ram0[YM]
        bra n=0, srt2
        ld A, ram0[XM]
        ld X, A
        ld A, ram0[XB]
        ld ram0[XM], A
        ld A, X
        ld ram0[XB], A
        ld A, ram0[YM]
        ld X, A
        ld A, ram0[YB]
        ld ram0[YM], A
        ld A, X
        ld ram0[YB], A
srt2:
        call always, sort_tm
; -cross = (ym - yt)(xb - xt) - (xm - xt)(yb - yt); zero: nothing to draw; negative: the long edge is the left one
        ld A, ram0[XM]
        sub A, ram0[XT]
        ld (r3|00), A
        ld A, ram0[YB]
        sub A, ram0[YT]
        ld (r3|01), A
        ld A, ram0[YM]
        sub A, ram0[YT]
        ld (r3|10), A
        ld A, ram0[XB]
        sub A, ram0[XT]
        ld Y, A
        ld X, (r3|10)
        ld A, P
        ld X, (r3|00)
        ld Y, (r3|01)
        sub A, P
        bra z=1, tri_ret
        ld ram0[TMP2], A        ; sign of -cross in bit 15
; row base of the top row: yt * 128 + BUFOFF
        ld A, ram0[YT]
        ld ram0[YROW], A
        andi A, 0xFFFF
        mod always, shl
        mod always, shl
        mod always, shl
        mod always, shl
        mod always, shl
        mod always, shl
        mod always, shl
        add A, ram0[BUFOFF]
        ld ram0[ROWB], A
; long edge top -> bottom
        call always, arg_tb
        ld A, ram0[TMP2]
        andi A, 0x8000
        bra z=1, long_right
        call always, setl
        call always, arg_tm
        call always, setr
        call always, seg_tm
        call always, arg_mb
        call always, setr
        call always, seg_mb
        ret
long_right:
        call always, setr
        call always, arg_tm
        call always, setl
        call always, seg_tm
        call always, arg_mb
        call always, setl
        call always, seg_mb
tri_ret:
        ret

; swap top and middle when the middle is above
sort_tm:
        ld A, ram0[YM]
        sub A, ram0[YT]
        bra n=0, sort_ret
        ld A, ram0[XT]
        ld X, A
        ld A, ram0[XM]
        ld ram0[XT], A
        ld A, X
        ld ram0[XM], A
        ld A, ram0[YT]
        ld X, A
        ld A, ram0[YM]
        ld ram0[YT], A
        ld A, X
        ld ram0[YM], A
sort_ret:
        ret

arg_tb:
        ld A, ram0[XT]
        ld ram0[UX], A
        ld A, ram0[YT]
        ld ram0[UY], A
        ld A, ram0[XB]
        ld ram0[LX], A
        ld A, ram0[YB]
        ld ram0[LY], A
        ret
arg_tm:
        ld A, ram0[XT]
        ld ram0[UX], A
        ld A, ram0[YT]
        ld ram0[UY], A
        ld A, ram0[XM]
        ld ram0[LX], A
        ld A, ram0[YM]
        ld ram0[LY], A
        ret
arg_mb:
        ld A, ram0[XM]
        ld ram0[UX], A
        ld A, ram0[YM]
        ld ram0[UY], A
        ld A, ram0[XB]
        ld ram0[LX], A
        ld A, ram0[YB]
        ld ram0[LY], A
        ret

; left edge from (UX, UY) to (LX, LY): h = LY - UY, w = LX - UX; e(x) = h - w + 2h (x - UX) at the upper row;
; stored one row step early (e + 2w) because each row adds -2w first
setl:
        ld A, ram0[LY]
        sub A, ram0[UY]
        ld ram0[TMP], A         ; h
        add A, ram0[TMP]
        ld ram0[SL], A          ; 2h
        ld A, ram0[LX]
        sub A, ram0[UX]
        ld ram0[TMP2], A        ; w
        add A, ram0[TMP2]
        ld ram0[RL], A          ; 2w
        ldi A, 0
        sub A, ram0[RL]
        ld ram0[RL], A          ; -2w
        ld A, ram0[TMP]
        add A, ram0[TMP2]
        ld ram0[EL], A          ; h + w
        ld A, ram0[UX]
        ld r0, A
        ret

; right edge: e'(x) = e(x) - 1 = w - h - 1 - 2h (x - UX); stored as e' - 2w = -w - h - 1
setr:
        ld A, ram0[LY]
        sub A, ram0[UY]
        ld ram0[TMP], A         ; h
        add A, ram0[TMP]
        ld ram0[SR], A          ; 2h
        ld A, ram0[LX]
        sub A, ram0[UX]
        ld ram0[TMP2], A        ; w
        add A, ram0[TMP2]
        ld ram0[RR], A          ; 2w
        ldi A, 0xFFFF
        sub A, ram0[TMP]
        sub A, ram0[TMP2]
        ld ram0[ER], A
        ld A, ram0[UX]
        ld r4, A
        ret

seg_tm:
        ld A, ram0[YM]
        sub A, ram0[YT]
        ld ram0[SEGN], A
        bra always, seg
seg_mb:
        ld A, ram0[YB]
        sub A, ram0[YM]
        ld ram0[SEGN], A

; rows of one segment: step both edges, then the span when 0 <= y < 192 and xl <= xr
seg:
        ld A, ram0[SEGN]
        subi 0x01
        bra n=1, seg_end
        ld ram0[SEGN], A
; left edge: smallest x with e >= 0
        ld A, ram0[EL]
        add A, ram0[RL]
        bra n=1, l_inc
l_dec:
        sub A, ram0[SL]
        bra n=1, l_undo
        ld -, (r0-)
        bra always, l_dec
l_undo:
        add A, ram0[SL]
        bra always, l_done
l_inc:
        add A, ram0[SL]
        ld -, (r0+!)
        bra n=1, l_inc
l_done:
        ld ram0[EL], A
; right edge: largest x with e' >= 0
        ld A, ram0[ER]
        add A, ram0[RR]
        bra n=1, r_dec
r_inc:
        sub A, ram0[SR]
        bra n=1, r_undo
        ld -, (r4+!)
        bra always, r_inc
r_undo:
        add A, ram0[SR]
        bra always, r_done
r_dec:
        add A, ram0[SR]
        ld -, (r4-)
        bra n=1, r_dec
r_done:
        ld ram0[ER], A
; clip rows
        ld A, ram0[YROW]
        cmpi 0x00
        bra n=1, seg_next
        cmpi 0xC0
        bra n=0, seg_next
        ld A, r4
        sub A, r0
        bra n=1, seg_next
; span r0..r4 on row ROWB
        ld A, r0
        andi 0x03
        addi LMASK
        ld r5, A
        ld A, r4
        andi 0x03
        addi RMASK
        ld r6, A
        ld A, r0
        mod always, shr
        mod always, shr
        ld ram0[N0], A
        ld A, r4
        mod always, shr
        mod always, shr
        sub A, ram0[N0]
        ld ram0[CNT], A
        ld A, ram0[ROWB]
        add A, ram0[N0]
        ld -, AL
        ld EXT6, A
        ldi EXT6, MODE_FILL
        ld EXT4, -
        ld A, ram0[CNT]
        subi 0x01
        bra n=0, span_multi
        ld A, (r3|11)
        and A, (r5)
        and A, (r6)
        ld EXT4, A
        bra always, seg_next
span_multi:
        ld A, (r3|11)
        and A, (r5)
        ld EXT4, A
        ld Y, (r3|11)
        ldi A, FILLEND + 1
        sub A, ram0[CNT]
        ldi STACK, span_right
        ld PC, A
span_right:
        ld A, (r3|11)
        and A, (r6)
        ld EXT4, A
seg_next:
        ld A, ram0[YROW]
        addi 0x01
        ld ram0[YROW], A
        ld A, ram0[ROWB]
        addi 0x80
        ld ram0[ROWB], A
        bra always, seg
seg_end:
        ret

; 63 writes of Y through PM4; entered at FILLEND - n for n writes
FILL:
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
        ld EXT4, Y
FILLEND:
        ret

        .include "tables.inc"
