/*
 * ssp16sim.c -- instruction-set simulator of the SSP1601 subset used by this cartridge, for tracing and instruction
 * counts.  Semantics: svpdoc.txt section 2 (registers, pointer modifiers, PMC/PM4 programming, over-write mode) and
 * the MiSTer SSP160x core's decode (conditions always, z, n; mod shr, shl, neg, abs).  Not cycle-accurate: it counts
 * instructions and their words.
 *
 *   ssp16sim [-t] [-n max] [-o dram.bin] image.bin k     run one command (frame k) from word 0x0400 until the SSP
 *                                                         writes XST; -o writes the 128 KB DRAM as big-endian words
 */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint16_t prog[0x10000], ram[2][256], dram[0x10000];
static uint32_t A, PMC, pm_w4, pm_r4;
static uint16_t X, Y, ST, PC, stk[6], xst; static int sp, pmc_hi, pm_set_w, ca;
static uint8_t r[8];
static int fz, fn, trace, xst_written;
static unsigned long ninst, nwords, nlong;

static int32_t Pv(void) { return (int32_t)(int16_t)X * (int16_t)Y * 2; }

static unsigned ptr(int p, int m)                      /* pointer value after the modifier; r3, r7: m selects 0..3 */
{
    unsigned v;
    if ((p & 3) == 3) return (unsigned)m;
    v = r[p];
    if (m == 1 || m == 3) r[p]++;
    else if (m == 2) r[p]--;
    return v;
}

static void pm4_write(uint16_t d)
{
    uint32_t a = pm_w4 & 0x1FFFFF; uint16_t mode = (uint16_t)(pm_w4 >> 16) & 0xFFE0;
    unsigned inc = (mode >> 11) & 7;
    if ((a >> 16) == 0x18) {
        uint16_t *p = &dram[a & 0xFFFF];
        if (mode & 0x0400) { int s; for (s = 0; s < 16; s += 4) if (d & (0xF << s)) *p = (uint16_t)((*p & ~(0xF << s)) | (d & (0xF << s))); }
        else *p = d;
    } else fprintf(stderr, "PM4 write outside DRAM %06X\n", (unsigned)a);
    if (inc) pm_w4 = (pm_w4 & 0xFFE00000u) | ((a + (inc == 7 ? 128u : 1u << (inc - 1))) & 0x1FFFFF);
}

static uint16_t rd(int reg, int blind)
{
    switch (reg) {
    case 0: return 0xFFFF;
    case 1: return X;
    case 2: return Y;
    case 3: return (uint16_t)(A >> 16);
    case 4: return (uint16_t)((ST & 0x0FFF) | (fz << 13) | (fn << 15));
    case 5: sp = (sp + 5) % 6; return stk[sp];
    case 6: return PC;
    case 7: return (uint16_t)(Pv() >> 16);
    case 8: { uint16_t v = (uint16_t)(ca << 1); ca = 0; return v; }           /* PM0 as XST status */
    case 11: return xst;
    case 12: if (blind) { pm_r4 = PMC; return 0; } fprintf(stderr, "PM4 read not modelled\n"); return 0;
    case 14: { uint16_t v = pmc_hi ? (uint16_t)(PMC >> 16) : (uint16_t)PMC; pmc_hi ^= 1; return v; }
    case 15: if (blind) { pmc_hi = 0; return 0; } return (uint16_t)A;
    default: fprintf(stderr, "read of register %d at %04X\n", reg, PC); return 0;
    }
}

static void wr(int reg, uint16_t v, int blind, int is_p32)
{
    switch (reg) {
    case 0: break;
    case 1: X = v; break;
    case 2: Y = v; break;
    case 3: if (is_p32) A = (uint32_t)Pv(); else A = (A & 0xFFFF) | ((uint32_t)v << 16); break;
    case 4: ST = v; break;
    case 5: stk[sp] = v; sp = (sp + 1) % 6; break;
    case 6: PC = v; break;
    case 11: xst = v; xst_written = 1; break;
    case 12: if (blind) { pm_w4 = PMC; pm_set_w = 1; } else pm4_write(v); break;
    case 14: if (pmc_hi) PMC = (PMC & 0xFFFF) | ((uint32_t)v << 16); else PMC = (PMC & 0xFFFF0000u) | v; pmc_hi ^= 1; break;
    case 15: A = (A & 0xFFFF0000u) | v; break;
    default: fprintf(stderr, "write of register %d at %04X\n", reg, PC); break;
    }
}

static void flags(void) { fz = A == 0; fn = (int)(A >> 31); }

static int cond(uint16_t op)
{
    int c = (op >> 4) & 15, f = (op >> 8) & 1;
    if (c == 0) return 1;
    if (c == 5) return fz == f;
    if (c == 7) return fn == f;
    fprintf(stderr, "condition %d not in the subset at %04X\n", c, PC);
    return 0;
}

static void alu(int o, uint32_t b)
{
    switch (o) {
    case 1: A -= b; break;
    case 3: { uint32_t t = A - b; fz = t == 0; fn = (int)(t >> 31); return; }
    case 4: A += b; break;
    case 5: A &= b; break;
    case 6: A |= b; break;
    case 7: A ^= b; break;
    default: fprintf(stderr, "ALU op %d\n", o); break;
    }
    flags();
}

