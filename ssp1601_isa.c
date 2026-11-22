/* ssp1601_isa.c  -  the SSP1601 instruction set: operand parsing, encoding, canonical text.
 * Project svp-lara, Mike Wolak, EPROM Foundry Inc.  Word layout: ssp1601_isa.h. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ssp1601_isa.h"

ssp_u16 rd16be(const ssp_u8 *p) { return (ssp_u16)((p[0] << 8) | p[1]); }
void wr16be(ssp_u8 *p, ssp_u16 v) { p[0] = (ssp_u8)(v >> 8); p[1] = (ssp_u8)v; }

const char *const ssp_reg[16] = { "-", "X", "Y", "A", "ST", "STACK", "PC", "P",
                                  "EXT0", "EXT1", "EXT2", "EXT3", "EXT4", "EXT5", "EXT6", "AL" };
const char *const ssp_regalias[16] = { 0, 0, 0, 0, 0, 0, 0, 0, "PM0", "PM1", "PM2", "XST", "PM4", 0, "PMC", 0 };
const char *const ssp_cond[16] = { "always", "c1", "gpi0", "gpi1", "l", "z", "ov", "n",
                                   "diof", "gpi2", "gpi3", "c11", "c12", "c13", "c14", "c15" };
const char *const ssp_accop[8] = { "ror", "rol", "shr", "shl", "inc", "dec", "neg", "abs" };
const char *const ssp_flagop[16] = { 0, 0, "resl", "setl", "resie", "setie", 0, 0,
                                     "resop", "setop", 0, 0, 0, 0, "res", "set" };
const char *const ssp_aluop[8] = { 0, "sub", 0, "cmp", "add", "and", "or", "eor" };

static int ieq(const char *a, const char *b)
{
    while (*a && *b) { if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0; a++; b++; }
    return *a == *b;
}

static void trim(char *s)
{
    char *b = s, *e;
    while (*b == ' ' || *b == '\t') b++;
    memmove(s, b, strlen(b) + 1);
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
}

static int reg_of(const char *s)
{
    int i;
    for (i = 0; i < 16; i++)
        if (ieq(s, ssp_reg[i]) || (ssp_regalias[i] && ieq(s, ssp_regalias[i]))) return i;
    return -1;
}

/* "rN" with an optional modifier: + +! - for r0-r2, r4-r6; |00 |01 |10 |11 for r3, r7 */
static int ptr_of(const char *src, int *ptr, int *mod)
{
    char t[32]; const char *m; int p;
    if (strlen(src) >= sizeof t) return 0;
    strcpy(t, src); trim(t);
    if ((t[0] != 'r' && t[0] != 'R') || t[1] < '0' || t[1] > '7') return 0;
    p = t[1] - '0'; m = t + 2;
    while (*m == ' ') m++;
    if (*m == 0) *mod = 0;
    else if (p == 3 || p == 7) {
        if (m[0] == '|' && (m[1] == '0' || m[1] == '1') && (m[2] == '0' || m[2] == '1') && m[3] == 0)
            *mod = (m[1] - '0') * 2 + (m[2] - '0');
        else return 0;
    } else if (!strcmp(m, "+!")) *mod = 1;
    else if (!strcmp(m, "-")) *mod = 2;
    else if (!strcmp(m, "+")) *mod = 3;
    else return 0;
    *ptr = p;
    return 1;
}

int ssp_parse_cond(const char *s, int *code)
{
    char t[64]; char *eq; int i, f = 0;
    if (strlen(s) >= sizeof t) return 0;
    strcpy(t, s); trim(t);
    eq = strchr(t, '=');
    if (eq) {
        *eq = 0; trim(t); trim(eq + 1);
        if (strcmp(eq + 1, "0") && strcmp(eq + 1, "1")) return 0;
        f = eq[1] - '0';
    }
    for (i = 0; i < 16; i++) if (ieq(t, ssp_cond[i])) { *code = (f << 8) | (i << 4); return eq || i == 0; }
    return 0;
}

