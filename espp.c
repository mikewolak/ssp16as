/* espp.c  -  the esdsp source preprocessor (espp.h).  Project panologicdp2, Mike Wolak, EPROM Foundry Inc.
 *
 * 1 SOURCES.  A source is an array of lines, each with its file and line number.  A file is read whole; a macro
 *   expansion or a repeat is a source built from captured lines.  process() walks a source once, keeping the
 *   conditional stack, the capture of a .macro or .rept body, and the output.
 * 2 LINES.  A line's code part ends at its first ';', then at a '#' inside what is left (the assembler's own rule);
 *   directives and symbol replacement look only at the code part, and a line with nothing replaced is emitted as
 *   read.  A replaced line is the new code part followed by the original comment, delimiter included.
 * 3 LIMITS.  Include and macro nesting 32, symbols 1024, macros 256 with 32 parameters, a line 1023 characters.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "espp.h"

#define MAXLINE  1024
#define MAXDEPTH 32
#define MAXSYM   1024
#define MAXMAC   256
#define MAXPARAM 32
#define MAXINC   64
#define MAXCOND  64
#define MAXWARN  256

typedef struct { char *name, *value; } sym;
typedef struct {
    char  *name;
    int    nparam;
    char  *param[MAXPARAM];
    espp_line *body;
    int    nbody;
} mac;

static sym   syms[MAXSYM];
static int   nsym;
static mac   macs[MAXMAC];
static int   nmac;
static char *incdir[MAXINC];
static int   nincdir;
static char *interned[4096];
static int   ninterned;
static char *warns[MAXWARN];
static int   nwarns;
static long  expansions;

static espp_line *out;
static int nout, capout;
static char *errbuf;
static int errlen;
static int failed;

/* ---- helpers ------------------------------------------------------------------------------------------------- */
static char *dupstr(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (!d) { fprintf(stderr, "esdsp: out of memory\n"); exit(1); }
    memcpy(d, s, n);
    return d;
}

static const char *intern(const char *s)
{
    int i;
    for (i = 0; i < ninterned; i++) if (!strcmp(interned[i], s)) return interned[i];
    if (ninterned < (int)(sizeof interned / sizeof interned[0])) { interned[ninterned] = dupstr(s); return interned[ninterned++]; }
    return dupstr(s);
}

static void fail(const char *file, int line, const char *msg, const char *arg)
{
    if (failed) return;
    failed = 1;
    if (errbuf && errlen > 0) {
        char t[MAXLINE + 256];
        sprintf(t, "%s:%d: %s%s%s", file, line, msg, arg ? " " : "", arg ? arg : "");
        strncpy(errbuf, t, (size_t)errlen - 1);
        errbuf[errlen - 1] = '\0';
    }
}

static void warn(const char *file, int line, const char *msg)
{
    char t[MAXLINE + 256];
    if (nwarns >= MAXWARN) return;
    sprintf(t, "%s:%d: warning: %s", file, line, msg);
    warns[nwarns++] = dupstr(t);
}

static void emit(const char *text, const char *file, int line)
{
    if (nout == capout) {
        capout = capout ? capout * 2 : 256;
        out = (espp_line *)realloc(out, (size_t)capout * sizeof *out);
        if (!out) { fprintf(stderr, "esdsp: out of memory\n"); exit(1); }
    }
    out[nout].text = dupstr(text);
    out[nout].file = file;
    out[nout].line = line;
    nout++;
}

static int isword(int c) { return isalnum(c) || c == '_' || c == '.' || c == '$'; }

/* the code part's end: the first ';', then a '#' before it (the assembler's rule) */
static size_t code_end(const char *s)
{
    const char *semi = strchr(s, ';');
    size_t n = semi ? (size_t)(semi - s) : strlen(s);
    const char *hash = (const char *)memchr(s, '#', n);
    if (hash) n = (size_t)(hash - s);
    return n;
}