static void step(void)
{
    uint16_t op = prog[PC], pc0 = PC; int k = op >> 9, p = ((op >> 8) & 1) * 4 + (op & 3), m = (op >> 2) & 3;
    int d = (op >> 4) & 15, s = op & 15, o = op >> 13;
    PC++; ninst++; nwords++;
    if (trace) fprintf(stderr, "%04X %04X A=%08X X=%04X Y=%04X r0=%02X r4=%02X\n", pc0, op, (unsigned)A, X, Y, r[0], r[4]);
    switch (k) {
    case 0x00:
        if (op == 0x0065) { sp = (sp + 5) % 6; PC = stk[sp]; return; }
        if (d == 3 && s == 7) { A = (uint32_t)Pv(); return; }
        if (d == 3 && s == 3) return;
        wr(d, rd(s, d == 0), s == 0, 0); return;
    case 0x01: wr(d, ram[p >> 2][ptr(p, m)], 0, 0); return;
    case 0x02: { uint16_t v = rd(d, 0); ram[p >> 2][ptr(p, m)] = v; return; }
    case 0x03: A = (A & 0xFFFF) | ((uint32_t)ram[(op >> 8) & 1][op & 0xFF] << 16); return;
    case 0x04: nwords++; nlong++; wr(d, prog[PC++], 0, 0); return;
    case 0x05: { uint16_t *a = &ram[p >> 2][ptr(p, m)]; nlong++; wr(d, prog[*a], 0, 0); (*a)++; return; }
    case 0x06: nwords++; nlong++; ram[p >> 2][ptr(p, m)] = prog[PC++]; return;
    case 0x07: ram[(op >> 8) & 1][op & 0xFF] = (uint16_t)(A >> 16); return;
    case 0x09: wr(d, r[p], 0, 0); return;
    case 0x0a: r[p] = (uint8_t)rd(d, 0); return;
    case 0x0c: case 0x0d: case 0x0e: case 0x0f: r[(op >> 8) & 7] = (uint8_t)op; return;
    case 0x24: nwords++; nlong++; if (cond(op)) { stk[sp] = (uint16_t)(PC + 1); sp = (sp + 1) % 6; PC = prog[PC]; } else PC++; return;
    case 0x25: nlong++; wr(d, prog[A >> 16], 0, 0); return;
    case 0x26: nwords++; nlong++; if (cond(op)) PC = prog[PC]; else PC++; return;
    case 0x48:
        if (!cond(op)) return;
        switch (op & 7) {
        case 2: A = (uint32_t)((int32_t)A >> 1); break;
        case 3: A <<= 1; break;
        case 6: A = (uint32_t)-(int32_t)A; break;
        case 7: if ((int32_t)A < 0) A = (uint32_t)-(int32_t)A; break;
        default: fprintf(stderr, "mod op %d not in the subset at %04X\n", op & 7, pc0); return;
        }
        flags(); return;
    default: break;
    }
    if (o == 1 || o >= 3) {
        switch (k & 0xF) {
        case 0x0: alu(o, s == 7 ? (uint32_t)Pv() : s == 3 ? A : (uint32_t)rd(s, 0) << 16); return;
        case 0x1: alu(o, (uint32_t)ram[p >> 2][ptr(p, m)] << 16); return;
        case 0x3: alu(o, (uint32_t)ram[(op >> 8) & 1][op & 0xFF] << 16); return;
        case 0x4: nwords++; nlong++; alu(o, (uint32_t)prog[PC++] << 16); return;
        case 0x9: alu(o, (uint32_t)r[p] << 16); return;
        case 0xc: alu(o, (uint32_t)(op & 0xFF) << 16); return;
        default: break;
        }
    }
    fprintf(stderr, "opcode %04X at %04X not modelled\n", op, pc0);
    exit(2);
}

int main(int argc, char **argv)
{
    const char *out = NULL; unsigned long max = 50000000ul; int c; FILE *f; unsigned char *raw; size_t n, i;
    while ((c = getopt(argc, argv, "tn:o:")) != -1) {
        if (c == 't') trace = 1;
        else if (c == 'n') max = strtoul(optarg, 0, 0);
        else if (c == 'o') out = optarg;
        else return 1;
    }
    if (optind + 2 > argc) { fprintf(stderr, "usage: ssp16sim [-t] [-n max] [-o dram.bin] image.bin k\n"); return 1; }
    f = fopen(argv[optind], "rb"); if (!f) { perror(argv[optind]); return 1; }
    raw = malloc(0x20000); n = fread(raw, 1, 0x20000, f); fclose(f);
    for (i = 0; i < n / 2; i++) prog[i] = (uint16_t)(raw[2 * i] << 8 | raw[2 * i + 1]);
    xst = (uint16_t)strtoul(argv[optind + 1], 0, 0); ca = 1; PC = 0x0400;
    while (!xst_written && ninst < max) step();
    printf("frame %s: %lu instructions, %lu words fetched, %lu two-cycle (extension or indirect) instructions\n",
           argv[optind + 1], ninst, nwords, nlong);
    if (getenv("SSP16SIM_RAM")) {
        for (i = 0; i < 256; i++) fprintf(stderr, "%s%04X%s", (i & 15) ? "" : "ram0 ", ram[0][i], (i & 15) == 15 ? "\n" : " ");
    }
    if (out) {
        f = fopen(out, "wb"); if (!f) { perror(out); return 1; }
        for (i = 0; i < 0x10000; i++) { fputc(dram[i] >> 8, f); fputc(dram[i] & 0xFF, f); }
        fclose(f);
    }
    return xst_written ? 0 : 3;
}