void ssp_parse_opnd(const char *src, ssp_opnd *o)
{
    char s[256]; size_t n;
    memset(o, 0, sizeof *o);
    if (strlen(src) >= sizeof s) { o->kind = SO_EXPR; return; }
    strcpy(s, src); trim(s);
    strcpy(o->text, s);
    n = strlen(s);
    if (n == 0) { o->kind = SO_NONE; return; }
    if ((o->reg = reg_of(s)) >= 0) { o->kind = SO_REG; return; }
    if (ptr_of(s, &o->ptr, &o->mod)) { o->kind = SO_PTR; return; }
    if (ieq(s, "(A)")) { o->kind = SO_ACCIND; return; }
    if (n > 4 && s[0] == '(' && s[1] == '(' && s[n - 1] == ')' && s[n - 2] == ')') {
        char t[256]; memcpy(t, s + 2, n - 4); t[n - 4] = 0;
        if (ptr_of(t, &o->ptr, &o->mod)) { o->kind = SO_IND2; return; }
    }
    if (n > 2 && s[0] == '(' && s[n - 1] == ')') {
        char t[256]; memcpy(t, s + 1, n - 2); t[n - 2] = 0;
        if (ptr_of(t, &o->ptr, &o->mod)) { o->kind = SO_IND1; return; }
    }
    if (n > 6 && (s[0] == 'r' || s[0] == 'R') && (s[1] == 'a' || s[1] == 'A') && (s[2] == 'm' || s[2] == 'M')
        && (s[3] == '0' || s[3] == '1') && s[4] == '[' && s[n - 1] == ']') {
        o->kind = SO_RAM; o->bank = s[3] - '0';
        memcpy(o->text, s + 5, n - 6); o->text[n - 6] = 0;
        return;
    }
    if (ssp_parse_cond(s, &o->cond)) { o->kind = SO_COND; return; }
    o->kind = SO_EXPR;
}

/* ------------------------------------------------------------------ the form table */
enum {
    F_NOP, F_RET, F_FLAG,
    F_LD_RR, F_LD_R_I1, F_LD_I1_R, F_LD_A_RAM, F_LDI_R, F_LD_R_I2, F_LDI_I1, F_LD_RAM_A, F_LD_R_P, F_LD_P_R,
    F_LDI_P, F_LD_R_ACC,
    F_ALU_R, F_ALU_I1, F_ALU_RAM, F_ALUI_L, F_ALU_I2, F_ALU_P, F_ALUI_S,
    F_MOD, F_CALL, F_BRA, F_CALL_A, F_BRA_A, F_MAC
};
typedef struct { const char *mn; int form; int k0, k1, k2; ssp_u16 base; int words; } form_t;

#define R SO_REG
#define I1 SO_IND1
#define I2 SO_IND2
#define P SO_PTR
#define M SO_RAM
#define E SO_EXPR
#define C SO_COND
#define N SO_NONE
static const form_t forms[] = {
    { "nop",  F_NOP,      N, N, N, 0x0000, 1 },
    { "ret",  F_RET,      N, N, N, 0x0065, 1 },
    { "ld",   F_LD_RR,    R, R, N, 0x0000, 1 },
    { "ld",   F_LD_R_I1,  R, I1, N, 0x0200, 1 },
    { "ld",   F_LD_I1_R,  I1, R, N, 0x0400, 1 },
    { "ld",   F_LD_A_RAM, R, M, N, 0x0600, 1 },
    { "ld",   F_LD_R_I2,  R, I2, N, 0x0A00, 1 },
    { "ld",   F_LD_RAM_A, M, R, N, 0x0E00, 1 },
    { "ld",   F_LD_R_P,   R, P, N, 0x1200, 1 },
    { "ld",   F_LD_P_R,   P, R, N, 0x1400, 1 },
    { "ld",   F_LD_R_ACC, R, SO_ACCIND, N, 0x4A00, 1 },
    { "ldi",  F_LDI_R,    R, E, N, 0x0800, 2 },
    { "ldi",  F_LDI_I1,   I1, E, N, 0x0C00, 2 },
    { "ldi",  F_LDI_P,    P, E, N, 0x1800, 1 },
    { "call", F_CALL,     C, E, N, 0x4800, 2 },
    { "bra",  F_BRA,      C, E, N, 0x4C00, 2 },
    { "call", F_CALL_A,   E, N, N, 0x4800, 2 },
    { "bra",  F_BRA_A,    E, N, N, 0x4C00, 2 },
    { "mod",  F_MOD,      C, E, N, 0x9000, 1 },
    { "mpys", F_MAC,      I1, I1, N, 0x3600, 1 },
    { "mpya", F_MAC,      I1, I1, N, 0x9600, 1 },
    { "mld",  F_MAC,      I1, I1, N, 0xB600, 1 },
    { "mpys", F_MAC,      I1, I1, E, 0x3600, 1 },
    { "mpya", F_MAC,      I1, I1, E, 0x9600, 1 },
    { "mld",  F_MAC,      I1, I1, E, 0xB600, 1 },
};
#undef R
#undef I1
#undef I2
#undef P
#undef M
#undef E
#undef C
#undef N

