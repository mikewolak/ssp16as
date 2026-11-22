/* ssp16as.c  -  assembler and disassembler for the Samsung SSP1601 DSP of the Sega Virtua Processor (SVP).
 * Project svp-lara, Mike Wolak, EPROM Foundry Inc.  Subcommand layout, listing and preprocessor of esdsp (project
 * dp4plusplus, Mike Wolak, EPROM Foundry Inc.); the instruction set is ssp1601_isa.c.
 *
 *   ssp16as asm  [-o OUT] [-f image|raw|s] [-b BASE] [-s BYTES] [-l LST] [-I DIR] [-D N[=V]] FILE.s
 *   ssp16as dasm [-o OUT] [-r FIRST:END] FILE.bin
 *   ssp16as isa
 *
 * Program memory is 64 K words; word w is bytes 2w, 2w+1 of the cartridge image, big-endian.  The SSP starts at word
 * 0x0400; assembly starts there unless an org says otherwise.
 * Source: one statement a line, ';' or '#' starts a comment (outside a quoted string); the espp directives (.include,
 * .equ, .define, .macro, .rept, .if ...) are applied first, then labels "name:", "NAME equ EXPR", org EXPR,
 * dw EXPR,... and instructions (ssp1601_isa.h).  Expressions: numbers (0x.. or $.. hexadecimal, decimal), symbols,
 * + - * / % << >> & | ^ ~ and parentheses.
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "espp.h"
#include "ssp1601_isa.h"

#define IMG_WORDS 0x100000L             /* 2 MB cartridge image */
#define MAXSYM 8192

typedef struct { char name[64]; long val; int defined; } sym_t;
static sym_t syms[MAXSYM];
static int nsyms, pass, errors;
static const char *cur_file; static int cur_line;
static ssp_u16 *img; static unsigned char *touched;
static long pc, lo_word = -1, hi_word = -1;

static void *xmalloc(size_t n) { void *p = calloc(1, n); if (!p) { fprintf(stderr, "ssp16as: out of memory\n"); exit(1); } return p; }

static void err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:%d: error: ", cur_file ? cur_file : "?", cur_line);
    vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
    errors++;
}

static sym_t *sym_find(const char *n)
{
    int i;
    for (i = 0; i < nsyms; i++) if (!strcmp(syms[i].name, n)) return &syms[i];
    return NULL;
}

static void sym_set(const char *n, long v)
{
    sym_t *s = sym_find(n);
    if (!s) {
        if (nsyms >= MAXSYM) { err("too many symbols"); return; }
        s = &syms[nsyms++];
        if (strlen(n) >= sizeof s->name) err("symbol name too long: %s", n);
        strncpy(s->name, n, sizeof s->name - 1);
    }
    if (pass == 1 && s->defined) { err("symbol %s defined twice", n); return; }
    s->val = v; s->defined = 1;
}