/* the first token of the code part into tok, the rest (left-trimmed, code part only) into rest */
static void split_first(const char *s, char *tok, char *rest)
{
    char code[MAXLINE];
    size_t n = code_end(s), i = 0, k = 0;
    memcpy(code, s, n);
    code[n] = '\0';
    while (code[i] && isspace((unsigned char)code[i])) i++;
    while (code[i] && !isspace((unsigned char)code[i]) && code[i] != '=' && code[i] != ',') tok[k++] = code[i++];
    tok[k] = '\0';
    while (code[i] && isspace((unsigned char)code[i])) i++;
    strcpy(rest, code + i);
    k = strlen(rest);
    while (k && isspace((unsigned char)rest[k - 1])) rest[--k] = '\0';
}

static int ieq(const char *a, const char *b)
{
    while (*a && *b) { if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0; a++; b++; }
    return *a == *b;
}

static int find_sym(const char *name)
{
    int i;
    for (i = 0; i < nsym; i++) if (!strcmp(syms[i].name, name)) return i;
    return -1;
}

static int set_sym(const char *name, const char *value)
{
    int i = find_sym(name);
    if (i >= 0) { free(syms[i].value); syms[i].value = dupstr(value); return 0; }
    if (nsym >= MAXSYM) return -1;
    syms[nsym].name = dupstr(name);
    syms[nsym].value = dupstr(value);
    nsym++;
    return 0;
}

static int find_mac(const char *name)
{
    int i;
    for (i = 0; i < nmac; i++) if (ieq(macs[i].name, name)) return i;
    return -1;
}

/* whole-word replacement of the symbols in s (code part only), repeated for chained symbols; 1 if changed */
static int subst_syms(char *s)
{
    int changed = 0, pass;
    for (pass = 0; pass < 8 && nsym; pass++) {
        char t[MAXLINE];
        size_t i = 0, k = 0;
        int any = 0;
        while (s[i] && k < MAXLINE - 1) {
            if (isword((unsigned char)s[i]) && (i == 0 || !isword((unsigned char)s[i - 1]))) {
                size_t j = i;
                char w[MAXLINE];
                int si;
                while (isword((unsigned char)s[j])) j++;
                memcpy(w, s + i, j - i);
                w[j - i] = '\0';
                si = find_sym(w);
                if (si >= 0) {
                    size_t vl = strlen(syms[si].value);
                    if (k + vl >= MAXLINE - 1) break;
                    memcpy(t + k, syms[si].value, vl);
                    k += vl;
                    any = 1;
                } else {
                    if (k + (j - i) >= MAXLINE - 1) break;
                    memcpy(t + k, w, j - i);
                    k += j - i;
                }
                i = j;
            } else t[k++] = s[i++];
        }
        t[k] = '\0';
        if (!any) break;
        strcpy(s, t);
        changed = 1;
    }
    return changed;
}

static int parse_int(const char *s, long *v)
{
    char *e;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '$') *v = strtol(s + 1, &e, 16);
    else *v = strtol(s, &e, 0);
    if (e == s) return -1;
    while (isspace((unsigned char)*e)) e++;
    return *e ? -1 : 0;
}

/* .if: an integer, or two integers with == != < > <= >= */
static int eval_if(const char *expr, int *res)
{
    static const char *ops[] = { "==", "!=", "<=", ">=", "<", ">" };
    char a[MAXLINE];
    long x, y;
    int k;
    for (k = 0; k < 6; k++) {
        const char *p = strstr(expr, ops[k]);
        if (p) {
            size_t n = (size_t)(p - expr);
            memcpy(a, expr, n);
            a[n] = '\0';
            if (parse_int(a, &x) || parse_int(p + strlen(ops[k]), &y)) return -1;
            switch (k) {
            case 0: *res = x == y; break;
            case 1: *res = x != y; break;
            case 2: *res = x <= y; break;
            case 3: *res = x >= y; break;
            case 4: *res = x < y; break;
            default: *res = x > y; break;
            }
            return 0;
        }
    }
    if (parse_int(expr, &x)) return -1;
    *res = x != 0;
    return 0;
}