static int alu_of(const char *mn, int *imm)
{
    int i; size_t l = strlen(mn);
    *imm = 0;
    for (i = 0; i < 8; i++) {
        if (!ssp_aluop[i]) continue;
        if (ieq(mn, ssp_aluop[i])) return i;
        if (l == strlen(ssp_aluop[i]) + 1 && (mn[l - 1] == 'i' || mn[l - 1] == 'I')) {
            char t[8]; memcpy(t, mn, l - 1); t[l - 1] = 0;
            if (ieq(t, ssp_aluop[i])) { *imm = 1; return i; }
        }
    }
    return -1;
}

static int flag_of(const char *mn)
{
    int i;
    for (i = 0; i < 16; i++) if (ssp_flagop[i] && ieq(mn, ssp_flagop[i])) return i;
    return -1;
}

/* the operand kind as a form sees it: "A" also matches an expression slot only when it is not a register */
static int kind_ok(int want, const ssp_opnd *o)
{
    if (want == o->kind) return 1;
    if (want == SO_EXPR && o->kind == SO_COND) return 0;
    return 0;
}

/* find the form; for ALU and flag mnemonics returns synthetic forms */
static int find_form(const char *mn, int nops, const ssp_opnd *op, form_t *f, char *err, int errsz)
{
    size_t i; int imm, o;
    if ((o = alu_of(mn, &imm)) >= 0) {
        f->mn = mn; f->base = (ssp_u16)(o << 13); f->words = 1;
        if (imm) {
            if (nops == 2 && op[0].kind == SO_REG && op[0].reg == 3 && op[1].kind == SO_EXPR) { f->form = F_ALUI_L; f->words = 2; return 1; }
            if (nops == 1 && op[0].kind == SO_EXPR) { f->form = F_ALUI_S; return 1; }
        } else if (nops == 2 && op[0].kind == SO_REG && op[0].reg == 3) {
            switch (op[1].kind) {
            case SO_REG:  f->form = F_ALU_R; return 1;
            case SO_IND1: f->form = F_ALU_I1; return 1;
            case SO_RAM:  f->form = F_ALU_RAM; return 1;
            case SO_IND2: f->form = F_ALU_I2; return 1;
            case SO_PTR:  f->form = F_ALU_P; return 1;
            default: break;
            }
        }
        snprintf(err, (size_t)errsz, "%s: operands do not match any form (OP A, s | (ri) | ramB[a] | ((ri)) | ri; OPi A, imm16; OPi imm8)", mn);
        return 0;
    }
    if (nops == 0 && (o = flag_of(mn)) >= 0) { f->mn = mn; f->form = F_FLAG; f->base = (ssp_u16)(0x9400 | o); f->words = 1; return 1; }
    for (i = 0; i < sizeof forms / sizeof forms[0]; i++) {
        const form_t *t = &forms[i];
        int want = (t->k0 != SO_NONE) + (t->k1 != SO_NONE) + (t->k2 != SO_NONE);
        if (!ieq(mn, t->mn) || want != nops) continue;
        if (nops > 0 && !kind_ok(t->k0, &op[0])) continue;
        if (nops > 1 && !kind_ok(t->k1, &op[1])) continue;
        if (nops > 2 && !kind_ok(t->k2, &op[2])) continue;
        *f = *t;
        return 1;
    }
    snprintf(err, (size_t)errsz, "%s: no form takes these operands", mn);
    return 0;
}