/* ------------------------------------------------------------------ expressions */
static const char *ep;
static void skipws(void) { while (*ep == ' ' || *ep == '\t') ep++; }
static long e_or(void);
static long e_atom(void)
{
    skipws();
    if (*ep == '(') { long v; ep++; v = e_or(); skipws(); if (*ep == ')') ep++; else err("missing )"); return v; }
    if (*ep == '-') { ep++; return -e_atom(); }
    if (*ep == '+') { ep++; return e_atom(); }
    if (*ep == '~') { ep++; return ~e_atom(); }
    if (*ep == '$' && isxdigit((unsigned char)ep[1])) { ep++; return strtol(ep, (char **)&ep, 16); }
    if (ep[0] == '0' && (ep[1] == 'x' || ep[1] == 'X')) { ep += 2; return strtol(ep, (char **)&ep, 16); }
    if (isdigit((unsigned char)*ep)) return strtol(ep, (char **)&ep, 10);
    if (isalpha((unsigned char)*ep) || *ep == '_' || *ep == '.') {
        char n[64]; int k = 0; sym_t *s;
        while ((isalnum((unsigned char)*ep) || *ep == '_' || *ep == '.') && k < 63) n[k++] = *ep++;
        n[k] = 0; s = sym_find(n);
        if (!s || !s->defined) { if (pass == 2) err("undefined symbol %s", n); return 0; }
        return s->val;
    }
    err("bad expression at '%s'", ep);
    ep += strlen(ep);
    return 0;
}
static long e_mul(void)
{
    long v = e_atom();
    for (;;) {
        skipws();
        if (*ep == '*') { ep++; v *= e_atom(); }
        else if (*ep == '/' || *ep == '%') {
            char o = *ep++; long d = e_atom();
            if (!d) { err("division by zero"); d = 1; }
            v = o == '/' ? v / d : v % d;
        } else return v;
    }
}
static long e_add(void)
{
    long v = e_mul();
    for (;;) { skipws(); if (*ep == '+') { ep++; v += e_mul(); } else if (*ep == '-') { ep++; v -= e_mul(); } else return v; }
}
static long e_sh(void)
{
    long v = e_add();
    for (;;) {
        skipws();
        if (ep[0] == '<' && ep[1] == '<') { ep += 2; v <<= e_add(); }
        else if (ep[0] == '>' && ep[1] == '>') { ep += 2; v >>= e_add(); }
        else return v;
    }
}
static long e_and(void) { long v = e_sh(); for (;;) { skipws(); if (*ep == '&') { ep++; v &= e_sh(); } else return v; } }
static long e_xor(void) { long v = e_and(); for (;;) { skipws(); if (*ep == '^') { ep++; v ^= e_and(); } else return v; } }
static long e_or(void)  { long v = e_xor(); for (;;) { skipws(); if (*ep == '|') { ep++; v |= e_xor(); } else return v; } }
static long eval(const char *s)
{
    long v; ep = s; v = e_or(); skipws();
    if (*ep) err("junk after expression: '%s'", ep);
    return v;
}

/* ------------------------------------------------------------------ statements */
static void trim(char *s)
{
    char *b = s, *e;
    while (*b == ' ' || *b == '\t') b++;
    memmove(s, b, strlen(b) + 1);
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
}

static int split_ops(char *s, char **v, int max)
{
    int n = 0, depth = 0; char *p = s, *start = s;
    if (!*s) return 0;
    for (;; p++) {
        if (*p == '(' || *p == '[') depth++;
        else if (*p == ')' || *p == ']') depth--;
        if ((*p == ',' && depth == 0) || *p == 0) {
            int end = *p == 0;
            *p = 0;
            if (n == max) { err("more than %d operands", max); return n; }
            v[n++] = start; start = p + 1;
            if (end) break;
        }
    }
    return n;
}

static void emit(ssp_u16 w)
{
    if (pc < 0 || pc >= IMG_WORDS) { err("word address 0x%lX outside the image", pc); pc++; return; }
    if (pass == 2) {
        if (touched[pc]) err("word 0x%05lX written twice", pc);
        img[pc] = w; touched[pc] = 1;
        if (lo_word < 0 || pc < lo_word) lo_word = pc;
        if (pc > hi_word) hi_word = pc;
    }
    pc++;
}