/* a quoted, <bracketed> or bare argument */
static void unquote(const char *s, char *d)
{
    size_t n;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '"' || *s == '<') {
        char close = *s == '"' ? '"' : '>';
        const char *e = strchr(s + 1, close);
        n = e ? (size_t)(e - s - 1) : strlen(s + 1);
        memcpy(d, s + 1, n);
        d[n] = '\0';
        return;
    }
    strcpy(d, s);
    n = strlen(d);
    while (n && isspace((unsigned char)d[n - 1])) d[--n] = '\0';
}

static char *dir_of(const char *path)
{
    char *d = dupstr(path);
    char *s = strrchr(d, '/');
    if (s) s[1] = '\0'; else d[0] = '\0';
    return d;
}

/* ---- sources ------------------------------------------------------------------------------------------------- */
static espp_line *read_file(const char *path, int *n)
{
    FILE *f = fopen(path, "r");
    char raw[MAXLINE];
    espp_line *v = 0;
    int cap = 0, cnt = 0, ln = 0;
    const char *ip;
    if (!f) return 0;
    ip = intern(path);
    while (fgets(raw, sizeof raw, f)) {
        size_t k = strlen(raw);
        ln++;
        while (k && (raw[k - 1] == '\n' || raw[k - 1] == '\r')) raw[--k] = '\0';
        if (cnt == cap) {
            cap = cap ? cap * 2 : 128;
            v = (espp_line *)realloc(v, (size_t)cap * sizeof *v);
            if (!v) { fprintf(stderr, "esdsp: out of memory\n"); exit(1); }
        }
        v[cnt].text = dupstr(raw);
        v[cnt].file = ip;
        v[cnt].line = ln;
        cnt++;
    }
    fclose(f);
    *n = cnt;
    return v;
}

static const char *inc_stack[MAXDEPTH];
static int ninc_stack;

static void process(const espp_line *src, int n, int depth);

static void do_include(const char *arg, const char *file, int line, int depth)
{
    char name[MAXLINE], path[2 * MAXLINE];
    espp_line *v;
    int cnt = 0, i;
    FILE *f;
    unquote(arg, name);
    if (!name[0]) { fail(file, line, ".include needs a file name", 0); return; }
    path[0] = '\0';
    if (name[0] == '/') strcpy(path, name);
    else {
        char *d = dir_of(file);
        sprintf(path, "%s%s", d, name);
        free(d);
        f = fopen(path, "r");
        if (f) fclose(f);
        else {
            path[0] = '\0';
            for (i = 0; i < nincdir; i++) {
                sprintf(path, "%s/%s", incdir[i], name);
                f = fopen(path, "r");
                if (f) { fclose(f); break; }
                path[0] = '\0';
            }
        }
    }
    if (!path[0]) { fail(file, line, "cannot find include file", name); return; }
    if (depth >= MAXDEPTH || ninc_stack >= MAXDEPTH) { fail(file, line, "includes nested too deeply at", name); return; }
    for (i = 0; i < ninc_stack; i++)
        if (!strcmp(inc_stack[i], intern(path))) { fail(file, line, "recursive include of", name); return; }
    v = read_file(path, &cnt);
    if (!v) { fail(file, line, "cannot open include file", path); return; }
    inc_stack[ninc_stack++] = intern(path);
    process(v, cnt, depth + 1);
    ninc_stack--;
    for (i = 0; i < cnt; i++) free(v[i].text);
    free(v);
}

/* the arguments of a macro invocation, comma-separated, trimmed */
static int split_args(const char *s, char args[][MAXLINE], int max)
{
    int n = 0;
    while (*s && n < max) {
        const char *c = strchr(s, ',');
        size_t len = c ? (size_t)(c - s) : strlen(s);
        char *d = args[n];
        while (len && isspace((unsigned char)*s)) { s++; len--; }
        memcpy(d, s, len);
        d[len] = '\0';
        while (len && isspace((unsigned char)d[len - 1])) d[--len] = '\0';
        n++;
        if (!c) break;
        s = c + 1;
    }
    return n;
}