int ssp_size(const char *mn, int nops, const ssp_opnd *op, char *err, int errsz)
{
    form_t f;
    return find_form(mn, nops, op, &f, err, errsz) ? f.words : 0;
}

static int pfield(const ssp_opnd *o) { return ((o->ptr >> 2) << 8) | (o->mod << 2) | (o->ptr & 3); }

static int range(long v, long lo, long hi, const char *what, char *err, int errsz)
{
    if (v < lo || v > hi) { snprintf(err, (size_t)errsz, "%s %ld outside %ld..%ld", what, v, lo, hi); return 0; }
    return 1;
}

int ssp_encode(const char *mn, int nops, const ssp_opnd *op, const long *val, ssp_u16 out[2], int *nw,
               char *err, int errsz)
{
    form_t f; ssp_u16 w;
    if (!find_form(mn, nops, op, &f, err, errsz)) return -1;
    w = f.base; *nw = f.words; out[1] = 0;
    switch (f.form) {
    case F_NOP: case F_RET: case F_FLAG: break;
    case F_LD_RR:    w |= (ssp_u16)(op[0].reg << 4 | op[1].reg); break;
    case F_LD_R_I1:  case F_LD_R_I2: case F_LD_R_P: w |= (ssp_u16)(op[0].reg << 4 | pfield(&op[1])); break;
    case F_LD_I1_R:  case F_LD_P_R: w |= (ssp_u16)(op[1].reg << 4 | pfield(&op[0])); break;
    case F_LD_A_RAM:
        if (op[0].reg != 3) { snprintf(err, (size_t)errsz, "ld: direct RAM source needs destination A"); return -1; }
        if (!range(val[1], 0, 255, "RAM address", err, errsz)) return -1;
        w |= (ssp_u16)(op[1].bank << 8 | val[1]); break;
    case F_LD_RAM_A:
        if (op[1].reg != 3) { snprintf(err, (size_t)errsz, "ld: direct RAM destination needs source A"); return -1; }
        if (!range(val[0], 0, 255, "RAM address", err, errsz)) return -1;
        w |= (ssp_u16)(op[0].bank << 8 | val[0]); break;
    case F_LD_R_ACC: w |= (ssp_u16)(op[0].reg << 4); break;
    case F_LDI_R:
        if (!range(val[1], -32768, 65535, "immediate", err, errsz)) return -1;
        w |= (ssp_u16)(op[0].reg << 4); out[1] = (ssp_u16)val[1]; break;
    case F_LDI_I1:
        if (!range(val[1], -32768, 65535, "immediate", err, errsz)) return -1;
        w |= (ssp_u16)pfield(&op[0]); out[1] = (ssp_u16)val[1]; break;
    case F_LDI_P:
        if (op[0].mod) { snprintf(err, (size_t)errsz, "ldi rN takes no modifier"); return -1; }
        if (!range(val[1], -128, 255, "immediate", err, errsz)) return -1;
        w |= (ssp_u16)(op[0].ptr << 8 | (val[1] & 0xFF)); break;
    case F_ALU_R:   w |= (ssp_u16)op[1].reg; break;
    case F_ALU_I1:  w |= (ssp_u16)(0x0200 | pfield(&op[1])); break;
    case F_ALU_RAM:
        if (!range(val[1], 0, 255, "RAM address", err, errsz)) return -1;
        w |= (ssp_u16)(0x0600 | op[1].bank << 8 | val[1]); break;
    case F_ALUI_L:
        if (!range(val[1], -32768, 65535, "immediate", err, errsz)) return -1;
        w |= 0x0800; out[1] = (ssp_u16)val[1]; break;
    case F_ALU_I2:  w |= (ssp_u16)(0x0A00 | pfield(&op[1])); break;
    case F_ALU_P:   w |= (ssp_u16)(0x1200 | pfield(&op[1])); break;
    case F_ALUI_S:
        if (!range(val[0], 0, 255, "immediate", err, errsz)) return -1;
        w |= (ssp_u16)(0x1800 | val[0]); break;
    case F_MOD: {
        int i;
        for (i = 0; i < 8; i++) if (ieq(op[1].text, ssp_accop[i])) break;
        if (i == 8) { snprintf(err, (size_t)errsz, "mod: unknown accumulator operation %s", op[1].text); return -1; }
        w |= (ssp_u16)(op[0].cond | i); break;
    }
    case F_CALL: case F_BRA:
        if (!range(val[1], 0, 65535, "address", err, errsz)) return -1;
        w |= (ssp_u16)op[0].cond; out[1] = (ssp_u16)val[1]; break;
    case F_CALL_A: case F_BRA_A:
        if (!range(val[0], 0, 65535, "address", err, errsz)) return -1;
        out[1] = (ssp_u16)val[0]; break;
    case F_MAC: {
        long b = nops == 3 ? val[2] : 1;
        if (op[0].ptr < 4 || op[1].ptr > 3) { snprintf(err, (size_t)errsz, "%s: operands are (r4..r7), (r0..r3)", mn); return -1; }
        if (!range(b, 0, 1, "third operand", err, errsz)) return -1;
        w |= (ssp_u16)(b << 8 | op[0].mod << 6 | (op[0].ptr & 3) << 4 | op[1].mod << 2 | (op[1].ptr & 3));
        break;
    }
    default: snprintf(err, (size_t)errsz, "internal: form %d", f.form); return -1;
    }
    out[0] = w;
    return 0;
}

