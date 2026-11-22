/* ssp1601_isa.h  -  the Samsung SSP1601 (Sega Virtua Processor) instruction set: decode, canonical text, encode.
 * Project svp-lara, Mike Wolak, EPROM Foundry Inc.  Module layout of esdsp's es5510_isa.h (project dp4plusplus);
 * encodings from svpdoc.txt section 2.4 (Grazvydas Ignotas) and MAME's ssp1601d.cpp (BSD-3-Clause).
 *
 * Word layout (16-bit words; a second word carries an immediate or a branch target):
 *   0000 0000 dddd ssss   ld d, s              0000 001j dddd mmpp   ld d, (ri)
 *   0000 010j ssss mmpp   ld (ri), s           0000 011j aaaa aaaa   ld A, ramj[a]
 *   0000 1000 dddd 0000   ldi d, imm16         0000 101j dddd mmpp   ld d, ((ri))
 *   0000 110j 0000 mmpp   ldi (ri), imm16      0000 111j aaaa aaaa   ld ramj[a], A
 *   0001 001j dddd mmpp   ld d, ri             0001 010j ssss mmpp   ld ri, s
 *   0001 1jpp iiii iiii   ldi ri, imm8         0100 1010 dddd 0000   ld d, (A)
 *   ooo0 0000 0000 ssss   OP A, s              ooo0 001j 0000 mmpp   OP A, (ri)
 *   ooo0 011j aaaa aaaa   OP A, ramj[a]        ooo0 1000 0000 0000   OPi A, imm16
 *   ooo0 101j 0000 mmpp   OP A, ((ri))         ooo1 001j 0000 mmpp   OP A, ri
 *   ooo1 1000 iiii iiii   OPi imm8             (ooo: 1 sub, 3 cmp, 4 add, 5 and, 6 or, 7 eor)
 *   0100 100f cccc 0000   call cond, addr      0100 110f cccc 0000   bra cond, addr
 *   1001 000f cccc 0ooo   mod cond, op         1001 0100 0000 ffff   flag operation
 *   0011 011b mmjj mmii   mpys (rj), (ri), b   1001 011b ...  mpya   1011 011b ...  mld
 *   0000 0000 0000 0000   nop (ld -, -)        0000 0000 0110 0101   ret (ld PC, STACK)
 * Registers 0-15: - X Y A ST STACK PC P EXT0..EXT6 AL (EXT0 PM0, EXT1 PM1, EXT2 PM2, EXT3 XST, EXT4 PM4, EXT6 PMC).
 * Pointer field: j (bit 8) selects RAM0 (r0..r3) or RAM1 (r4..r7), pp the register, mm the modifier:
 *   r0-r2, r4-r6: none, +!, -, +;   r3, r7: |00 |01 |10 |11 (word 0..3 of the bank).
 */
#ifndef SSP1601_ISA_H
#define SSP1601_ISA_H

#include "sspcompat.h"

extern const char *const ssp_reg[16];
extern const char *const ssp_regalias[16];
extern const char *const ssp_cond[16];
extern const char *const ssp_accop[8];
extern const char *const ssp_flagop[16];
extern const char *const ssp_aluop[8];

/* an operand as written, before expression values are known */
enum { SO_NONE, SO_REG, SO_PTR, SO_IND1, SO_IND2, SO_RAM, SO_ACCIND, SO_EXPR, SO_COND };
typedef struct {
    int kind;
    int reg;            /* SO_REG */
    int ptr, mod;       /* SO_PTR, SO_IND1, SO_IND2 */
    int bank;           /* SO_RAM; the address is an expression in text */
    int cond;           /* SO_COND: (f << 8) | (c << 4) */
    char text[256];     /* SO_EXPR, SO_RAM: the expression */
} ssp_opnd;

void ssp_parse_opnd(const char *s, ssp_opnd *o);
int  ssp_parse_cond(const char *s, int *code);

/* number of words the statement occupies (1 or 2); 0 when no form matches, with the reason in err */
int ssp_size(const char *mn, int nops, const ssp_opnd *op, char *err, int errsz);

/* encode; val[i] holds the value of operand i where it is an expression (SO_EXPR, SO_RAM); 0 ok, -1 with err */
int ssp_encode(const char *mn, int nops, const ssp_opnd *op, const long *val, ssp_u16 out[2], int *nw,
               char *err, int errsz);

/* canonical text of the instruction at w[0] (w[1] the following word); returns its size, 1 or 2.  The text
 * re-assembles to the same words; a word with no canonical form comes back as "dw 0xNNNN". */
int ssp_canon(const ssp_u16 *w, char *out, int outsz);

#endif /* SSP1601_ISA_H */