/* assemble one source line (comment already removed); returns the number of words it occupies */
static int statement(char *line)
{
    char *c, *mn, *rest, *ops[64]; int nops, i, nw = 0;
    trim(line);
    if (!*line) return 0;
    for (c = line; *c && (isalnum((unsigned char)*c) || *c == '_' || *c == '.'); c++) ;
    if (*c == ':' && c > line) {
        *c = 0; sym_set(line, pc);
        line = c + 1; trim(line);
        if (!*line) return 0;
    }
    {
        char *sp = line, *q;
        while (*sp && !isspace((unsigned char)*sp)) sp++;
        q = sp; while (isspace((unsigned char)*q)) q++;
        if (*sp && !strncmp(q, "equ", 3) && isspace((unsigned char)q[3])) { *sp = 0; sym_set(line, eval(q + 4)); return 0; }
    }
    mn = line; rest = line;
    while (*rest && !isspace((unsigned char)*rest)) rest++;
    if (*rest) { *rest++ = 0; trim(rest); }
    nops = split_ops(rest, ops, 64);
    if (!strcmp(mn, "org")) { pc = eval(rest); return 0; }
    if (!strcmp(mn, "dw")) {
        for (i = 0; i < nops; i++) {
            long v = eval(ops[i]);
            if (pass == 2 && (v < -32768 || v > 65535)) err("dw value %ld does not fit 16 bits", v);
            emit((ssp_u16)v);
        }
        return nops;
    }
    {
        ssp_opnd o[3]; long val[3] = { 0, 0, 0 }; ssp_u16 w[2]; char e[200]; int is_mod;
        if (nops > 3) { err("%s: too many operands", mn); return 0; }
        for (i = 0; i < 3; i++) ssp_parse_opnd(i < nops ? ops[i] : "", &o[i]);
        is_mod = !strcmp(mn, "mod") || !strcmp(mn, "MOD");
        if (pass == 1) {
            int sz = ssp_size(mn, nops, o, e, sizeof e);
            if (!sz) { err("%s", e); return 0; }
            pc += sz; return sz;
        }
        for (i = 0; i < nops; i++)
            if ((o[i].kind == SO_EXPR && !(is_mod && i == 1)) || o[i].kind == SO_RAM) val[i] = eval(o[i].text);
        if (ssp_encode(mn, nops, o, val, w, &nw, e, sizeof e)) { err("%s", e); pc += ssp_size(mn, nops, o, e, sizeof e); return 0; }
        emit(w[0]); if (nw == 2) emit(w[1]);
    }
    return nw;
}

