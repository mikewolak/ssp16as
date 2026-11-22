/* espp.h  -  the esdsp source preprocessor: include files, symbols, macros, repeats and conditionals in front of
 * the ES5510 assembler.  Project panologicdp2, Mike Wolak, EPROM Foundry Inc.
 *
 * espp_run() reads a source file and returns its lines after the directives are applied, each carrying the file
 * and line it came from.  A source with no directive lines comes back line for line unchanged, so the assembler's
 * output for it is byte-identical to the output without the preprocessor.  C89, libc only (escompat.h floor).
 *
 * Directives (case-insensitive, first token of a line, ';' and '#' comments allowed after them):
 *   .include "FILE" | <FILE> | FILE   FILE relative to the including file's directory, then each -I directory
 *   .equ NAME VALUE      also NAME .equ VALUE, NAME = VALUE, .define NAME VALUE, .set NAME VALUE: NAME is replaced
 *                        by VALUE as a whole word in the code part of later lines (not in comments)
 *   .undef NAME
 *   .macro NAME [P1[, P2 ...]]  ...  .endm   a macro; a line whose first token is NAME expands it with the
 *                        comma-separated arguments, each Pn replaced as \Pn or as a whole word; \@ is the expansion
 *                        number; macros may invoke macros
 *   .rept N  ...  .endr  the enclosed lines N times
 *   .ifdef NAME | .ifndef NAME | .if EXPR  ...  [.else]  ...  .endif   EXPR: an integer, or A == B, A != B, A < B,
 *                        A > B, A <= B, A >= B on integers (decimal, 0x hex, $ hex)
 *   .error "TEXT"        stops with TEXT;  .warning "TEXT" reports TEXT and continues
 */
#ifndef ESPP_H
#define ESPP_H

typedef struct {
    char       *text;      /* the line, comment kept */
    const char *file;      /* where it came from (an interned path; for a macro body, the invocation's file) */
    int         line;
} espp_line;

/* include directories and predefined symbols, before espp_run */
void espp_add_incdir(const char *dir);
int  espp_define(const char *name_eq_value);          /* "NAME" or "NAME=VALUE"; 0 ok */

/* the expanded source; NULL on an error, with "FILE:LINE: message" in err */
espp_line *espp_run(const char *path, int *count, char *err, int errsz);

/* warnings collected during the last espp_run, "FILE:LINE: warning: text" each */
int         espp_nwarn(void);
const char *espp_warn(int i);

#endif /* ESPP_H */