/* ------------------------------------------------------------------ canonical text */
static const char *pmod(int p, int m)
{
    static const char *const md[4] = { "", "+!", "-", "+" }, *const ms[4] = { "|00", "|01", "|10", "|11" };
    return (p == 3 || p == 7) ? ms[m] : md[m];
}

static int canon_raw(const ssp_u16 *w, char *out, int on)
{
    ssp_u16 op = w[0], ext = w[1];
    int p = ((op >> 8) & 1) * 4 + (op & 3), m = (op >> 2) & 3, r = (op >> 4) & 15, k = op >> 9;
    char cond[32]; size_t n = (size_t)on;
    if (((op >> 4) & 15) == 0 && !((op >> 8) & 1)) snprintf(cond, sizeof cond, "always");
    else snprintf(cond, sizeof cond, "%s=%d", ssp_cond[(op >> 4) & 15], (op >> 8) & 1);
    if (op == 0) { snprintf(out, n, "nop"); return 1; }
    if (op == 0x0065) { snprintf(out, n, "ret"); return 1; }
    switch (k) {
    case 0x00: snprintf(out, n, "ld %s, %s", ssp_reg[r], ssp_reg[op & 15]); return 1;
    case 0x01: snprintf(out, n, "ld %s, (r%d%s)", ssp_reg[r], p, pmod(p, m)); return 1;
    case 0x02: snprintf(out, n, "ld (r%d%s), %s", p, pmod(p, m), ssp_reg[r]); return 1;
    case 0x03: snprintf(out, n, "ld A, ram%d[0x%02X]", (op >> 8) & 1, op & 0xFF); return 1;
    case 0x04: snprintf(out, n, "ldi %s, 0x%04X", ssp_reg[r], ext); return 2;
    case 0x05: snprintf(out, n, "ld %s, ((r%d%s))", ssp_reg[r], p, pmod(p, m)); return 1;
    case 0x06: snprintf(out, n, "ldi (r%d%s), 0x%04X", p, pmod(p, m), ext); return 2;
    case 0x07: snprintf(out, n, "ld ram%d[0x%02X], A", (op >> 8) & 1, op & 0xFF); return 1;
    case 0x09: snprintf(out, n, "ld %s, r%d%s", ssp_reg[r], p, pmod(p, m)); return 1;
    case 0x0a: snprintf(out, n, "ld r%d%s, %s", p, pmod(p, m), ssp_reg[r]); return 1;
    case 0x0c: case 0x0d: case 0x0e: case 0x0f: snprintf(out, n, "ldi r%d, 0x%02X", (op >> 8) & 7, op & 0xFF); return 1;
    case 0x24: snprintf(out, n, "call %s, 0x%04X", cond, ext); return 2;
    case 0x25: snprintf(out, n, "ld %s, (A)", ssp_reg[r]); return 1;
    case 0x26: snprintf(out, n, "bra %s, 0x%04X", cond, ext); return 2;
    case 0x48: snprintf(out, n, "mod %s, %s", cond, ssp_accop[op & 7]); return 1;
    case 0x4a: if (ssp_flagop[op & 15]) { snprintf(out, n, "%s", ssp_flagop[op & 15]); return 1; } break;
    case 0x1b: case 0x4b: case 0x5b:
        snprintf(out, n, "%s (r%d%s), (r%d%s), %d", k == 0x1b ? "mpys" : k == 0x4b ? "mpya" : "mld",
                 4 + ((op >> 4) & 3), pmod(4 + ((op >> 4) & 3), (op >> 6) & 3), op & 3, pmod(op & 3, m), (op >> 8) & 1);
        return 1;
    default: break;
    }
    if (ssp_aluop[op >> 13]) {
        const char *a = ssp_aluop[op >> 13];
        switch (k & 0xF) {
        case 0x0: snprintf(out, n, "%s A, %s", a, ssp_reg[op & 15]); return 1;
        case 0x1: snprintf(out, n, "%s A, (r%d%s)", a, p, pmod(p, m)); return 1;
        case 0x3: snprintf(out, n, "%s A, ram%d[0x%02X]", a, (op >> 8) & 1, op & 0xFF); return 1;
        case 0x4: snprintf(out, n, "%si A, 0x%04X", a, ext); return 2;
        case 0x5: snprintf(out, n, "%s A, ((r%d%s))", a, p, pmod(p, m)); return 1;
        case 0x9: snprintf(out, n, "%s A, r%d%s", a, p, pmod(p, m)); return 1;
        case 0xc: snprintf(out, n, "%si 0x%02X", a, op & 0xFF); return 1;
        default: break;
        }
    }
    return 0;
}