static void expand_macro(int mi, const char *rest, const char *file, int line, int depth)
{
    mac *m = &macs[mi];
    char (*args)[MAXLINE] = (char (*)[MAXLINE])malloc(sizeof(char[MAXLINE]) * MAXPARAM);
    espp_line *body;
    int na = 0, i, p;
    char num[16];
    if (!args) { fprintf(stderr, "esdsp: out of memory\n"); exit(1); }
    if (depth >= MAXDEPTH) { fail(file, line, "macros nested too deeply in", m->name); free(args); return; }
    if (rest[0]) na = split_args(rest, args, MAXPARAM);
    if (na > m->nparam) { fail(file, line, "too many arguments to macro", m->name); free(args); return; }
    for (i = na; i < m->nparam; i++) args[i][0] = '\0';
    expansions++;
    sprintf(num, "%ld", expansions);
    body = (espp_line *)malloc(sizeof *body * (size_t)(m->nbody ? m->nbody : 1));
    if (!body) { fprintf(stderr, "esdsp: out of memory\n"); exit(1); }
    for (i = 0; i < m->nbody; i++) {
        const char *s = m->body[i].text;
        size_t ce = code_end(s), k = 0, j = 0;
        char t[MAXLINE];
        while (j < ce && k < MAXLINE - 1) {
            int done = 0;
            if (s[j] == '\\' && s[j + 1] == '@') {
                size_t l = strlen(num);
                if (k + l < MAXLINE - 1) { memcpy(t + k, num, l); k += l; }
                j += 2;
                continue;
            }
            for (p = 0; p < m->nparam && !done; p++) {
                size_t pl = strlen(m->param[p]);
                int bs = s[j] == '\\';
                size_t at = j + (size_t)bs;
                if (at + pl <= ce && !strncmp(s + at, m->param[p], pl) && !isword((unsigned char)s[at + pl]) &&
                    (bs || j == 0 || !isword((unsigned char)s[j - 1]))) {
                    size_t al = strlen(args[p]);
                    if (k + al < MAXLINE - 1) { memcpy(t + k, args[p], al); k += al; }
                    j = at + pl;
                    done = 1;
                }
            }
            if (!done) t[k++] = s[j++];
        }
        t[k] = '\0';
        if (strlen(t) + strlen(s + ce) < MAXLINE) strcat(t, s + ce);
        body[i].text = dupstr(t);
        body[i].file = file;
        body[i].line = line;
    }
    process(body, m->nbody, depth + 1);
    for (i = 0; i < m->nbody; i++) free(body[i].text);
    free(body);
    free(args);
}