/* ------------------------------------------------------------------ asm */
static ssp_u8 *load_file(const char *path, long *size)
{
    FILE *f = fopen(path, "rb"); ssp_u8 *b; long n;
    if (!f) { fprintf(stderr, "ssp16as: cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    b = xmalloc((size_t)n + 2);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "ssp16as: cannot read %s\n", path); exit(1); }
    fclose(f); *size = n;
    return b;
}

static void strip_comment(char *s)
{
    char *p; int q = 0;
    for (p = s; *p; p++) {
        if (*p == '"') q = !q;
        else if (!q && (*p == ';' || *p == '#')) { *p = 0; return; }
    }
}

static void write_listing(const char *path, const char *src, espp_line *lines, int n, const long *start, const int *count)
{
    FILE *f = fopen(path, "w"); int i, k; char stamp[32], dis[96]; time_t now = time(0);
    if (!f) { fprintf(stderr, "ssp16as: cannot write %s\n", path); exit(1); }
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(f, ";  SSP1601 listing\n;  source : %s\n;  built  : %s\n;  words  : 0x%04lX-0x%04lX\n", src, stamp, lo_word, hi_word);
    fprintf(f, ";  ADDR  WORDS       DISASSEMBLY                         SOURCE\n");
    for (i = 0; i < n; i++) {
        char text[1024];
        strncpy(text, lines[i].text, sizeof text - 1); text[sizeof text - 1] = 0; trim(text);
        if (!count[i]) { if (*text) fprintf(f, "%57s%s\n", "", text); continue; }
        for (k = 0; k < count[i]; ) {
            long a = start[i] + k; int sz = ssp_canon(&img[a], dis, sizeof dis);
            if (sz > count[i] - k) { snprintf(dis, sizeof dis, "dw 0x%04X", img[a]); sz = 1; }
            if (sz == 2) fprintf(f, "   %04lX  %04X %04X   %-36s%s\n", a, img[a], img[a + 1], dis, k ? "" : text);
            else fprintf(f, "   %04lX  %04X        %-36s%s\n", a, img[a], dis, k ? "" : text);
            k += sz;
        }
    }
    fclose(f);
}

static int cmd_asm(int argc, char **argv)
{
    const char *out = "out.bin", *fmt = NULL, *base = NULL, *lst = NULL, *src; long size = 0, i;
    espp_line *lines; int nl = 0, c, k; char e[1200]; long *start; int *count;
    while ((c = getopt(argc, argv, "o:f:b:s:l:I:D:")) != -1) {
        switch (c) {
        case 'o': out = optarg; break;
        case 'f': fmt = optarg; break;
        case 'b': base = optarg; break;
        case 's': size = strtol(optarg, 0, 0); break;
        case 'l': lst = optarg; break;
        case 'I': espp_add_incdir(optarg); break;
        case 'D': if (espp_define(optarg)) { fprintf(stderr, "ssp16as asm: bad -D %s\n", optarg); return 2; } break;
        default: return 2;
        }
    }
    if (optind != argc - 1) { fprintf(stderr, "ssp16as asm: one source file\n"); return 2; }
    src = argv[optind];
    if (!fmt) fmt = base ? "image" : "raw";
    if (strcmp(fmt, "image") && strcmp(fmt, "raw") && strcmp(fmt, "s")) { fprintf(stderr, "ssp16as asm: unknown format %s\n", fmt); return 2; }
    lines = espp_run(src, &nl, e, sizeof e);
    for (k = 0; k < espp_nwarn(); k++) fprintf(stderr, "%s\n", espp_warn(k));
    if (!lines) { fprintf(stderr, "%s\n", e); return 1; }
    img = xmalloc((size_t)IMG_WORDS * 2 + 4); touched = xmalloc((size_t)IMG_WORDS + 2);
    start = xmalloc(sizeof *start * (size_t)(nl + 1)); count = xmalloc(sizeof *count * (size_t)(nl + 1));
    for (pass = 1; pass <= 2; pass++) {
        pc = 0x0400;
        for (k = 0; k < nl; k++) {
            char buf[1024];
            strncpy(buf, lines[k].text, sizeof buf - 1); buf[sizeof buf - 1] = 0;
            strip_comment(buf);
            cur_file = lines[k].file; cur_line = lines[k].line;
            start[k] = pc;
            count[k] = statement(buf);
            if (count[k]) start[k] = pc - count[k];
        }
        if (errors) { fprintf(stderr, "ssp16as asm: %d error(s)\n", errors); return 1; }
    }
    if (lo_word < 0) { fprintf(stderr, "ssp16as asm: no words assembled\n"); return 1; }
    {
        FILE *f = fopen(out, "wb");
        if (!f) { fprintf(stderr, "ssp16as: cannot write %s\n", out); return 1; }
        if (!strcmp(fmt, "image")) {             /* BASE with the assembled words placed at byte 2w */
            ssp_u8 *b = NULL; long bsz = 0, n;
            if (base) b = load_file(base, &bsz);
            n = (hi_word + 1) * 2; if (bsz > n) n = bsz; if (size > n) n = size;
            for (i = 0; i < n; i += 2) {
                ssp_u8 w2[2] = { 0, 0 };
                if (touched[i / 2]) wr16be(w2, img[i / 2]);
                else if (i + 1 < bsz) { w2[0] = b[i]; w2[1] = b[i + 1]; }
                else if (i < bsz) w2[0] = b[i];
                fwrite(w2, 1, (size_t)(i + 1 < n ? 2 : 1), f);
            }
            free(b);
        } else if (!strcmp(fmt, "raw")) {        /* words lo..hi, big-endian: .incbin at byte 2 * lo */
            for (i = lo_word; i <= hi_word; i++) { ssp_u8 w2[2]; wr16be(w2, img[i]); fwrite(w2, 1, 2, f); }
        } else {                                 /* GNU as source for the 68000 side */
            fprintf(f, "| generated by ssp16as from %s: SSP words 0x%04lX-0x%04lX\n\t.org\t0x%lX\n", src, lo_word, hi_word, lo_word * 2);
            for (i = lo_word; i <= hi_word; i++)
                fprintf(f, "%s0x%04X%s", (i - lo_word) % 8 ? ", " : "\t.word\t", img[i], ((i - lo_word) % 8 == 7 || i == hi_word) ? "\n" : "");
        }
        fclose(f);
    }
    if (lst) write_listing(lst, src, lines, nl, start, count);
    return 0;
}

/* ------------------------------------------------------------------ dasm */
static int cmd_dasm(int argc, char **argv)
{
    const char *out = NULL, *rng = NULL; long first = 0x400, end = 0x10000, n, a; int c; ssp_u8 *b; ssp_u16 *w; FILE *f = stdout;
    while ((c = getopt(argc, argv, "o:r:")) != -1) {
        if (c == 'o') out = optarg;
        else if (c == 'r') rng = optarg;
        else return 2;
    }
    if (optind != argc - 1) { fprintf(stderr, "ssp16as dasm: one image file\n"); return 2; }
    if (rng) {
        char *colon; first = strtol(rng, &colon, 0);
        if (*colon != ':') { fprintf(stderr, "ssp16as dasm: -r FIRST:END\n"); return 2; }
        end = strtol(colon + 1, 0, 0);
    }
    b = load_file(argv[optind], &n);
    w = xmalloc(sizeof *w * (size_t)(n / 2 + 2));
    for (a = 0; a < n / 2; a++) w[a] = rd16be(b + 2 * a);
    if (end > n / 2) end = n / 2;
    if (out && !(f = fopen(out, "w"))) { fprintf(stderr, "ssp16as: cannot write %s\n", out); return 1; }
    fprintf(f, "; ssp16as dasm %s, words 0x%04lX-0x%04lX\n\torg 0x%04lX\n", argv[optind], first, end, first);
    for (a = first; a < end; ) {
        char t[96]; int sz = ssp_canon(&w[a], t, sizeof t);
        if (a + sz > end) { snprintf(t, sizeof t, "dw 0x%04X", w[a]); sz = 1; }
        fprintf(f, "\t%-40s; %04lX: %04X", t, a, w[a]);
        if (sz == 2) fprintf(f, " %04X", w[a + 1]);
        fputc('\n', f);
        a += sz;
    }
    if (out) fclose(f);
    return 0;
}

static int cmd_isa(void)
{
    int i;
    printf("reg"); for (i = 0; i < 16; i++) printf(" %s", ssp_reg[i]);
    printf("\nalias"); for (i = 0; i < 16; i++) if (ssp_regalias[i]) printf(" %s=%s", ssp_regalias[i], ssp_reg[i]);
    printf("\nptr r0 r1 r2 r3 r4 r5 r6 r7\nmodifier + +! - |00 |01 |10 |11\ncond");
    for (i = 0; i < 16; i++) printf(" %s", ssp_cond[i]);
    printf("\naccop"); for (i = 0; i < 8; i++) printf(" %s", ssp_accop[i]);
    printf("\nflagop"); for (i = 0; i < 16; i++) if (ssp_flagop[i]) printf(" %s", ssp_flagop[i]);
    printf("\nalu"); for (i = 0; i < 8; i++) if (ssp_aluop[i]) printf(" %s %si", ssp_aluop[i], ssp_aluop[i]);
    printf("\ninsn nop ret ld ldi call bra mod mld mpya mpys\n"
           "directive org dw equ .include .equ .define .set .undef .macro .endm .rept .endr .ifdef .ifndef .if .else .endif .error .warning\n");
    return 0;
}

static void usage(FILE *f)
{
    fputs("ssp16as -- SSP1601 (SVP) assembler and disassembler\n"
          "  ssp16as asm  [-o OUT] [-f image|raw|s] [-b BASE] [-s BYTES] [-l LST] [-I DIR] [-D N[=V]] FILE.s\n"
          "       image: BASE (a cartridge image) with the assembled words placed at byte 2w (default with -b)\n"
          "       raw:   the words from the lowest to the highest address, big-endian (default without -b)\n"
          "       s:     GNU as source (.org, .word) for the 68000 side\n", f);
    fputs("  ssp16as dasm [-o OUT] [-r FIRST:END] FILE.bin    canonical source of words FIRST..END-1\n"
          "  ssp16as isa                                     the instruction set's names\n", f);
}

int main(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "help")) { usage(argc < 2 ? stderr : stdout); return argc < 2 ? 2 : 0; }
    if (!strcmp(argv[1], "asm"))  return cmd_asm(argc - 1, argv + 1);
    if (!strcmp(argv[1], "dasm")) return cmd_dasm(argc - 1, argv + 1);
    if (!strcmp(argv[1], "isa"))  return cmd_isa();
    fprintf(stderr, "ssp16as: unknown subcommand '%s'\n", argv[1]);
    usage(stderr);
    return 2;
}