/* split "a, b, c" at top-level commas */
static int split(char *s, char **v, int max)
{
    int n = 0, depth = 0; char *p = s, *start = s;
    if (!*s) return 0;
    for (;; p++) {
        if (*p == '(' || *p == '[') depth++;
        else if (*p == ')' || *p == ']') depth--;
        if ((*p == ',' && depth == 0) || *p == 0) {
            int end = *p == 0;
            *p = 0; if (n < max) v[n++] = start; start = p + 1;
            if (end) break;
        }
    }
    return n;
}

int ssp_canon(const ssp_u16 *w, char *out, int outsz)
{
    char t[128], *mn, *rest, *ops[4]; ssp_opnd o[3]; long val[3]; ssp_u16 enc[2]; int sz, nops, nw, i;
    char err[160];
    sz = canon_raw(w, t, sizeof t);
    if (sz) {                                   /* keep the text only when it encodes back to the same words */
        char c[128]; strcpy(c, t);
        mn = c; rest = c; while (*rest && *rest != ' ') rest++;
        if (*rest) *rest++ = 0;
        nops = split(rest, ops, 4);
        for (i = 0; i < 3; i++) { ssp_parse_opnd(i < nops ? ops[i] : "", &o[i]); val[i] = strtol(o[i].text, 0, 0); }
        if (nops > 3 || ssp_encode(mn, nops, o, val, enc, &nw, err, sizeof err) || nw != sz || enc[0] != w[0]
            || (sz == 2 && enc[1] != w[1])) sz = 0;
    }
    if (!sz) { snprintf(out, (size_t)outsz, "dw 0x%04X", w[0]); return 1; }
    snprintf(out, (size_t)outsz, "%s", t);
    return sz;
}