static void process(const espp_line *src, int n, int depth)
{
    int cond[MAXCOND], taken[MAXCOND], ncond = 0;
    int cap_kind = 0, cap_nest = 0, cap_from = 0, cap_mac = -1;
    long rept_n = 0;
    int i;
    for (i = 0; i < n && !failed; i++) {
        const char *s = src[i].text, *file = src[i].file;
        int line = src[i].line, active = 1, k;
        char tok[MAXLINE], rest[MAXLINE], name[MAXLINE], t[MAXLINE];
        for (k = 0; k < ncond; k++) if (!cond[k]) active = 0;
        split_first(s, tok, rest);

        /* a .macro or .rept body: captured up to its matching end */
        if (cap_kind) {
            if (ieq(tok, ".macro") || ieq(tok, ".rept")) cap_nest++;
            else if ((ieq(tok, ".endm") || ieq(tok, ".endr")) && cap_nest > 0) cap_nest--;
            else if (cap_kind == 1 && ieq(tok, ".endm")) {
                mac *m = &macs[cap_mac];
                int j, cnt = i - cap_from;
                m->body = (espp_line *)malloc(sizeof *m->body * (size_t)(cnt ? cnt : 1));
                if (!m->body) { fprintf(stderr, "esdsp: out of memory\n"); exit(1); }
                for (j = 0; j < cnt; j++) {
                    m->body[j].text = dupstr(src[cap_from + j].text);
                    m->body[j].file = src[cap_from + j].file;
                    m->body[j].line = src[cap_from + j].line;
                }
                m->nbody = cnt;
                cap_kind = 0;
                continue;
            } else if (cap_kind == 2 && ieq(tok, ".endr")) {
                long r;
                cap_kind = 0;
                for (r = 0; r < rept_n && !failed; r++) process(src + cap_from, i - cap_from, depth + 1);
                continue;
            } else if ((cap_kind == 1 && ieq(tok, ".endr")) || (cap_kind == 2 && ieq(tok, ".endm"))) {
                fail(file, line, "mismatched", tok);
                return;
            }
            continue;
        }

        /* conditionals, evaluated even in skipped regions so the nesting holds */
        if (ieq(tok, ".ifdef") || ieq(tok, ".ifndef") || ieq(tok, ".if")) {
            int v = 0;
            if (ncond >= MAXCOND) { fail(file, line, "conditionals nested too deeply", 0); return; }
            if (active) {
                if (ieq(tok, ".if")) {
                    strcpy(t, rest);
                    subst_syms(t);
                    if (eval_if(t, &v)) { fail(file, line, ".if cannot evaluate", rest); return; }
                } else {
                    unquote(rest, name);
                    v = find_sym(name) >= 0 || find_mac(name) >= 0;
                    if (ieq(tok, ".ifndef")) v = !v;
                }
            }
            cond[ncond] = v;
            taken[ncond] = v;
            ncond++;
            continue;
        }
        if (ieq(tok, ".else")) {
            if (!ncond) { fail(file, line, ".else without .if", 0); return; }
            cond[ncond - 1] = !taken[ncond - 1];
            taken[ncond - 1] = 1;
            continue;
        }
        if (ieq(tok, ".endif")) {
            if (!ncond) { fail(file, line, ".endif without .if", 0); return; }
            ncond--;
            continue;
        }
        if (!active) continue;

        if (ieq(tok, ".include")) { do_include(rest, file, line, depth); continue; }
        if (ieq(tok, ".equ") || ieq(tok, ".define") || ieq(tok, ".set")) {
            char *v;
            size_t j = 0;
            while (rest[j] && !isspace((unsigned char)rest[j]) && rest[j] != ',' && rest[j] != '=') j++;
            memcpy(name, rest, j);
            name[j] = '\0';
            v = rest + j;
            while (*v && (isspace((unsigned char)*v) || *v == ',' || *v == '=')) v++;
            if (!name[0]) { fail(file, line, "missing symbol name after", tok); return; }
            strcpy(t, v);
            subst_syms(t);
            if (set_sym(name, t)) { fail(file, line, "too many symbols at", name); return; }
            continue;
        }
        if (ieq(tok, ".undef")) {
            int si;
            unquote(rest, name);
            si = find_sym(name);
            if (si >= 0) { free(syms[si].name); free(syms[si].value); syms[si] = syms[--nsym]; }
            continue;
        }
        {   /* NAME .equ VALUE, NAME = VALUE */
            char t2[MAXLINE], r2[MAXLINE];
            split_first(rest, t2, r2);
            if (tok[0] && !strchr(tok, '=') && (ieq(t2, ".equ") || ieq(t2, ".set") || (rest[0] == '=' && rest[1] != '='))) {
                const char *v = rest[0] == '=' ? rest + 1 : r2;
                while (*v && isspace((unsigned char)*v)) v++;
                strcpy(t, v);
                subst_syms(t);
                if (set_sym(tok, t)) { fail(file, line, "too many symbols at", tok); return; }
                continue;
            }
        }
        if (ieq(tok, ".macro")) {
            char args[MAXPARAM][MAXLINE];
            size_t j = 0;
            int na, a;
            mac *m;
            while (rest[j] && !isspace((unsigned char)rest[j]) && rest[j] != ',') j++;
            memcpy(name, rest, j);
            name[j] = '\0';
            if (!name[0]) { fail(file, line, ".macro needs a name", 0); return; }
            if (find_mac(name) >= 0) { fail(file, line, "macro defined twice:", name); return; }
            if (nmac >= MAXMAC) { fail(file, line, "too many macros at", name); return; }
            while (rest[j] && (isspace((unsigned char)rest[j]) || rest[j] == ',')) j++;
            na = rest[j] ? split_args(rest + j, args, MAXPARAM) : 0;
            m = &macs[nmac];
            memset(m, 0, sizeof *m);
            m->name = dupstr(name);
            m->nparam = na;
            for (a = 0; a < na; a++) m->param[a] = dupstr(args[a][0] == '\\' ? args[a] + 1 : args[a]);
            cap_mac = nmac++;
            cap_kind = 1;
            cap_nest = 0;
            cap_from = i + 1;
            continue;
        }
        if (ieq(tok, ".rept")) {
            strcpy(t, rest);
            subst_syms(t);
            if (parse_int(t, &rept_n) || rept_n < 0 || rept_n > 4096) { fail(file, line, ".rept needs a count 0-4096, not", rest); return; }
            cap_kind = 2;
            cap_nest = 0;
            cap_from = i + 1;
            continue;
        }
        if (ieq(tok, ".endm") || ieq(tok, ".endr")) { fail(file, line, "unmatched", tok); return; }
        if (ieq(tok, ".error")) { unquote(rest, name); fail(file, line, "error:", name); return; }
        if (ieq(tok, ".warning")) { unquote(rest, name); warn(file, line, name); continue; }
        if (tok[0] == '.' && tok[1] && isalpha((unsigned char)tok[1])) { fail(file, line, "unknown directive", tok); return; }
        {
            int mi = tok[0] ? find_mac(tok) : -1;
            if (mi >= 0) {
                strcpy(t, rest);
                subst_syms(t);
                expand_macro(mi, t, file, line, depth);
                continue;
            }
        }
        /* an ordinary line: symbols replaced in its code part, the comment kept */
        {
            size_t ce = code_end(s);
            if (ce >= MAXLINE) ce = MAXLINE - 1;
            memcpy(t, s, ce);
            t[ce] = '\0';
            if (subst_syms(t)) {
                if (strlen(t) + strlen(s + ce) < MAXLINE) strcat(t, s + ce);
                emit(t, file, line);
            } else emit(s, file, line);
        }
    }
    if (!failed && cap_kind)
        fail(src[cap_from > 0 ? cap_from - 1 : 0].file, src[cap_from > 0 ? cap_from - 1 : 0].line,
             cap_kind == 1 ? ".macro without .endm" : ".rept without .endr", 0);
    if (!failed && ncond)
        fail(n ? src[n - 1].file : "?", n ? src[n - 1].line : 0, ".if without .endif", 0);
}

/* ---- the interface ------------------------------------------------------------------------------------------- */
void espp_add_incdir(const char *dir)
{
    if (nincdir < MAXINC) incdir[nincdir++] = dupstr(dir);
}

int espp_define(const char *s)
{
    char name[MAXLINE];
    const char *eq = strchr(s, '=');
    size_t n = eq ? (size_t)(eq - s) : strlen(s);
    if (!n || n >= MAXLINE) return -1;
    memcpy(name, s, n);
    name[n] = '\0';
    return set_sym(name, eq ? eq + 1 : "1");
}

espp_line *espp_run(const char *path, int *count, char *err, int errsz)
{
    espp_line *v;
    int cnt = 0, i;
    errbuf = err;
    errlen = errsz;
    if (err && errsz) err[0] = '\0';
    failed = 0;
    out = 0;
    nout = capout = 0;
    ninc_stack = 0;
    v = read_file(path, &cnt);
    if (!v) { fail(path, 0, "cannot open", 0); return 0; }
    inc_stack[ninc_stack++] = intern(path);
    process(v, cnt, 0);
    for (i = 0; i < cnt; i++) free(v[i].text);
    free(v);
    if (failed) return 0;
    *count = nout;
    if (!out) out = (espp_line *)malloc(sizeof *out);
    return out;
}

int espp_nwarn(void) { return nwarns; }
const char *espp_warn(int i) { return (i >= 0 && i < nwarns) ? warns[i] : ""; }
