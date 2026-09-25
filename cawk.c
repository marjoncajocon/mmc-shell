/*
** cawk.c - awk: the POSIX language, with the gawk extensions people use
**
**   awk [-F fs] [-v var=value]... [-f progfile | 'program'] [file | var=value]...
** Tree-walking interpreter. Strings are counted and shared (reference
** counts). Commands run by system(), "cmd" | getline and print | "cmd"
** are run by mmc itself, so they work on Windows too.
** gawk extras: length(array), gensub, strftime, systime, tolower, toupper,
** fflush, nextfile, delete array, RS as a regex, RT, IGNORECASE, ** **=.
*/

#include "mmc.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
** {==================================================================
** Values
** ===================================================================
*/

typedef struct Str {
  int ref;
  size_t len;
  char s[1];
} Str;

#define AT_NUM		1	/* a number */
#define AT_STR		2	/* a string */
#define AT_STRNUM	4	/* a string from input that looks like a number */
#define AT_UNINIT	8	/* never set: "" and 0 */

typedef struct Val {
  int t;
  double n;
  Str *s;
} Val;

typedef struct AElem {
  Str *key;
  Val v;
  struct AElem *next;
} AElem;

typedef struct Array {
  AElem **tab;
  size_t cap, n;
} Array;

enum { AK_UNKNOWN, AK_SCALAR, AK_ARRAY };

typedef struct AVar {
  int kind;
  Val v;
  Array *arr;
  struct AVar *ref;	/* a parameter bound to the caller's untyped variable */
  int own_arr;
} AVar;


static Str *str_new (const char *s, size_t n) {
  Str *r = (Str *)xmalloc(sizeof(Str) + n);
  r->ref = 1;
  r->len = n;
  if (n) memcpy(r->s, s, n);
  r->s[n] = '\0';
  return r;
}


static Str *str_c (const char *s) {
  return str_new(s, strlen(s));
}


static Str *str_ref (Str *s) {
  if (s) s->ref++;
  return s;
}


static void str_unref (Str *s) {
  if (s && --s->ref == 0) free(s);
}


static Str *empty_str (void) {
  static Str *e = NULL;
  if (e == NULL) {
    e = str_new("", 0);
    e->ref = 1 << 30;
  }
  return str_ref(e);
}


static void val_free (Val *v) {
  if (v->t & (AT_STR | AT_STRNUM)) str_unref(v->s);
  v->s = NULL;
  v->t = AT_UNINIT;
}


static Val val_num (double n) {
  Val v;
  v.t = AT_NUM;
  v.n = n;
  v.s = NULL;
  return v;
}


static Val val_str (Str *s) {	/* takes s */
  Val v;
  v.t = AT_STR;
  v.n = 0;
  v.s = s;
  return v;
}


static Val val_copy (const Val *v) {
  Val r = *v;
  if (r.t & (AT_STR | AT_STRNUM)) str_ref(r.s);
  return r;
}


static Val val_uninit (void) {
  Val v;
  v.t = AT_UNINIT;
  v.n = 0;
  v.s = NULL;
  return v;
}


/* does this text look like a number (for input strings)? */
static int looks_numeric (const char *s, size_t len, double *out) {
  const char *p = s, *e = s + len;
  char *end;
  char buf[128];
  double v;
  while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v')) p++;
  if (p == e) return 0;
  {
    const char *q = p;
    if (*q == '+' || *q == '-') q++;
    if (!(q < e && (isdigit((unsigned char)*q) || (*q == '.' && q + 1 < e && isdigit((unsigned char)q[1])))))
      return 0;
    if (q + 1 < e && q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) {
      /* hex is not numeric input in POSIX awk: 0x10 is 0 */
    }
  }
  if ((size_t)(e - p) >= sizeof(buf)) {
    char *big = xstrndup(p, (size_t)(e - p));
    v = strtod(big, &end);
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') end++;
    if (*end) {
      free(big);
      return 0;
    }
    free(big);
  }
  else {
    memcpy(buf, p, (size_t)(e - p));
    buf[e - p] = '\0';
    if (strpbrk(buf, "xX")) return 0;
    v = strtod(buf, &end);
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') end++;
    if (*end) return 0;
  }
  *out = v;
  return 1;
}


/* a string that came from input: a strnum if it looks like a number */
static Val val_input (Str *s) {
  Val v = val_str(s);
  if (looks_numeric(s->s, s->len, &v.n)) v.t = AT_STRNUM;
  return v;
}


static double str_to_num (const char *s) {
  char *end;
  while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
  if ((s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ||
      ((s[0] == '+' || s[0] == '-') && s[1] == '0' && (s[2] == 'x' || s[2] == 'X')))
    return 0;	/* no hex, like POSIX awk */
  if (strncmp(s, "inf", 3) == 0 || strncmp(s, "nan", 3) == 0 || strncmp(s, "INF", 3) == 0 ||
      strncmp(s, "NAN", 3) == 0)
    return 0;
  return strtod(s, &end);
}

/* }================================================================== */


/*
** {==================================================================
** Arrays
** ===================================================================
*/

static unsigned hash_key (const char *s, size_t n) {
  unsigned h = 2166136261u;
  size_t i;
  for (i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
  return h;
}


static Array *arr_new (void) {
  Array *a = (Array *)xmalloc(sizeof(Array));
  a->cap = 16;
  a->n = 0;
  a->tab = (AElem **)xmalloc(a->cap * sizeof(AElem *));
  memset(a->tab, 0, a->cap * sizeof(AElem *));
  return a;
}


static void arr_clear (Array *a) {
  size_t i;
  for (i = 0; i < a->cap; i++) {
    AElem *e = a->tab[i];
    while (e) {
      AElem *nx = e->next;
      str_unref(e->key);
      val_free(&e->v);
      free(e);
      e = nx;
    }
    a->tab[i] = NULL;
  }
  a->n = 0;
}


static void arr_free (Array *a) {
  if (a == NULL) return;
  arr_clear(a);
  free(a->tab);
  free(a);
}


static AElem *arr_find (Array *a, const char *k, size_t n) {
  AElem *e = a->tab[hash_key(k, n) & (a->cap - 1)];
  for (; e; e = e->next)
    if (e->key->len == n && memcmp(e->key->s, k, n) == 0) return e;
  return NULL;
}


static AElem *arr_get (Array *a, Str *key) {	/* creates it */
  AElem *e = arr_find(a, key->s, key->len);
  unsigned h;
  if (e) return e;
  if (a->n + 1 > a->cap) {	/* grow */
    size_t nc = a->cap * 2, i;
    AElem **nt = (AElem **)xmalloc(nc * sizeof(AElem *));
    memset(nt, 0, nc * sizeof(AElem *));
    for (i = 0; i < a->cap; i++) {
      AElem *x = a->tab[i];
      while (x) {
        AElem *nx = x->next;
        unsigned hh = hash_key(x->key->s, x->key->len) & (unsigned)(nc - 1);
        x->next = nt[hh];
        nt[hh] = x;
        x = nx;
      }
    }
    free(a->tab);
    a->tab = nt;
    a->cap = nc;
  }
  h = hash_key(key->s, key->len) & (unsigned)(a->cap - 1);
  e = (AElem *)xmalloc(sizeof(AElem));
  e->key = str_ref(key);
  e->v = val_uninit();
  e->next = a->tab[h];
  a->tab[h] = e;
  a->n++;
  return e;
}


static void arr_delete (Array *a, const char *k, size_t n) {
  AElem **pp = &a->tab[hash_key(k, n) & (a->cap - 1)];
  for (; *pp; pp = &(*pp)->next) {
    AElem *e = *pp;
    if (e->key->len == n && memcmp(e->key->s, k, n) == 0) {
      *pp = e->next;
      str_unref(e->key);
      val_free(&e->v);
      free(e);
      a->n--;
      return;
    }
  }
}


/* the keys, in a stable order: numbers by value first, then strings */
static int key_cmp (const void *x, const void *y) {
  const Str *a = *(Str *const *)x, *b = *(Str *const *)y;
  double na, nb;
  int an = looks_numeric(a->s, a->len, &na), bn = looks_numeric(b->s, b->len, &nb);
  if (an && bn) return na < nb ? -1 : na > nb ? 1 : 0;
  if (an != bn) return an ? -1 : 1;
  return strcmp(a->s, b->s);
}


static Str **arr_keys (Array *a, size_t *n) {
  Str **v = (Str **)xmalloc((a->n + 1) * sizeof(Str *));
  size_t i, k = 0;
  for (i = 0; i < a->cap; i++) {
    AElem *e;
    for (e = a->tab[i]; e; e = e->next) v[k++] = str_ref(e->key);
  }
  qsort(v, k, sizeof(Str *), key_cmp);
  *n = k;
  return v;
}

/* }================================================================== */


/*
** {==================================================================
** The syntax tree
** ===================================================================
*/

enum {
  /* expressions */
  AN_NUM, AN_STR, AN_REGEX, AN_VAR, AN_LOCAL, AN_FIELD, AN_INDEX, AN_ASSIGN, AN_COND, AN_OR, AN_AND,
  AN_IN, AN_MATCH, AN_NOMATCH, AN_LT, AN_LE, AN_GT, AN_GE, AN_EQ, AN_NE, AN_CONCAT, AN_ADD, AN_SUB,
  AN_MUL, AN_DIV, AN_MOD, AN_POW, AN_NEG, AN_PLUS, AN_NOT, AN_PREINC, AN_PREDEC, AN_POSTINC,
  AN_POSTDEC, AN_CALL, AN_BUILTIN, AN_GETLINE, AN_GROUP,
  /* statements */
  AS_EXPR, AS_PRINT, AS_PRINTF, AS_IF, AS_WHILE, AS_DO, AS_FOR, AS_FORIN, AS_BREAK, AS_CONTINUE,
  AS_NEXT, AS_NEXTFILE, AS_EXIT, AS_RETURN, AS_DELETE, AS_BLOCK, AS_GETLINE
};

enum {
  AB_LENGTH, AB_SUBSTR, AB_INDEX, AB_SPLIT, AB_SUB, AB_GSUB, AB_MATCH, AB_SPRINTF, AB_SIN, AB_COS,
  AB_ATAN2, AB_EXP, AB_LOG, AB_SQRT, AB_INT, AB_RAND, AB_SRAND, AB_TOLOWER, AB_TOUPPER, AB_SYSTEM,
  AB_CLOSE, AB_FFLUSH, AB_GENSUB, AB_STRFTIME, AB_SYSTIME
};

/* getline kinds */
enum { AG_SIMPLE, AG_FILE, AG_CMD };
/* print redirections */
enum { AR_NONE, AR_FILE, AR_APPEND, AR_PIPE };

typedef struct ANode {
  int op;
  int line;
  struct ANode *a, *b, *c, *d;
  struct ANode *next;	/* lists: statements, arguments */
  double num;
  Str *str;
  Regex *re;
  int idx;	/* variable / local slot / function / builtin; assignment operator */
  int nargs;
  int kind;	/* getline kind, print redirection */
} ANode;

typedef struct AFunc {
  char *name;
  int nparams;
  char **params;
  ANode *body;
  int defined;
  int line;
} AFunc;

typedef struct ARule {
  int kind;	/* 0 main, 1 BEGIN, 2 END */
  ANode *pat, *pat2;	/* pat2: a range */
  ANode *action;	/* NULL: print */
  int in_range;
} ARule;

/* }================================================================== */


/*
** {==================================================================
** The interpreter's state
** ===================================================================
*/

enum {
  AG_NR, AG_NF, AG_FNR, AG_FS, AG_OFS, AG_ORS, AG_RS, AG_FILENAME, AG_SUBSEP, AG_RSTART, AG_RLENGTH,
  AG_CONVFMT, AG_OFMT, AG_ENVIRON, AG_ARGC, AG_ARGV, AG_RT, AG_IGNORECASE, AG_NSPECIAL
};

static const char *const special_names[] = {
  "NR", "NF", "FNR", "FS", "OFS", "ORS", "RS", "FILENAME", "SUBSEP", "RSTART", "RLENGTH",
  "CONVFMT", "OFMT", "ENVIRON", "ARGC", "ARGV", "RT", "IGNORECASE", NULL};

typedef struct OStream {	/* an output file or pipe */
  char *name;
  int kind;	/* AR_FILE, AR_APPEND, AR_PIPE */
  int fd;
  Buf pipe_data;	/* print | "cmd": what the command gets */
  Out o;
} OStream;

typedef struct IStream {	/* getline < file, "cmd" | getline */
  char *name;
  int is_cmd;
  In in;
  int open;
  char *data;	/* cmd: its whole output */
  size_t len, pos;
} IStream;

typedef struct AFrame {
  AVar *locals;
  int n;
} AFrame;

typedef struct Awk {
  /* program */
  ARule *rules;
  int nrules;
  AFunc *funcs;
  int nfuncs;
  AVar *globals;
  char **gnames;
  int nglobals, gcap;
  /* parse */
  const char *src, *p, *end;
  int line;
  const char *srcname;
  int tok;
  double tnum;
  Str *tstr;
  char *tname;
  int prev_tok;
  AFunc *cur_func;
  int in_loop;
  int print_ctx;	/* parsing a print list: '>' is a redirection */
  int in_paren;
  int errors;
  /* running */
  Val rec;	/* $0 */
  Val *fields;	/* $1 .. */
  int nf, fcap;
  int fields_ok;	/* $0 split into fields */
  int rec_ok;	/* $0 matches the fields */
  AFrame *frame;
  Val retval;
  int exit_code, exiting, in_end;
  int in, out, err;
  Out o;
  OStream *outs;
  int nouts;
  IStream *ins;
  int nins;
  /* main input */
  int argi;	/* the next ARGV to look at */
  In cur;
  int cur_open;
  int used_stdin;
  /* regex cache */
  struct { Str *key; Regex *re; int flags; } rcache[64];
  int rnext;
  unsigned rseed;
  double prev_seed;
  int status;
  int fatal_err;	/* a fatal error: no END */
  int cur_line;	/* of the statement running, for messages */
  Str *rec_fs;	/* the FS when the record was read: a new FS is for the next one */
} Awk;

static Awk *g_awk;	/* for the error functions */

static Str *to_str (Awk *A, const Val *v);
static double to_num (const Val *v);


/* a runtime error, as gawk says it: "awk: cmd. line:3: fatal: ..." */
static void fatal (Awk *A, const char *fmt, ...) {
  char msg[1024];
  va_list ap;
  const char *kind = "fatal";
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  out_flush(&A->o);
  if (strncmp(msg, "division by zero", 16) == 0) kind = "error";	/* gawk 5: exit status 1 */
  {	/* while reading records gawk says where */
    char where[600];
    where[0] = '\0';
    if (A->globals && to_num(&A->globals[AG_NR].v) > 0) {
      Str *fn = to_str(A, &A->globals[AG_FILENAME].v);
      snprintf(where, sizeof(where), "(FILENAME=%s FNR=%.0f) ", fn->len ? fn->s : "-",
               to_num(&A->globals[AG_FNR].v));
      str_unref(fn);
    }
    if (A->srcname) fd_printf(A->err, "awk: %s:%d: %s%s: %s\n", A->srcname, A->cur_line, where, kind, msg);
    else fd_printf(A->err, "awk: cmd. line:%d: %s%s: %s\n", A->cur_line, where, kind, msg);
  }
  A->exiting = 1;
  A->exit_code = kind[0] == 'e' ? 1 : 2;
  A->fatal_err = 1;
}

/* }================================================================== */


/*
** {==================================================================
** Lexer
** ===================================================================
*/

enum {
  AT_EOF = 256, AT_NEWLINE, AT_NUMBER, AT_STRING, AT_ERE, AT_NAME, AT_FUNC_NAME, AT_BUILTIN,
  AT_BEGIN, AT_END, AT_FUNCTION, AT_IF, AT_ELSE, AT_WHILE, AT_FOR, AT_DO, AT_BREAK, AT_CONTINUE,
  AT_NEXT, AT_NEXTFILE, AT_EXIT, AT_RETURN, AT_DELETE, AT_IN, AT_GETLINE, AT_PRINT, AT_PRINTF,
  AT_ADD_ASSIGN, AT_SUB_ASSIGN, AT_MUL_ASSIGN, AT_DIV_ASSIGN, AT_MOD_ASSIGN, AT_POW_ASSIGN,
  AT_OR, AT_AND, AT_NOMATCH, AT_EQ, AT_LE, AT_GE, AT_NE, AT_INCR, AT_DECR, AT_APPEND, AT_POW
};

static const struct { const char *name; int tok; } keywords[] = {
  {"BEGIN", AT_BEGIN}, {"END", AT_END}, {"function", AT_FUNCTION}, {"func", AT_FUNCTION},
  {"if", AT_IF}, {"else", AT_ELSE}, {"while", AT_WHILE}, {"for", AT_FOR}, {"do", AT_DO},
  {"break", AT_BREAK}, {"continue", AT_CONTINUE}, {"next", AT_NEXT}, {"nextfile", AT_NEXTFILE},
  {"exit", AT_EXIT}, {"return", AT_RETURN}, {"delete", AT_DELETE}, {"in", AT_IN},
  {"getline", AT_GETLINE}, {"print", AT_PRINT}, {"printf", AT_PRINTF}, {NULL, 0}};

static const struct { const char *name; int id; } builtins[] = {
  {"length", AB_LENGTH}, {"substr", AB_SUBSTR}, {"index", AB_INDEX}, {"split", AB_SPLIT},
  {"sub", AB_SUB}, {"gsub", AB_GSUB}, {"match", AB_MATCH}, {"sprintf", AB_SPRINTF},
  {"sin", AB_SIN}, {"cos", AB_COS}, {"atan2", AB_ATAN2}, {"exp", AB_EXP}, {"log", AB_LOG},
  {"sqrt", AB_SQRT}, {"int", AB_INT}, {"rand", AB_RAND}, {"srand", AB_SRAND},
  {"tolower", AB_TOLOWER}, {"toupper", AB_TOUPPER}, {"system", AB_SYSTEM}, {"close", AB_CLOSE},
  {"fflush", AB_FFLUSH}, {"gensub", AB_GENSUB}, {"strftime", AB_STRFTIME}, {"systime", AB_SYSTIME},
  {NULL, 0}};


static void syntax_error (Awk *A, const char *msg) {
  const char *ls = A->p, *le;
  if (A->errors++ > 0) return;
  while (ls > A->src && ls[-1] != '\n') ls--;
  le = A->p;
  while (le < A->end && *le != '\n') le++;
  if (A->srcname) fd_printf(A->err, "awk: %s:%d: %.*s\n", A->srcname, A->line, (int)(le - ls), ls);
  else fd_printf(A->err, "awk: cmd. line:%d: %.*s\n", A->line, (int)(le - ls), ls);
  if (A->srcname) fd_printf(A->err, "awk: %s:%d: %*s^ %s\n", A->srcname, A->line, (int)(A->p - ls), "", msg);
  else fd_printf(A->err, "awk: cmd. line:%d: %*s^ %s\n", A->line, (int)(A->p - ls), "", msg);
}


/* after these, a '/' starts a regex, not a division */
static int regex_allowed (int prev) {
  switch (prev) {
    case AT_NAME: case AT_NUMBER: case AT_STRING: case AT_ERE: case ')': case ']': case '$':
    case AT_INCR: case AT_DECR: case AT_BUILTIN: case AT_FUNC_NAME:
      return 0;
  }
  return 1;
}


static void read_string (Awk *A, Buf *b, char quote) {
  while (A->p < A->end && *A->p != quote) {
    char c = *A->p++;
    if (c == '\n') {
      syntax_error(A, "unterminated string");
      return;
    }
    if (c == '\\' && A->p < A->end) {
      c = *A->p++;
      switch (c) {
        case 'n': buf_putc(b, '\n'); break;
        case 't': buf_putc(b, '\t'); break;
        case 'r': buf_putc(b, '\r'); break;
        case '\\': buf_putc(b, '\\'); break;
        case '"': buf_putc(b, '"'); break;
        case '/': buf_putc(b, '/'); break;
        case 'a': buf_putc(b, '\a'); break;
        case 'b': buf_putc(b, '\b'); break;
        case 'f': buf_putc(b, '\f'); break;
        case 'v': buf_putc(b, '\v'); break;
        case '\n': A->line++; break;	/* continued */
        default:
          if (c >= '0' && c <= '7') {
            int v = c - '0', k;
            for (k = 0; k < 2 && A->p < A->end && *A->p >= '0' && *A->p <= '7'; k++) v = v * 8 + (*A->p++ - '0');
            buf_putc(b, (char)v);
          }
          else {	/* unknown: keep the backslash (gawk warns, keeps the char) */
            buf_putc(b, '\\');
            buf_putc(b, c);
          }
      }
      continue;
    }
    buf_putc(b, c);
  }
  if (A->p < A->end) A->p++;
  else syntax_error(A, "unterminated string");
}


static int next_token (Awk *A) {
  int prev = A->tok;
  const char *p;
  A->prev_tok = prev;
again:
  while (A->p < A->end && (*A->p == ' ' || *A->p == '\t' || *A->p == '\r')) A->p++;
  if (A->p < A->end && *A->p == '\\' && A->p + 1 < A->end && (A->p[1] == '\n' || A->p[1] == '\r')) {
    A->p += A->p[1] == '\r' ? 3 : 2;
    A->line++;
    goto again;
  }
  if (A->p < A->end && *A->p == '#') {
    while (A->p < A->end && *A->p != '\n') A->p++;
  }
  if (A->p >= A->end) return A->tok = AT_EOF;
  p = A->p;
  if (*p == '\n') {
    A->p++;
    A->line++;
    return A->tok = AT_NEWLINE;
  }
  if (isdigit((unsigned char)*p) || (*p == '.' && A->p + 1 < A->end && isdigit((unsigned char)p[1]))) {
    char *e;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {	/* gawk: hex constants */
      A->tnum = (double)strtoull(p + 2, &e, 16);
    }
    else A->tnum = strtod(p, &e);
    A->p = e;
    return A->tok = AT_NUMBER;
  }
  if (isalpha((unsigned char)*p) || *p == '_') {
    const char *s = p;
    int k;
    while (A->p < A->end && (isalnum((unsigned char)*A->p) || *A->p == '_')) A->p++;
    free(A->tname);
    A->tname = xstrndup(s, (size_t)(A->p - s));
    for (k = 0; keywords[k].name; k++)
      if (strcmp(keywords[k].name, A->tname) == 0) return A->tok = keywords[k].tok;
    for (k = 0; builtins[k].name; k++)
      if (strcmp(builtins[k].name, A->tname) == 0) {
        A->tnum = builtins[k].id;
        return A->tok = AT_BUILTIN;
      }
    if (A->p < A->end && *A->p == '(') return A->tok = AT_FUNC_NAME;
    return A->tok = AT_NAME;
  }
  if (*p == '"') {
    Buf b;
    A->p++;
    buf_init(&b);
    read_string(A, &b, '"');
    str_unref(A->tstr);
    A->tstr = str_new(b.s ? b.s : "", b.len);
    buf_free(&b);
    return A->tok = AT_STRING;
  }
  if (*p == '/' && regex_allowed(prev)) {	/* a regex literal */
    Buf b;
    int in_br = 0;
    A->p++;
    buf_init(&b);
    while (A->p < A->end && (*A->p != '/' || in_br) && *A->p != '\n') {
      char c = *A->p++;
      if (c == '\\' && A->p < A->end) {
        if (*A->p == '/') buf_putc(&b, '/');
        else {
          buf_putc(&b, '\\');
          buf_putc(&b, *A->p);
        }
        A->p++;
        continue;
      }
      if (c == '[' && !in_br) {
        in_br = 1;
        buf_putc(&b, c);
        if (A->p < A->end && *A->p == '^') buf_putc(&b, *A->p++);
        if (A->p < A->end && *A->p == ']') buf_putc(&b, *A->p++);
        continue;
      }
      if (c == '[' && in_br && A->p < A->end && (*A->p == ':' || *A->p == '.' || *A->p == '=')) {
        char e = *A->p;
        buf_putc(&b, c);
        buf_putc(&b, *A->p++);
        while (A->p + 1 < A->end && !(A->p[0] == e && A->p[1] == ']')) buf_putc(&b, *A->p++);
        if (A->p + 1 < A->end) {
          buf_putc(&b, *A->p++);
          buf_putc(&b, *A->p++);
        }
        continue;
      }
      if (c == ']' && in_br) in_br = 0;
      buf_putc(&b, c);
    }
    if (A->p >= A->end || *A->p != '/') {
      syntax_error(A, "unterminated regexp");
      buf_free(&b);
      return A->tok = AT_EOF;
    }
    A->p++;
    str_unref(A->tstr);
    A->tstr = str_new(b.s ? b.s : "", b.len);
    buf_free(&b);
    return A->tok = AT_ERE;
  }
  A->p++;
  switch (*p) {
    case '+':
      if (A->p < A->end && *A->p == '+') { A->p++; return A->tok = AT_INCR; }
      if (A->p < A->end && *A->p == '=') { A->p++; return A->tok = AT_ADD_ASSIGN; }
      return A->tok = '+';
    case '-':
      if (A->p < A->end && *A->p == '-') { A->p++; return A->tok = AT_DECR; }
      if (A->p < A->end && *A->p == '=') { A->p++; return A->tok = AT_SUB_ASSIGN; }
      return A->tok = '-';
    case '*':
      if (A->p < A->end && *A->p == '*') {
        A->p++;
        if (A->p < A->end && *A->p == '=') { A->p++; return A->tok = AT_POW_ASSIGN; }
        return A->tok = AT_POW;
      }
      if (A->p < A->end && *A->p == '=') { A->p++; return A->tok = AT_MUL_ASSIGN; }
      return A->tok = '*';
    case '/':
      if (A->p < A->end && *A->p == '=') { A->p++; return A->tok = AT_DIV_ASSIGN; }
      return A->tok = '/';
    case '%':
      if (A->p < A->end && *A->p == '=') { A->p++; return A->tok = AT_MOD_ASSIGN; }
      return A->tok = '%';
    case '^':
      if (A->p < A->end && *A->p == '=') { A->p++; return A->tok = AT_POW_ASSIGN; }
      return A->tok = '^';
    case '=':
      if (A->p < A->end && *A->p == '=') { A->p++; return A->tok = AT_EQ; }
      return A->tok = '=';
    case '!':
      if (A->p < A->end && *A->p == '=') { A->p++; return A->tok = AT_NE; }
      if (A->p < A->end && *A->p == '~') { A->p++; return A->tok = AT_NOMATCH; }
      return A->tok = '!';
    case '<':
      if (A->p < A->end && *A->p == '=') { A->p++; return A->tok = AT_LE; }
      return A->tok = '<';
    case '>':
      if (A->p < A->end && *A->p == '=') { A->p++; return A->tok = AT_GE; }
      if (A->p < A->end && *A->p == '>') { A->p++; return A->tok = AT_APPEND; }
      return A->tok = '>';
    case '|':
      if (A->p < A->end && *A->p == '|') { A->p++; return A->tok = AT_OR; }
      return A->tok = '|';
    case '&':
      if (A->p < A->end && *A->p == '&') { A->p++; return A->tok = AT_AND; }
      break;
    case '{': case '}': case '(': case ')': case '[': case ']': case ';': case ',': case '~':
    case '?': case ':': case '$':
      return A->tok = (unsigned char)*p;
  }
  A->p--;
  syntax_error(A, "invalid char in expression");
  A->p++;
  return A->tok = AT_EOF;
}

/* }================================================================== */


/*
** {==================================================================
** Parser
** ===================================================================
*/

static ANode *anode (Awk *A, int op) {
  ANode *n = (ANode *)xmalloc(sizeof(ANode));
  memset(n, 0, sizeof(*n));
  n->op = op;
  n->line = A->line;
  return n;
}


static int global_index (Awk *A, const char *name) {
  int i;
  for (i = 0; i < A->nglobals; i++)
    if (strcmp(A->gnames[i], name) == 0) return i;
  if (A->nglobals == A->gcap) {
    A->gcap = A->gcap ? A->gcap * 2 : 64;
    A->gnames = (char **)xrealloc(A->gnames, (size_t)A->gcap * sizeof(char *));
    A->globals = (AVar *)xrealloc(A->globals, (size_t)A->gcap * sizeof(AVar));
  }
  A->gnames[A->nglobals] = xstrdup(name);
  memset(&A->globals[A->nglobals], 0, sizeof(AVar));
  A->globals[A->nglobals].v = val_uninit();
  return A->nglobals++;
}


static int func_index (Awk *A, const char *name) {
  int i;
  for (i = 0; i < A->nfuncs; i++)
    if (strcmp(A->funcs[i].name, name) == 0) return i;
  A->funcs = (AFunc *)xrealloc(A->funcs, (size_t)(A->nfuncs + 1) * sizeof(AFunc));
  memset(&A->funcs[A->nfuncs], 0, sizeof(AFunc));
  A->funcs[A->nfuncs].name = xstrdup(name);
  A->funcs[A->nfuncs].line = A->line;
  return A->nfuncs++;
}


/* a name: a local of the function being parsed, or a global */
static ANode *name_node (Awk *A, const char *name) {
  ANode *n;
  if (A->cur_func) {
    int i;
    for (i = 0; i < A->cur_func->nparams; i++)
      if (strcmp(A->cur_func->params[i], name) == 0) {
        n = anode(A, AN_LOCAL);
        n->idx = i;
        return n;
      }
  }
  n = anode(A, AN_VAR);
  n->idx = global_index(A, name);
  return n;
}


static void opt_newlines (Awk *A) {
  while (A->tok == AT_NEWLINE) next_token(A);
}


static int expect (Awk *A, int tok, const char *what) {
  if (A->tok != tok) {
    char msg[64];
    snprintf(msg, sizeof(msg), A->tok == AT_NEWLINE || A->tok == AT_EOF ?
             "unexpected newline or end of string" : "syntax error (expected %s)", what);
    syntax_error(A, msg);
    return -1;
  }
  next_token(A);
  return 0;
}


static ANode *expr (Awk *A);
static ANode *ternary (Awk *A);
static ANode *unary (Awk *A);
static ANode *statement (Awk *A);
static ANode *simple_statement (Awk *A);


static int is_lvalue (const ANode *n) {
  return n && (n->op == AN_VAR || n->op == AN_LOCAL || n->op == AN_FIELD || n->op == AN_INDEX);
}


/* expression list, for print and function arguments */
static ANode *expr_list (Awk *A, int *count) {
  ANode *head = NULL, **tail = &head;
  *count = 0;
  for (;;) {
    ANode *e = expr(A);
    if (e == NULL) return head;
    *tail = e;
    tail = &e->next;
    (*count)++;
    if (A->tok != ',') break;
    next_token(A);
    opt_newlines(A);
  }
  return head;
}


static ANode *primary (Awk *A) {
  ANode *n;
  switch (A->tok) {
    case AT_NUMBER:
      n = anode(A, AN_NUM);
      n->num = A->tnum;
      next_token(A);
      return n;
    case AT_STRING:
      n = anode(A, AN_STR);
      n->str = str_ref(A->tstr);
      next_token(A);
      return n;
    case AT_ERE: {
      char *err = NULL;
      n = anode(A, AN_REGEX);
      n->str = str_ref(A->tstr);
      n->re = regex_new_n(A->tstr->s, A->tstr->len, RE_EXTENDED | RE_AWK, &err);
      if (n->re == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "invalid regexp: %s: /%s/", err ? err : "?", A->tstr->s);
        syntax_error(A, msg);
        free(err);
      }
      next_token(A);
      return n;
    }
    case '(': {
      int count;
      ANode *list;
      int saved = A->print_ctx;
      next_token(A);
      A->print_ctx = 0;
      A->in_paren++;
      list = expr_list(A, &count);
      A->in_paren--;
      A->print_ctx = saved;
      if (expect(A, ')', "')'") != 0) return list;
      if (count > 1) {	/* (a, b) in arr, or print (a, b) */
        n = anode(A, AN_GROUP);
        n->a = list;
        n->nargs = count;
        if (A->tok == AT_IN) return n;
        return n;
      }
      n = anode(A, AN_GROUP);
      n->a = list;
      n->nargs = 1;
      return n;
    }
    case '$': {
      next_token(A);
      n = anode(A, AN_FIELD);
      if (A->tok == AT_INCR || A->tok == AT_DECR) {	/* $++i */
        int op = A->tok == AT_INCR ? AN_PREINC : AN_PREDEC;
        ANode *t;
        next_token(A);
        t = anode(A, op);
        t->a = primary(A);
        n->a = t;
      }
      else if (A->tok == '-' || A->tok == '+' || A->tok == '!') {
        n->a = unary(A);
      }
      else n->a = primary(A);
      return n;
    }
    case AT_INCR: case AT_DECR: {
      int op = A->tok == AT_INCR ? AN_PREINC : AN_PREDEC;
      next_token(A);
      n = anode(A, op);
      n->a = primary(A);
      if (!is_lvalue(n->a)) syntax_error(A, "++ or -- needs a variable");
      return n;
    }
    case AT_NAME: {
      char *name = xstrdup(A->tname);
      next_token(A);
      if (A->tok == '[') {	/* arr[subs] */
        int count, saved = A->print_ctx;
        next_token(A);
        n = anode(A, AN_INDEX);
        n->a = name_node(A, name);
        A->print_ctx = 0;
        n->b = expr_list(A, &count);
        A->print_ctx = saved;
        n->nargs = count;
        expect(A, ']', "']'");
      }
      else n = name_node(A, name);
      free(name);
      return n;
    }
    case AT_FUNC_NAME: {
      int count, saved = A->print_ctx;
      n = anode(A, AN_CALL);
      n->idx = func_index(A, A->tname);
      next_token(A);	/* ( */
      next_token(A);
      opt_newlines(A);
      A->print_ctx = 0;
      if (A->tok != ')') n->a = expr_list(A, &count);
      else count = 0;
      A->print_ctx = saved;
      n->nargs = count;
      expect(A, ')', "')'");
      return n;
    }
    case AT_BUILTIN: {
      int count = 0, id = (int)A->tnum;
      n = anode(A, AN_BUILTIN);
      n->idx = id;
      next_token(A);
      if (A->tok == '(') {
        int saved = A->print_ctx;
        next_token(A);
        opt_newlines(A);
        A->print_ctx = 0;
        if (A->tok != ')') n->a = expr_list(A, &count);
        A->print_ctx = saved;
        expect(A, ')', "')'");
      }
      else if (id != AB_LENGTH) syntax_error(A, "builtin function needs its arguments");
      n->nargs = count;
      return n;
    }
    case AT_GETLINE: {	/* getline [var] [< file] */
      next_token(A);
      n = anode(A, AN_GETLINE);
      n->kind = AG_SIMPLE;
      if (A->tok == '$' || A->tok == AT_NAME) n->a = primary(A);
      if (A->tok == '<') {
        next_token(A);
        n->kind = AG_FILE;
        n->b = primary(A);
      }
      return n;
    }
    case '-': case '+': case '!':
      return unary(A);
  }
  syntax_error(A, A->tok == AT_NEWLINE || A->tok == AT_EOF ? "unexpected newline or end of string" : "syntax error");
  return NULL;
}


static ANode *postfix (Awk *A) {
  ANode *n = primary(A);
  if (n && is_lvalue(n) && (A->tok == AT_INCR || A->tok == AT_DECR)) {
    ANode *p = anode(A, A->tok == AT_INCR ? AN_POSTINC : AN_POSTDEC);
    p->a = n;
    next_token(A);
    return p;
  }
  return n;
}


static ANode *power (Awk *A) {
  ANode *n = postfix(A);
  if (n && (A->tok == '^' || A->tok == AT_POW)) {	/* right associative */
    ANode *p = anode(A, AN_POW);
    next_token(A);
    p->a = n;
    if (A->tok == '-' || A->tok == '+' || A->tok == '!') p->b = unary(A);
    else p->b = power(A);
    return p;
  }
  return n;
}


static ANode *unary (Awk *A) {
  if (A->tok == '-' || A->tok == '+' || A->tok == '!') {
    int op = A->tok == '-' ? AN_NEG : A->tok == '+' ? AN_PLUS : AN_NOT;
    ANode *n = anode(A, op);
    next_token(A);
    n->a = unary(A);
    return n;
  }
  return power(A);
}


static ANode *multiplicative (Awk *A) {
  ANode *n = unary(A);
  while (n && (A->tok == '*' || A->tok == '/' || A->tok == '%')) {
    ANode *p = anode(A, A->tok == '*' ? AN_MUL : A->tok == '/' ? AN_DIV : AN_MOD);
    next_token(A);
    p->a = n;
    p->b = unary(A);
    n = p;
  }
  return n;
}


static ANode *additive (Awk *A) {
  ANode *n = multiplicative(A);
  while (n && (A->tok == '+' || A->tok == '-')) {
    ANode *p = anode(A, A->tok == '+' ? AN_ADD : AN_SUB);
    next_token(A);
    p->a = n;
    p->b = multiplicative(A);
    n = p;
  }
  return n;
}


/* can this token start an operand of a concatenation? */
static int starts_operand (Awk *A) {
  switch (A->tok) {
    case AT_NUMBER: case AT_STRING: case AT_ERE: case '(': case '$': case AT_NAME: case AT_FUNC_NAME:
    case AT_BUILTIN: case '!': case AT_INCR: case AT_DECR: case '-': case '+':
      return 1;
    case AT_IN:
      return 0;
  }
  return 0;
}


static ANode *concatenation (Awk *A) {
  ANode *n = additive(A);
  while (n && starts_operand(A) && A->tok != '-' && A->tok != '+' && A->tok != '!' ) {
    ANode *p = anode(A, AN_CONCAT);
    p->a = n;
    p->b = additive(A);
    if (p->b == NULL) return p;
    n = p;
  }
  return n;
}


static ANode *comparison (Awk *A) {
  ANode *n = concatenation(A);
  for (;;) {
    int op;
    if (n == NULL) return NULL;
    if (A->tok == '|' ) {	/* "cmd" | getline [var] */
      const char *save_p = A->p;
      int save_line = A->line, save_tok = A->tok;
      next_token(A);
      if (A->tok == AT_GETLINE) {
        ANode *g = anode(A, AN_GETLINE);
        next_token(A);
        g->kind = AG_CMD;
        g->b = n;
        if (A->tok == '$' || A->tok == AT_NAME) g->a = primary(A);
        n = g;
        continue;
      }
      /* print ... | "cmd": not ours; put the token back */
      A->p = save_p;
      A->line = save_line;
      A->tok = save_tok;
      return n;
    }
    switch (A->tok) {
      case '<': op = AN_LT; break;
      case AT_LE: op = AN_LE; break;
      case AT_NE: op = AN_NE; break;
      case AT_EQ: op = AN_EQ; break;
      case '>': if (A->print_ctx && !A->in_paren) return n; op = AN_GT; break;
      case AT_GE: op = AN_GE; break;
      default: return n;
    }
    {
      ANode *p = anode(A, op);
      next_token(A);
      p->a = n;
      p->b = concatenation(A);
      n = p;
    }
  }
}


static ANode *matching (Awk *A) {
  ANode *n = comparison(A);
  while (n && (A->tok == '~' || A->tok == AT_NOMATCH)) {
    ANode *p = anode(A, A->tok == '~' ? AN_MATCH : AN_NOMATCH);
    next_token(A);
    p->a = n;
    p->b = comparison(A);
    n = p;
  }
  return n;
}


static ANode *in_expr (Awk *A) {
  ANode *n = matching(A);
  while (n && A->tok == AT_IN) {
    ANode *p = anode(A, AN_IN);
    next_token(A);
    if (A->tok != AT_NAME) {
      syntax_error(A, "expected an array name after 'in'");
      return n;
    }
    p->a = n;	/* the subscript (a group for (i,j)) */
    p->b = name_node(A, A->tname);
    next_token(A);
    n = p;
  }
  return n;
}


static ANode *and_expr (Awk *A) {
  ANode *n = in_expr(A);
  while (n && A->tok == AT_AND) {
    ANode *p = anode(A, AN_AND);
    next_token(A);
    opt_newlines(A);
    p->a = n;
    p->b = in_expr(A);
    n = p;
  }
  return n;
}


static ANode *or_expr (Awk *A) {
  ANode *n = and_expr(A);
  while (n && A->tok == AT_OR) {
    ANode *p = anode(A, AN_OR);
    next_token(A);
    opt_newlines(A);
    p->a = n;
    p->b = and_expr(A);
    n = p;
  }
  return n;
}


static ANode *ternary (Awk *A) {
  ANode *n = or_expr(A);
  if (n && A->tok == '?') {
    ANode *p = anode(A, AN_COND);
    next_token(A);
    opt_newlines(A);
    p->a = n;
    p->b = ternary(A);
    opt_newlines(A);
    if (expect(A, ':', "':'") != 0) return p;
    opt_newlines(A);
    p->c = ternary(A);
    return p;
  }
  return n;
}


static ANode *expr (Awk *A) {
  ANode *n = ternary(A);
  int op;
  if (n == NULL) return NULL;
  switch (A->tok) {
    case '=': op = 0; break;
    case AT_ADD_ASSIGN: op = AN_ADD; break;
    case AT_SUB_ASSIGN: op = AN_SUB; break;
    case AT_MUL_ASSIGN: op = AN_MUL; break;
    case AT_DIV_ASSIGN: op = AN_DIV; break;
    case AT_MOD_ASSIGN: op = AN_MOD; break;
    case AT_POW_ASSIGN: op = AN_POW; break;
    default: return n;
  }
  if (!is_lvalue(n)) {
    if (A->tok == AT_DIV_ASSIGN && n->op != AN_REGEX) {	/* a /= b that was not */
      syntax_error(A, "assignment to a non-variable");
      return n;
    }
    syntax_error(A, "assignment to a non-variable");
    return n;
  }
  {
    ANode *p = anode(A, AN_ASSIGN);
    p->idx = op;
    next_token(A);
    opt_newlines(A);
    p->a = n;
    p->b = expr(A);
    return p;
  }
}


static void end_simple (Awk *A) {	/* ; or newline or } ends a simple statement */
  if (A->tok == ';' || A->tok == AT_NEWLINE) {
    next_token(A);
    return;
  }
  if (A->tok == '}' || A->tok == AT_EOF) return;
  syntax_error(A, "syntax error");
}


static ANode *block (Awk *A) {
  ANode *b = anode(A, AS_BLOCK), **tail = &b->a;
  expect(A, '{', "'{'");
  for (;;) {
    ANode *s;
    while (A->tok == AT_NEWLINE || A->tok == ';') next_token(A);
    if (A->tok == '}' || A->tok == AT_EOF || A->errors) break;
    s = statement(A);
    if (s == NULL) break;
    *tail = s;
    while (*tail) tail = &(*tail)->next;
  }
  expect(A, '}', "'}'");
  return b;
}


static ANode *simple_statement (Awk *A) {
  ANode *n;
  switch (A->tok) {
    case AT_PRINT: case AT_PRINTF: {
      int count = 0, is_printf = A->tok == AT_PRINTF;
      n = anode(A, is_printf ? AS_PRINTF : AS_PRINT);
      next_token(A);
      A->print_ctx = 1;
      if (A->tok != ';' && A->tok != AT_NEWLINE && A->tok != '}' && A->tok != '>' &&
          A->tok != AT_APPEND && A->tok != '|' && A->tok != AT_EOF) {
        n->a = expr_list(A, &count);
        /* print (a, b): the group is the list */
        if (count == 1 && n->a->op == AN_GROUP && n->a->nargs > 1 && n->a->next == NULL) {
          count = n->a->nargs;
          n->a = n->a->a;
        }
      }
      A->print_ctx = 0;
      n->nargs = count;
      if (A->tok == '>' || A->tok == AT_APPEND || A->tok == '|') {
        n->kind = A->tok == '>' ? AR_FILE : A->tok == AT_APPEND ? AR_APPEND : AR_PIPE;
        next_token(A);
        n->b = concatenation(A);
      }
      if (is_printf && count == 0) syntax_error(A, "printf: no format");
      return n;
    }
    case AT_DELETE:
      n = anode(A, AS_DELETE);
      next_token(A);
      if (A->tok != AT_NAME) {
        syntax_error(A, "delete needs an array");
        return n;
      }
      n->a = name_node(A, A->tname);
      next_token(A);
      if (A->tok == '[') {
        int count;
        next_token(A);
        n->b = expr_list(A, &count);
        n->nargs = count;
        expect(A, ']', "']'");
      }
      return n;
    case AT_NEXT:
      if (A->cur_func == NULL && 0) {}
      next_token(A);
      return anode(A, AS_NEXT);
    case AT_NEXTFILE:
      next_token(A);
      return anode(A, AS_NEXTFILE);
    case AT_BREAK:
      next_token(A);
      return anode(A, AS_BREAK);
    case AT_CONTINUE:
      next_token(A);
      return anode(A, AS_CONTINUE);
    case AT_EXIT:
      n = anode(A, AS_EXIT);
      next_token(A);
      if (A->tok != ';' && A->tok != AT_NEWLINE && A->tok != '}' && A->tok != AT_EOF) n->a = expr(A);
      return n;
    case AT_RETURN:
      n = anode(A, AS_RETURN);
      if (A->cur_func == NULL) syntax_error(A, "return used outside function context");
      next_token(A);
      if (A->tok != ';' && A->tok != AT_NEWLINE && A->tok != '}' && A->tok != AT_EOF) n->a = expr(A);
      return n;
  }
  n = anode(A, AS_EXPR);
  n->a = expr(A);
  return n;
}


static ANode *statement (Awk *A) {
  ANode *n;
  opt_newlines(A);
  switch (A->tok) {
    case '{':
      return block(A);
    case ';':
      next_token(A);
      return anode(A, AS_BLOCK);
    case AT_IF:
      n = anode(A, AS_IF);
      next_token(A);
      expect(A, '(', "'('");
      n->a = expr(A);
      expect(A, ')', "')'");
      opt_newlines(A);
      n->b = statement(A);
      {	/* else may come after newlines and a ; */
        const char *sp = A->p;
        int sl = A->line, st = A->tok;
        while (A->tok == AT_NEWLINE || A->tok == ';') next_token(A);
        if (A->tok == AT_ELSE) {
          next_token(A);
          opt_newlines(A);
          n->c = statement(A);
        }
        else {
          A->p = sp;
          A->line = sl;
          A->tok = st;
        }
      }
      return n;
    case AT_WHILE:
      n = anode(A, AS_WHILE);
      next_token(A);
      expect(A, '(', "'('");
      n->a = expr(A);
      expect(A, ')', "')'");
      if (A->tok == ';') {	/* while (x); */
        next_token(A);
        n->b = anode(A, AS_BLOCK);
        return n;
      }
      opt_newlines(A);
      n->b = statement(A);
      return n;
    case AT_DO:
      n = anode(A, AS_DO);
      next_token(A);
      opt_newlines(A);
      n->b = statement(A);
      while (A->tok == AT_NEWLINE || A->tok == ';') next_token(A);
      expect(A, AT_WHILE, "'while'");
      expect(A, '(', "'('");
      n->a = expr(A);
      expect(A, ')', "')'");
      end_simple(A);
      return n;
    case AT_FOR: {
      next_token(A);
      expect(A, '(', "'('");
      /* for (k in arr) */
      if (A->tok == AT_NAME) {
        const char *sp = A->p;
        int sl = A->line;
        char *nm = xstrdup(A->tname);
        next_token(A);
        if (A->tok == AT_IN) {
          next_token(A);
          if (A->tok == AT_NAME) {
            char *arr = xstrdup(A->tname);
            next_token(A);
            if (A->tok == ')') {
              n = anode(A, AS_FORIN);
              n->a = name_node(A, nm);
              n->b = name_node(A, arr);
              next_token(A);
              opt_newlines(A);
              n->c = statement(A);
              free(nm);
              free(arr);
              return n;
            }
            free(arr);
          }
        }
        /* not that: back to the start */
        A->p = sp;
        A->line = sl;
        free(A->tname);
        A->tname = nm;
        A->tok = AT_NAME;
        if (A->p < A->end && *A->p == '(') A->tok = AT_FUNC_NAME;
        else nm = NULL;
        if (nm) {
          /* it was a name directly followed by '(': a call */
        }
      }
      n = anode(A, AS_FOR);
      if (A->tok != ';') n->a = simple_statement(A);
      expect(A, ';', "';'");
      opt_newlines(A);
      if (A->tok != ';') n->b = expr(A);
      expect(A, ';', "';'");
      opt_newlines(A);
      if (A->tok != ')') n->c = simple_statement(A);
      expect(A, ')', "')'");
      if (A->tok == ';') {
        next_token(A);
        n->d = anode(A, AS_BLOCK);
        return n;
      }
      opt_newlines(A);
      n->d = statement(A);
      return n;
    }
  }
  n = simple_statement(A);
  end_simple(A);
  return n;
}


static void parse_program (Awk *A) {
  next_token(A);
  for (;;) {
    ARule r;
    while (A->tok == AT_NEWLINE || A->tok == ';') next_token(A);
    if (A->tok == AT_EOF || A->errors) break;
    memset(&r, 0, sizeof(r));
    if (A->tok == AT_FUNCTION) {
      AFunc *f;
      int fi;
      next_token(A);
      if (A->tok != AT_NAME && A->tok != AT_FUNC_NAME) {
        syntax_error(A, "function name expected");
        break;
      }
      fi = func_index(A, A->tname);
      f = &A->funcs[fi];
      if (f->defined) {
        syntax_error(A, "function redefined");
        break;
      }
      f->defined = 1;
      next_token(A);
      expect(A, '(', "'('");
      while (A->tok == AT_NAME) {
        f->params = (char **)xrealloc(f->params, (size_t)(f->nparams + 1) * sizeof(char *));
        f->params[f->nparams++] = xstrdup(A->tname);
        next_token(A);
        if (A->tok == ',') {
          next_token(A);
          opt_newlines(A);
        }
      }
      expect(A, ')', "')'");
      opt_newlines(A);
      A->cur_func = f;
      f->body = block(A);
      A->cur_func = NULL;
      continue;
    }
    if (A->tok == AT_BEGIN || A->tok == AT_END) {
      r.kind = A->tok == AT_BEGIN ? 1 : 2;
      next_token(A);
      opt_newlines(A);
      if (A->tok != '{') {
        syntax_error(A, "BEGIN and END need an action");
        break;
      }
      r.action = block(A);
    }
    else {
      if (A->tok != '{') {
        r.pat = expr(A);
        if (A->tok == ',') {
          next_token(A);
          opt_newlines(A);
          r.pat2 = expr(A);
        }
      }
      if (A->tok == '{') r.action = block(A);
    }
    if (A->errors) break;
    A->rules = (ARule *)xrealloc(A->rules, (size_t)(A->nrules + 1) * sizeof(ARule));
    A->rules[A->nrules++] = r;
    if (A->tok != AT_NEWLINE && A->tok != ';' && A->tok != AT_EOF && A->tok != '{') {
      if (!(A->tok == AT_NAME || A->tok == AT_BEGIN || A->tok == AT_END || A->tok == AT_FUNCTION)) {
        syntax_error(A, "syntax error");
        break;
      }
    }
  }
  {
    int i;
    for (i = 0; i < A->nfuncs && !A->errors; i++)
      if (!A->funcs[i].defined) {
        fd_printf(A->err, "awk: cmd. line:%d: fatal: function `%s' not defined\n", A->funcs[i].line, A->funcs[i].name);
        A->errors++;
      }
  }
}


static void node_free (ANode *n) {
  while (n) {
    ANode *nx = n->next;
    node_free(n->a);
    node_free(n->b);
    node_free(n->c);
    node_free(n->d);
    str_unref(n->str);
    if (n->re) regex_free(n->re);
    free(n);
    n = nx;
  }
}

/* }================================================================== */


/*
** {==================================================================
** Conversions
** ===================================================================
*/

static const char *gstr (Awk *A, int g) {	/* a special variable's text */
  AVar *v = &A->globals[g];
  if (v->v.t & (AT_STR | AT_STRNUM)) return v->v.s->s;
  return NULL;
}


static Str *num_to_str (Awk *A, double d, int output) {
  char buf[512];
  if (d == 0) return str_c("0");
  if (isnan(d)) return str_c(signbit(d) ? "-nan" : "+nan");
  if (isinf(d)) return str_c(d < 0 ? "-inf" : "+inf");
  if (d == floor(d)) {	/* whole numbers in full, like gawk */
    if (fabs(d) < 1e16) snprintf(buf, sizeof(buf), "%lld", (long long)d);
    else snprintf(buf, sizeof(buf), "%.0f", d);
    return str_c(buf);
  }
  {
    const char *fmt = gstr(A, output ? AG_OFMT : AG_CONVFMT);
    if (fmt == NULL) fmt = "%.6g";
    snprintf(buf, sizeof(buf), fmt, d);
  }
  return str_c(buf);
}


/* the text of a value (a new reference) */
static Str *to_str (Awk *A, const Val *v) {
  if (v->t & (AT_STR | AT_STRNUM)) return str_ref(v->s);
  if (v->t & AT_NUM) return num_to_str(A, v->n, 0);
  return empty_str();
}


static Str *to_str_out (Awk *A, const Val *v) {	/* for print: OFMT */
  if (v->t & (AT_STR | AT_STRNUM)) return str_ref(v->s);
  if (v->t & AT_NUM) return num_to_str(A, v->n, 1);
  return empty_str();
}


static double to_num (const Val *v) {
  if (v->t & (AT_NUM | AT_STRNUM)) return v->n;
  if (v->t & AT_STR) return str_to_num(v->s->s);
  return 0;
}


static int to_bool (const Val *v) {
  if (v->t & AT_NUM) return v->n != 0;
  if (v->t & AT_STRNUM) return v->n != 0;
  if (v->t & AT_STR) return v->s->len > 0;
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** Records and fields
** ===================================================================
*/

static void set_nf_var (Awk *A) {
  AVar *v = &A->globals[AG_NF];
  val_free(&v->v);
  v->v = val_num(A->nf);
}


static void fields_grow (Awk *A, int n) {
  if (n > A->fcap) {
    int k;
    int nc = A->fcap ? A->fcap : 16;
    while (nc < n) nc *= 2;
    A->fields = (Val *)xrealloc(A->fields, (size_t)(nc + 1) * sizeof(Val));
    for (k = A->fcap + 1; k <= nc; k++) A->fields[k] = val_uninit();
    A->fcap = nc;
  }
}


static void fields_clear (Awk *A) {
  int k;
  for (k = 1; k <= A->nf; k++) val_free(&A->fields[k]);
  A->nf = 0;
}


static void add_field (Awk *A, const char *s, size_t n) {
  fields_grow(A, A->nf + 1);
  A->nf++;
  A->fields[A->nf] = val_input(str_new(s, n));
}


static Regex *get_regex (Awk *A, Str *key, int flags);

/* splits s by fs (as FS works) into cb */
typedef void (*FieldFn) (void *ctx, const char *s, size_t n);

static void split_with (Awk *A, const char *s, size_t len, const char *fs, size_t fslen,
                        int paragraph, FieldFn fn, void *ctx) {
  const char *p = s, *e = s + len;
  int force_re = paragraph == 2;
  if (force_re) paragraph = 0;
  if (len == 0) return;
  if (fslen == 1 && fs[0] == ' ' && !force_re) {	/* runs of blanks; newlines too */
    for (;;) {
      const char *st;
      while (p < e && (*p == ' ' || *p == '\t' || *p == '\n')) p++;
      if (p >= e) break;
      st = p;
      while (p < e && !(*p == ' ' || *p == '\t' || *p == '\n')) p++;
      fn(ctx, st, (size_t)(p - st));
    }
    return;
  }
  if (fslen == 0 && !force_re) {	/* each character */
    while (p < e) {
      int l = tool_utf8() ? utf8_len(p) : 1;
      if (l < 1 || p + l > e) l = 1;
      fn(ctx, p, (size_t)l);
      p += l;
    }
    return;
  }
  if (fslen == 1 && fs[0] != '\\' && !paragraph && !force_re) {	/* one character, as it is */
    int icase = A->globals[AG_IGNORECASE].v.t != AT_UNINIT && to_num(&A->globals[AG_IGNORECASE].v) != 0;
    for (;;) {
      const char *st = p;
      if (icase && isalpha((unsigned char)fs[0])) {
        while (p < e && tolower((unsigned char)*p) != tolower((unsigned char)fs[0])) p++;
      }
      else while (p < e && *p != fs[0]) p++;
      fn(ctx, st, (size_t)(p - st));
      if (p >= e) break;
      p++;
    }
    return;
  }
  {	/* a regex; with RS="" newlines separate fields too */
    Str *key = paragraph ? NULL : str_new(fs, fslen);
    Regex *re;
    size_t m[2], pos = 0;
    if (paragraph) {
      Buf b;
      buf_init(&b);
      buf_puts(&b, "(");
      buf_putn(&b, fs, fslen);
      buf_puts(&b, ")|\n");
      key = str_new(b.s, b.len);
      buf_free(&b);
    }
    re = get_regex(A, key, 0);
    str_unref(key);
    if (re == NULL) return;
    {
      size_t search = 0;
      for (;;) {	/* separators are the non-empty matches */
        int found = 0;
        while (search <= len && regex_match(re, s, len, search, search > 0 ? RE_NOTBOL : 0, m)) {
          if (m[1] > m[0]) {
            found = 1;
            break;
          }
          search = m[0] + 1;
        }
        if (!found) {
          fn(ctx, s + pos, len - pos);
          break;
        }
        fn(ctx, s + pos, m[0] - pos);
        pos = search = m[1];
      }
    }
  }
}


static void field_cb (void *ctx, const char *s, size_t n) {
  add_field((Awk *)ctx, s, n);
}


static int paragraph_mode (Awk *A) {
  const char *rs = gstr(A, AG_RS);
  return rs != NULL && rs[0] == '\0' && A->globals[AG_RS].v.t != AT_UNINIT;
}


static void split_record (Awk *A) {
  Str *rec, *fs;
  Val fsv;
  if (A->fields_ok) return;
  fields_clear(A);
  rec = to_str(A, &A->rec);
  fsv = A->globals[AG_FS].v;
  fs = A->rec_fs ? str_ref(A->rec_fs) : to_str(A, &fsv);
  split_with(A, rec->s, rec->len, fs->s, fs->len, paragraph_mode(A) && !(fs->len == 1 && fs->s[0] == ' '),
             field_cb, A);
  str_unref(rec);
  str_unref(fs);
  A->fields_ok = 1;
  set_nf_var(A);
}


/* $0 from the fields, joined by OFS */
static void rebuild_record (Awk *A) {
  Buf b;
  int k;
  Str *ofs;
  if (A->rec_ok) return;
  ofs = to_str(A, &A->globals[AG_OFS].v);
  buf_init(&b);
  for (k = 1; k <= A->nf; k++) {
    Str *s = to_str(A, &A->fields[k]);
    if (k > 1) buf_putn(&b, ofs->s, ofs->len);
    buf_putn(&b, s->s, s->len);
    str_unref(s);
  }
  val_free(&A->rec);
  A->rec = val_input(str_new(b.s ? b.s : "", b.len));
  buf_free(&b);
  str_unref(ofs);
  A->rec_ok = 1;
}


static void set_record (Awk *A, Str *s) {	/* takes s */
  val_free(&A->rec);
  A->rec = val_input(s);
  str_unref(A->rec_fs);
  A->rec_fs = to_str(A, &A->globals[AG_FS].v);
  A->fields_ok = 0;
  A->rec_ok = 1;
  fields_clear(A);
  /* NF is computed when asked for */
}


static void set_nf (Awk *A, int n) {
  int k;
  split_record(A);
  if (n < 0) n = 0;
  if (n > A->nf) {
    fields_grow(A, n);
    for (k = A->nf + 1; k <= n; k++) A->fields[k] = val_uninit();
  }
  else for (k = n + 1; k <= A->nf; k++) val_free(&A->fields[k]);
  A->nf = n;
  A->rec_ok = 0;
  rebuild_record(A);
  set_nf_var(A);
}


static Val get_field (Awk *A, int k) {
  if (k < 0) {
    fatal(A, "attempt to access field %d", k);
    return val_uninit();
  }
  if (k == 0) {
    rebuild_record(A);
    return val_copy(&A->rec);
  }
  split_record(A);
  if (k > A->nf) return val_uninit();
  return val_copy(&A->fields[k]);
}


static void set_field (Awk *A, int k, Val v) {	/* takes v */
  if (k < 0) {
    fatal(A, "attempt to access field %d", k);
    val_free(&v);
    return;
  }
  if (k == 0) {
    Str *s = to_str(A, &v);
    val_free(&v);
    set_record(A, s);
    return;
  }
  split_record(A);
  if (k > A->nf) {
    int j;
    fields_grow(A, k);
    for (j = A->nf + 1; j <= k; j++) A->fields[j] = val_uninit();
    A->nf = k;
    set_nf_var(A);
  }
  val_free(&A->fields[k]);
  /* a string assigned to a field is a string, not a strnum */
  A->fields[k] = v;
  A->rec_ok = 0;
}

/* }================================================================== */


/*
** {==================================================================
** Variables
** ===================================================================
*/

static AVar *var_of (Awk *A, ANode *n) {
  AVar *v;
  if (n->op == AN_LOCAL) {
    v = &A->frame->locals[n->idx];
    if (v->kind == AK_UNKNOWN && v->ref && v->ref->kind == AK_ARRAY) return v->ref;
    return v;
  }
  return &A->globals[n->idx];
}


static Array *array_of (Awk *A, ANode *n) {
  AVar *v = var_of(A, n);
  if (v->kind == AK_SCALAR) {
    fatal(A, "attempt to use scalar `%s' as an array", n->op == AN_VAR ? A->gnames[n->idx] : "parameter");
    return NULL;
  }
  if (v->kind == AK_UNKNOWN) {
    if (v->ref && v->ref->kind != AK_SCALAR) {	/* the caller's untyped variable becomes an array */
      AVar *r = v->ref;
      if (r->arr == NULL) {
        r->arr = arr_new();
        r->own_arr = 1;
      }
      r->kind = AK_ARRAY;
      v->kind = AK_ARRAY;
      v->arr = r->arr;
      v->own_arr = 0;
      return v->arr;
    }
    v->kind = AK_ARRAY;
    v->arr = arr_new();
    v->own_arr = 1;
  }
  return v->arr;
}


static Val eval (Awk *A, ANode *n);

/* the key of arr[a, b]: joined by SUBSEP */
static Str *subscript (Awk *A, ANode *list) {
  Val v;
  Str *s;
  if (list && list->next == NULL) {
    v = eval(A, list);
    s = to_str(A, &v);
    val_free(&v);
    return s;
  }
  {
    Buf b;
    Str *sep = to_str(A, &A->globals[AG_SUBSEP].v);
    int first = 1;
    buf_init(&b);
    for (; list; list = list->next) {
      Str *p;
      v = eval(A, list);
      p = to_str(A, &v);
      val_free(&v);
      if (!first) buf_putn(&b, sep->s, sep->len);
      buf_putn(&b, p->s, p->len);
      str_unref(p);
      first = 0;
    }
    str_unref(sep);
    s = str_new(b.s ? b.s : "", b.len);
    buf_free(&b);
    return s;
  }
}


static void special_assigned (Awk *A, int g) {
  if (g == AG_NF) {
    int n = (int)to_num(&A->globals[AG_NF].v);
    set_nf(A, n);
  }
  else if (g == AG_FS) {
    /* the new FS counts from the next record: fields already split stay */
  }
}


/* stores v (taken) into the lvalue n */
static void assign (Awk *A, ANode *n, Val v) {
  switch (n->op) {
    case AN_VAR: case AN_LOCAL: {
      AVar *var = var_of(A, n);
      if (var->kind == AK_ARRAY) {
        fatal(A, "attempt to use array `%s' in a scalar context", n->op == AN_VAR ? A->gnames[n->idx] : "parameter");
        val_free(&v);
        return;
      }
      var->kind = AK_SCALAR;
      val_free(&var->v);
      var->v = v;
      if (n->op == AN_VAR && n->idx < AG_NSPECIAL) special_assigned(A, n->idx);
      return;
    }
    case AN_FIELD: {
      Val k = eval(A, n->a);
      int idx = (int)to_num(&k);
      val_free(&k);
      set_field(A, idx, v);
      return;
    }
    case AN_INDEX: {
      Array *arr = array_of(A, n->a);
      Str *key;
      AElem *e;
      if (arr == NULL) {
        val_free(&v);
        return;
      }
      key = subscript(A, n->b);
      e = arr_get(arr, key);
      str_unref(key);
      val_free(&e->v);
      e->v = v;
      return;
    }
  }
  val_free(&v);
}

/* }================================================================== */


/*
** {==================================================================
** Regexes, dynamic ones cached
** ===================================================================
*/

static Regex *get_regex (Awk *A, Str *key, int flags) {
  int i;
  char *err = NULL;
  Regex *re;
  int icase = A->globals[AG_IGNORECASE].v.t != AT_UNINIT && to_num(&A->globals[AG_IGNORECASE].v) != 0;
  flags |= RE_EXTENDED | RE_AWK | (icase ? RE_ICASE : 0);
  for (i = 0; i < 64; i++)
    if (A->rcache[i].key && A->rcache[i].flags == flags && A->rcache[i].key->len == key->len &&
        memcmp(A->rcache[i].key->s, key->s, key->len) == 0)
      return A->rcache[i].re;
  re = regex_new_n(key->s, key->len, flags, &err);
  if (re == NULL) {
    fatal(A, "invalid regexp: %s: /%s/", err ? err : "?", key->s);
    free(err);
    return NULL;
  }
  i = A->rnext;
  A->rnext = (A->rnext + 1) % 64;
  if (A->rcache[i].key) {
    str_unref(A->rcache[i].key);
    regex_free(A->rcache[i].re);
  }
  A->rcache[i].key = str_ref(key);
  A->rcache[i].re = re;
  A->rcache[i].flags = flags;
  return re;
}


/* the regex of an operand: /re/ as it is, anything else as text */
static Regex *regex_of (Awk *A, ANode *n) {
  Val v;
  Str *s;
  Regex *re;
  if (n->op == AN_REGEX) {
    int icase = A->globals[AG_IGNORECASE].v.t != AT_UNINIT && to_num(&A->globals[AG_IGNORECASE].v) != 0;
    if (!icase) return n->re;
    return get_regex(A, n->str, 0);
  }
  v = eval(A, n);
  s = to_str(A, &v);
  val_free(&v);
  re = get_regex(A, s, 0);
  str_unref(s);
  return re;
}


static int match_rec (Awk *A, Regex *re) {
  Str *s;
  int r;
  if (re == NULL) return 0;
  rebuild_record(A);
  s = to_str(A, &A->rec);
  r = regex_match(re, s->s, s->len, 0, 0, NULL);
  str_unref(s);
  return r;
}

/* }================================================================== */


/*
** {==================================================================
** Input
** ===================================================================
*/

/* one record from r by RS; 0 at the end */
static int read_record (Awk *A, In *r, Str **out) {
  Val rsv = A->globals[AG_RS].v;
  Str *rs = to_str(A, &rsv);
  char *line;
  size_t len;
  int had, ok = 0;
  Str *rt = NULL;
  if (rs->len == 1) {
    if (in_line(r, &line, &len, rs->s[0], &had)) {
      *out = str_new(line, len);
      rt = had ? str_ref(rs) : empty_str();
      ok = 1;
    }
  }
  else if (rs->len == 0 && A->globals[AG_RS].v.t != AT_UNINIT) {	/* paragraphs */
    Buf b;
    int got = 0;
    buf_init(&b);
    while (in_line(r, &line, &len, '\n', &had)) {
      if (len == 0) {
        if (got) break;
        continue;	/* leading blank lines */
      }
      if (got) buf_putc(&b, '\n');
      buf_putn(&b, line, len);
      got = 1;
    }
    if (got) {
      *out = str_new(b.s, b.len);
      rt = str_c("\n\n");
      ok = 1;
    }
    buf_free(&b);
  }
  else {	/* a regex RS (gawk): read on until it matches, or the end */
    Regex *re = get_regex(A, rs, 0);
    Buf b;
    size_t m[2];
    buf_init(&b);
    for (;;) {
      if (b.len > 0 && re && regex_match(re, b.s, b.len, 0, 0, m) && m[1] > m[0] &&
          (m[1] < b.len || r->eof)) {
        *out = str_new(b.s, m[0]);
        rt = str_new(b.s + m[0], m[1] - m[0]);
        /* what came after goes back into the reader */
        {
          size_t rest = b.len - m[1];
          if (rest > 0) {
            if (r->start >= rest) {
              r->start -= rest;
              memcpy(r->buf + r->start, b.s + m[1], rest);
            }
            else {
              size_t have = r->end - r->start;
              char *nb = (char *)xmalloc(rest + have + r->cap);
              memcpy(nb, b.s + m[1], rest);
              memcpy(nb + rest, r->buf + r->start, have);
              free(r->buf);
              r->buf = nb;
              r->cap = rest + have + r->cap;
              r->start = 0;
              r->end = rest + have;
            }
          }
        }
        ok = 1;
        break;
      }
      {
        char chunk[4096];
        long n = in_read(r, chunk, sizeof(chunk));
        if (n <= 0) {
          if (b.len > 0) {
            *out = str_new(b.s, b.len);
            rt = empty_str();
            ok = 1;
          }
          break;
        }
        buf_putn(&b, chunk, (size_t)n);
      }
    }
    buf_free(&b);
  }
  str_unref(rs);
  if (ok) {
    AVar *v = &A->globals[AG_RT];
    val_free(&v->v);
    v->v = val_str(rt);
  }
  else str_unref(rt);
  return ok;
}


static void set_num_var (Awk *A, int g, double d) {
  AVar *v = &A->globals[g];
  val_free(&v->v);
  v->v = val_num(d);
  v->kind = AK_SCALAR;
}


/* var=value from the command line: escapes like a string literal */
static void cmdline_assign (Awk *A, const char *arg) {
  const char *eq = strchr(arg, '=');
  char *name = xstrndup(arg, (size_t)(eq - arg));
  Buf b;
  const char *p = eq + 1;
  int g;
  buf_init(&b);
  while (*p) {
    if (*p == '\\' && p[1]) {
      p++;
      switch (*p) {
        case 'n': buf_putc(&b, '\n'); break;
        case 't': buf_putc(&b, '\t'); break;
        case 'r': buf_putc(&b, '\r'); break;
        case '\\': buf_putc(&b, '\\'); break;
        case '"': buf_putc(&b, '"'); break;
        case '/': buf_putc(&b, '/'); break;
        case 'a': buf_putc(&b, '\a'); break;
        case 'b': buf_putc(&b, '\b'); break;
        case 'f': buf_putc(&b, '\f'); break;
        case 'v': buf_putc(&b, '\v'); break;
        default:
          buf_putc(&b, '\\');
          buf_putc(&b, *p);
      }
      p++;
      continue;
    }
    buf_putc(&b, *p++);
  }
  g = global_index(A, name);
  if (A->globals[g].kind == AK_ARRAY) fatal(A, "cannot use array `%s' as a scalar", name);
  else {
    AVar *v = &A->globals[g];
    val_free(&v->v);
    v->v = val_input(str_new(b.s ? b.s : "", b.len));
    v->kind = AK_SCALAR;
    if (g < AG_NSPECIAL) special_assigned(A, g);
  }
  buf_free(&b);
  free(name);
}


static int is_assignment_arg (const char *a) {
  const char *p = a;
  if (!(isalpha((unsigned char)*p) || *p == '_')) return 0;
  while (isalnum((unsigned char)*p) || *p == '_') p++;
  return *p == '=';
}


/* the next record of the main input (the ARGV files); 0 at the end */
static int next_main_record (Awk *A, Str **out) {
  for (;;) {
    if (!A->cur_open) {
      AVar *argc = &A->globals[AG_ARGC];
      int n = (int)to_num(&argc->v);
      Array *argv = A->globals[AG_ARGV].arr;
      const char *arg = NULL;
      while (A->argi < n) {
        char num[24];
        AElem *e = argv ? arr_find(argv, ll_to_str(A->argi, num), strlen(num)) : NULL;
        Str *s = e ? to_str(A, &e->v) : NULL;
        A->argi++;
        if (s == NULL || s->len == 0) {
          str_unref(s);
          continue;
        }
        if (is_assignment_arg(s->s)) {
          cmdline_assign(A, s->s);
          str_unref(s);
          continue;
        }
        arg = xstrdup(s->s);
        str_unref(s);
        break;
      }
      if (arg == NULL) {
        if (A->used_stdin) return 0;
        arg = xstrdup("-");
      }
      A->used_stdin = 1;
      if (in_open(&A->cur, arg, A->in) != 0) {
        char *native = path_to_native(arg);
        OsStat st;
        out_flush(&A->o);
        if (os_stat(native, &st) == 0 && st.is_dir)
          fd_printf(A->err, "awk: warning: command line argument `%s' is a directory: skipped\n", arg);
        else fd_printf(A->err, "awk: fatal: cannot open file `%s' for reading: %s\n", arg, os_errmsg());
        free(native);
        if (!(os_stat(native = path_to_native(arg), &st) == 0 && st.is_dir)) {
          free(native);
          free((char *)arg);
          A->exit_code = 2;
          A->exiting = 1;
          return 0;
        }
        free(native);
        free((char *)arg);
        continue;
      }
      A->cur_open = 1;
      {
        AVar *fv = &A->globals[AG_FILENAME];
        val_free(&fv->v);
        fv->v = val_str(str_c(strcmp(arg, "-") == 0 ? "" : arg));
      }
      set_num_var(A, AG_FNR, 0);
      free((char *)arg);
    }
    if (read_record(A, &A->cur, out)) {
      set_num_var(A, AG_NR, to_num(&A->globals[AG_NR].v) + 1);
      set_num_var(A, AG_FNR, to_num(&A->globals[AG_FNR].v) + 1);
      return 1;
    }
    in_close(&A->cur);
    A->cur_open = 0;
  }
}

/* }================================================================== */


/*
** {==================================================================
** Output streams, getline streams, commands
** ===================================================================
*/

static int run_command (Awk *A, const char *cmd, const char *input, size_t inlen) {
  int status;
  out_flush(&A->o);
  if (input != NULL) {	/* its input through a temporary file */
    char *dir = path_tmpdir(), *tmp, name[64], *disp, *full;
    char num[24];
    int fd;
    snprintf(name, sizeof(name), "awkpipe%s.tmp", ll_to_str((long long)os_getpid() * 1000 + (long long)(os_now_us() % 1000), num));
    tmp = path_join(dir, name);
    free(dir);
    fd = os_open(tmp, OS_WRITE);
    if (fd < 0) {
      free(tmp);
      return -1;
    }
    os_write(fd, input, inlen);
    os_close(fd);
    disp = path_to_display(tmp);
    {
      char *q = shell_quote(disp);
      full = xstrcat3("{ ", cmd, "\n} < ");
      {
        char *f2 = xstrcat3(full, q, "");
        free(full);
        full = f2;
      }
      free(q);
    }
    status = sh_run_string(full, "awk", 1);
    free(full);
    free(disp);
    os_unlink(tmp);
    free(tmp);
  }
  else status = sh_run_string(cmd, "awk", 1);
  return status;
}


static OStream *get_out (Awk *A, const char *name, int kind) {
  int i;
  OStream *s;
  for (i = 0; i < A->nouts; i++)
    if (strcmp(A->outs[i].name, name) == 0 && (A->outs[i].kind == AR_PIPE) == (kind == AR_PIPE)) return &A->outs[i];
  A->outs = (OStream *)xrealloc(A->outs, (size_t)(A->nouts + 1) * sizeof(OStream));
  s = &A->outs[A->nouts];
  memset(s, 0, sizeof(*s));
  s->name = xstrdup(name);
  s->kind = kind;
  s->fd = -1;
  if (kind == AR_PIPE) buf_init(&s->pipe_data);
  else if (strcmp(name, "/dev/stdout") == 0 || strcmp(name, "-") == 0) s->fd = -2;
  else if (strcmp(name, "/dev/stderr") == 0) s->fd = A->err;
  else {
    char *native = path_to_native(name);
    s->fd = os_open(native, kind == AR_APPEND ? OS_APPEND : OS_WRITE);
    free(native);
    if (s->fd < 0) {
      fatal(A, "can't redirect to `%s' (%s)", name, os_errmsg());
      free(s->name);
      return NULL;
    }
  }
  if (s->fd >= 0) out_init(&s->o, s->fd);
  A->nouts++;
  return s;
}


static int close_out (Awk *A, OStream *s) {
  int r = 0;
  if (s->kind == AR_PIPE) {
    r = run_command(A, s->name, s->pipe_data.s ? s->pipe_data.s : "", s->pipe_data.len);
    buf_free(&s->pipe_data);
  }
  else if (s->fd >= 0) {
    out_flush(&s->o);
    if (s->fd != A->err) os_close(s->fd);
  }
  free(s->name);
  return r;
}


static IStream *get_in (Awk *A, const char *name, int is_cmd) {
  int i;
  IStream *s;
  for (i = 0; i < A->nins; i++)
    if (A->ins[i].is_cmd == is_cmd && strcmp(A->ins[i].name, name) == 0) return &A->ins[i];
  A->ins = (IStream *)xrealloc(A->ins, (size_t)(A->nins + 1) * sizeof(IStream));
  s = &A->ins[A->nins];
  memset(s, 0, sizeof(*s));
  s->name = xstrdup(name);
  s->is_cmd = is_cmd;
  if (is_cmd) {	/* the command runs now; its output is read line by line */
    int p[2];
    out_flush(&A->o);
    s->data = sh_capture(name, &s->len);
    (void)p;
    s->open = 1;
    /* an In over the captured text: a pipe would need a thread; a temp buffer is simpler */
    s->in.fd = -1;
    s->in.own = 0;
    s->in.eof = 1;
    s->in.buf = s->data;
    s->in.cap = s->len + 1;
    s->in.start = 0;
    s->in.end = s->len;
    s->data = NULL;
  }
  else {
    if (in_open(&s->in, name, A->in) != 0) {
      s->open = -1;
    }
    else s->open = 1;
  }
  A->nins++;
  return s;
}


static int close_in (Awk *A, IStream *s) {
  (void)A;
  if (s->open == 1) {
    if (s->is_cmd) {
      free(s->in.buf);
      s->in.buf = NULL;
    }
    else in_close(&s->in);
  }
  free(s->name);
  return 0;
}


static int do_close (Awk *A, const char *name) {
  int i, r = -1;
  for (i = 0; i < A->nouts; i++)
    if (strcmp(A->outs[i].name, name) == 0) {
      r = close_out(A, &A->outs[i]);
      A->outs[i] = A->outs[--A->nouts];
      i--;
    }
  for (i = 0; i < A->nins; i++)
    if (strcmp(A->ins[i].name, name) == 0) {
      r = close_in(A, &A->ins[i]);
      A->ins[i] = A->ins[--A->nins];
      i--;
    }
  return r;
}

/* }================================================================== */


/*
** {==================================================================
** printf
** ===================================================================
*/

static void put_utf8 (Buf *b, unsigned long cp) {
  if (cp < 0x80) buf_putc(b, (char)cp);
  else if (cp < 0x800) {
    buf_putc(b, (char)(0xC0 | (cp >> 6)));
    buf_putc(b, (char)(0x80 | (cp & 0x3F)));
  }
  else if (cp < 0x10000) {
    buf_putc(b, (char)(0xE0 | (cp >> 12)));
    buf_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
    buf_putc(b, (char)(0x80 | (cp & 0x3F)));
  }
  else {
    buf_putc(b, (char)(0xF0 | (cp >> 18)));
    buf_putc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
    buf_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
    buf_putc(b, (char)(0x80 | (cp & 0x3F)));
  }
}


static Str *format (Awk *A, Str *fmt, Val *args, int nargs) {
  Buf b;
  const char *p = fmt->s, *e = fmt->s + fmt->len;
  int ai = 0;
  buf_init(&b);
  while (p < e) {
    char spec[64], conv;
    size_t sl = 0;
    int width_star = 0, prec_star = 0;
    if (*p != '%') {
      buf_putc(&b, *p++);
      continue;
    }
    if (p + 1 < e && p[1] == '%') {
      buf_putc(&b, '%');
      p += 2;
      continue;
    }
    spec[sl++] = *p++;
    while (p < e && strchr("-+ #0'", *p) && sl < 40) {
      if (*p != '\'') spec[sl++] = *p;
      p++;
    }
    if (p < e && *p == '*') {
      width_star = 1;
      p++;
    }
    else while (p < e && isdigit((unsigned char)*p) && sl < 50) spec[sl++] = *p++;
    if (p < e && *p == '.') {
      spec[sl++] = *p++;
      if (p < e && *p == '*') {
        prec_star = 1;
        p++;
      }
      else while (p < e && isdigit((unsigned char)*p) && sl < 58) spec[sl++] = *p++;
    }
    while (p < e && (*p == 'h' || *p == 'l' || *p == 'L' || *p == 'q' || *p == 'j' || *p == 'z')) p++;
    if (p >= e) {	/* a lone % at the end */
      spec[sl] = '\0';
      buf_puts(&b, spec);
      break;
    }
    conv = *p++;
    {
      char final[80], tmp[512];
      int w = 0, pr = -1, have_w = 0, have_p = 0;
      Val *arg;
      if (width_star) {
        w = ai < nargs ? (int)to_num(&args[ai++]) : 0;
        have_w = 1;
      }
      if (prec_star) {
        pr = ai < nargs ? (int)to_num(&args[ai++]) : 0;
        have_p = 1;
      }
      arg = ai < nargs ? &args[ai++] : NULL;
      spec[sl] = '\0';
      if (arg == NULL && strchr("diouxXeEfFgGaAcs", conv)) {	/* gawk: a fatal error */
        size_t at = (size_t)(p - fmt->s) - sl - 1;
        out_flush(&A->o);
        fatal(A, "not enough arguments to satisfy format string\n\t`%s'\n\t%*s^ ran out for this one",
              fmt->s, (int)at + 1, "");
        buf_free(&b);
        return empty_str();
      }
      /* rebuild the spec with the * values in place */
      {
        size_t k = 0, j = 0;
        char *dot = strchr(spec, '.');
        for (k = 0; spec[k] && &spec[k] != dot; k++) final[j++] = spec[k];
        if (have_w) j += (size_t)sprintf(final + j, "%d", w);
        if (dot) {
          final[j++] = '.';
          if (have_p) j += (size_t)sprintf(final + j, "%d", pr);
          else for (k = (size_t)(dot - spec) + 1; spec[k]; k++) final[j++] = spec[k];
        }
        final[j] = '\0';
      }
      switch (conv) {
        case 'd': case 'i': {
          double d = arg ? to_num(arg) : 0;
          size_t fl = strlen(final);
          d = d < 0 ? ceil(d) : floor(d);
          if (isnan(d) || isinf(d) || fabs(d) >= 9e18) {
            final[fl] = 'f';
            final[fl + 1] = '\0';
            snprintf(tmp, sizeof(tmp), final, d);
          }
          else {
            memcpy(final + fl, "lld", 4);
            snprintf(tmp, sizeof(tmp), final, (long long)d);
          }
          buf_puts(&b, tmp);
          break;
        }
        case 'o': case 'x': case 'X': case 'u': {
          double d = arg ? to_num(arg) : 0;
          size_t fl = strlen(final);
          final[fl] = 'l';
          final[fl + 1] = 'l';
          final[fl + 2] = conv;
          final[fl + 3] = '\0';
          snprintf(tmp, sizeof(tmp), final, d < 0 ? (unsigned long long)(long long)d : (unsigned long long)d);
          buf_puts(&b, tmp);
          break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
          size_t fl = strlen(final);
          final[fl] = conv;
          final[fl + 1] = '\0';
          snprintf(tmp, sizeof(tmp), final, arg ? to_num(arg) : 0.0);
          buf_puts(&b, tmp);
          break;
        }
        case 'c': {
          Buf cb;
          buf_init(&cb);
          if (arg && (arg->t & AT_NUM)) put_utf8(&cb, (unsigned long)to_num(arg));
          else if (arg) {
            Str *s = to_str(A, arg);
            if (s->len > 0) buf_putn(&cb, s->s, (size_t)(utf8_len(s->s) > 0 ? utf8_len(s->s) : 1));
            str_unref(s);
          }
          {
            size_t fl = strlen(final);
            final[fl] = 's';
            final[fl + 1] = '\0';
            if (cb.len == 1 && cb.s[0] == '\0') {	/* %c of 0: a NUL byte */
              buf_putc(&b, '\0');
            }
            else {
              char *big = (char *)xmalloc(cb.len + (size_t)(w > 0 ? w : 0) + 16);
              sprintf(big, final, cb.s ? cb.s : "");
              buf_puts(&b, big);
              free(big);
            }
          }
          buf_free(&cb);
          break;
        }
        case 's': {
          Str *s = arg ? to_str(A, arg) : empty_str();
          size_t fl = strlen(final);
          /* width and precision count characters */
          int prec = -1, wid = 0, left = strchr(final, '-') != NULL;
          const char *dot = strchr(final, '.');
          size_t take = s->len, cols;
          if (dot) prec = atoi(dot + 1);
          {
            const char *q = final + 1;
            while (*q && strchr("-+ #0", *q)) q++;
            wid = atoi(q);
          }
          if (prec >= 0) {	/* the first prec characters */
            size_t i = 0;
            int c = 0;
            while (i < s->len && c < prec) {
              int l = tool_utf8() ? utf8_len(s->s + i) : 1;
              if (l < 1) l = 1;
              i += (size_t)l;
              c++;
            }
            take = i;
          }
          cols = tool_utf8() ? utf8_count(s->s, take) : take;
          if (!left) {
            size_t k;
            for (k = cols; (int)k < wid; k++) buf_putc(&b, ' ');
          }
          buf_putn(&b, s->s, take);
          if (left) {
            size_t k;
            for (k = cols; (int)k < wid; k++) buf_putc(&b, ' ');
          }
          (void)fl;
          str_unref(s);
          break;
        }
        default:	/* not a conversion: as it is */
          buf_puts(&b, spec);
          buf_putc(&b, conv);
          if (arg) ai--;
          break;
      }
    }
  }
  {
    Str *r = str_new(b.s ? b.s : "", b.len);
    buf_free(&b);
    return r;
  }
}

/* }================================================================== */


/*
** {==================================================================
** Evaluation
** ===================================================================
*/

enum { AX_NORMAL, AX_BREAK, AX_CONTINUE, AX_NEXT, AX_NEXTFILE, AX_EXIT, AX_RETURN };

static int exec (Awk *A, ANode *n);


static int compare (Awk *A, Val *a, Val *b) {
  int an = (a->t & (AT_NUM | AT_STRNUM)) || a->t == AT_UNINIT;
  int bn = (b->t & (AT_NUM | AT_STRNUM)) || b->t == AT_UNINIT;
  if (an && bn) {
    double x = to_num(a), y = to_num(b);
    return x < y ? -1 : x > y ? 1 : 0;
  }
  {
    Str *x = to_str(A, a), *y = to_str(A, b);
    size_t n = x->len < y->len ? x->len : y->len;
    int r = memcmp(x->s, y->s, n);
    if (r == 0) r = x->len < y->len ? -1 : x->len > y->len ? 1 : 0;
    str_unref(x);
    str_unref(y);
    return r;
  }
}


static size_t char_len (const char *s, size_t n) {	/* characters in s */
  return tool_utf8() ? utf8_count(s, n) : n;
}


static size_t char_off (const char *s, size_t n, size_t chars) {	/* the byte of character chars */
  size_t i = 0, c = 0;
  if (!tool_utf8()) return chars < n ? chars : n;
  while (i < n && c < chars) {
    int l = utf8_len(s + i);
    if (l < 1) l = 1;
    i += (size_t)l;
    c++;
  }
  return i < n ? i : n;
}


/* sub and gsub (and gensub's work): the new text, the count */
static Str *substitute (Awk *A, Regex *re, Str *repl, Str *target, int global, int which,
                        int gensub_style, int *count) {
  Buf b;
  size_t pos = 0, done = 0, *m;
  int n = 0, nsub = regex_nsub(re), matchno = 0;
  size_t prev_end = (size_t)-1;
  (void)A;
  m = (size_t *)xmalloc(2 * ((size_t)nsub + 1) * sizeof(size_t));
  buf_init(&b);
  while (pos <= target->len &&
         regex_match(re, target->s, target->len, pos, pos > 0 ? RE_NOTBOL : 0, m)) {
    size_t ms = m[0], me = m[1];
    int take;
    if (me == ms && ms == prev_end) {	/* an empty match right after the last one */
      if (ms >= target->len) break;
      pos = ms + (size_t)(utf8_len(target->s + ms) > 0 ? utf8_len(target->s + ms) : 1);
      continue;
    }
    matchno++;
    take = global || matchno == which;
    if (take) {
      const char *r = repl->s, *re_ = repl->s + repl->len;
      buf_putn(&b, target->s + done, ms - done);
      while (r < re_) {
        if (*r == '\\' && !gensub_style) {	/* gawk's rules: backslashes count only before & */
          const char *q = r;
          size_t k, nb;
          while (q < re_ && *q == '\\') q++;
          nb = (size_t)(q - r);
          if (q < re_ && *q == '&') {
            for (k = 0; k < nb / 2; k++) buf_putc(&b, '\\');
            if (nb % 2) buf_putc(&b, '&');
            else buf_putn(&b, target->s + ms, me - ms);
            r = q + 1;
          }
          else {
            buf_putn(&b, r, nb);
            r = q;
          }
          continue;
        }
        if (*r == '\\' && r + 1 < re_) {
          if (r[1] == '&') {
            buf_putc(&b, '&');
            r += 2;
            continue;
          }
          if (r[1] == '\\') {
            buf_putc(&b, '\\');
            r += 2;
            continue;
          }
          if (gensub_style && isdigit((unsigned char)r[1])) {
            int g = r[1] - '0';
            if (g == 0) buf_putn(&b, target->s + ms, me - ms);
            else if (g <= nsub && m[2 * g] != (size_t)-1) buf_putn(&b, target->s + m[2 * g], m[2 * g + 1] - m[2 * g]);
            r += 2;
            continue;
          }
          buf_putc(&b, '\\');
          r++;
          continue;
        }
        if (*r == '&') {
          buf_putn(&b, target->s + ms, me - ms);
          r++;
          continue;
        }
        buf_putc(&b, *r++);
      }
      done = me;
      n++;
      if (!global) break;
    }
    prev_end = me;
    if (me == ms) {
      if (ms >= target->len) break;
      pos = ms + (size_t)(utf8_len(target->s + ms) > 0 ? utf8_len(target->s + ms) : 1);
    }
    else pos = me;
  }
  free(m);
  *count = n;
  if (n == 0) {
    buf_free(&b);
    return str_ref(target);
  }
  buf_putn(&b, target->s + done, target->len - done);
  {
    Str *r = str_new(b.s ? b.s : "", b.len);
    buf_free(&b);
    return r;
  }
}


typedef struct SplitCtx {
  Awk *A;
  Array *arr;
  int n;
} SplitCtx;


static void split_cb (void *ctx, const char *s, size_t n) {
  SplitCtx *c = (SplitCtx *)ctx;
  char num[24];
  Str *key = str_c(ll_to_str(++c->n, num));
  AElem *e = arr_get(c->arr, key);
  str_unref(key);
  val_free(&e->v);
  e->v = val_input(str_new(s, n));
}


static Val call_builtin (Awk *A, ANode *n) {
  ANode *a = n->a;
  int na = n->nargs;
  switch (n->idx) {
    case AB_LENGTH: {
      Val v;
      Str *s;
      double len;
      if (na == 0) {
        rebuild_record(A);
        s = to_str(A, &A->rec);
      }
      else {
        if ((a->op == AN_VAR || a->op == AN_LOCAL) && var_of(A, a)->kind == AK_ARRAY)
          return val_num((double)var_of(A, a)->arr->n);
        v = eval(A, a);
        s = to_str(A, &v);
        val_free(&v);
      }
      len = (double)char_len(s->s, s->len);
      str_unref(s);
      return val_num(len);
    }
    case AB_SUBSTR: {
      Val v = eval(A, a), mv = eval(A, a->next), lv;
      Str *s = to_str(A, &v);
      double m = to_num(&mv), l, total = (double)char_len(s->s, s->len), start, end;
      Str *r;
      val_free(&v);
      val_free(&mv);
      /* characters from round(m), l of them; like gawk (and BWK awk), a start
      ** below 1 is 1 and the length stays */
      m = floor(m + 0.5);
      if (m < 1) m = 1;
      if (na >= 3) {
        lv = eval(A, a->next->next);
        l = floor(to_num(&lv) + 0.5);
        val_free(&lv);
        end = m + l;
      }
      else end = total + 1;
      start = m;
      if (end > total + 1) end = total + 1;
      if (end <= start || isnan(start) || isnan(end)) r = empty_str();
      else {
        size_t bs = char_off(s->s, s->len, (size_t)(start - 1));
        size_t be = char_off(s->s, s->len, (size_t)(end - 1));
        r = str_new(s->s + bs, be - bs);
      }
      str_unref(s);
      return val_str(r);
    }
    case AB_INDEX: {
      Val v = eval(A, a), t = eval(A, a->next);
      Str *s = to_str(A, &v), *f = to_str(A, &t);
      double r = 0;
      val_free(&v);
      val_free(&t);
      if (f->len == 0) r = 1;	/* "" is found at once, as in gawk */
      else {
        size_t i;
        for (i = 0; i + f->len <= s->len; i++)
          if (memcmp(s->s + i, f->s, f->len) == 0) {
            r = (double)char_len(s->s, i) + 1;
            break;
          }
      }
      str_unref(s);
      str_unref(f);
      return val_num(r);
    }
    case AB_SPLIT: {
      Val v = eval(A, a);
      Str *s = to_str(A, &v);
      Array *arr = array_of(A, a->next);
      SplitCtx c;
      val_free(&v);
      if (arr == NULL) {
        str_unref(s);
        return val_num(0);
      }
      arr_clear(arr);
      c.A = A;
      c.arr = arr;
      c.n = 0;
      if (na >= 3) {
        ANode *fsn = a->next->next;
        if (fsn->op == AN_REGEX)	/* a regex literal: as a regex, even one character */
          split_with(A, s->s, s->len, fsn->str->s, fsn->str->len, 2, split_cb, &c);
        else {
          Val fv = eval(A, fsn);
          Str *fs = to_str(A, &fv);
          val_free(&fv);
          split_with(A, s->s, s->len, fs->s, fs->len, 0, split_cb, &c);
          str_unref(fs);
        }
      }
      else {
        Str *fs = to_str(A, &A->globals[AG_FS].v);
        split_with(A, s->s, s->len, fs->s, fs->len, paragraph_mode(A), split_cb, &c);
        str_unref(fs);
      }
      str_unref(s);
      return val_num(c.n);
    }
    case AB_SUB: case AB_GSUB: {
      Regex *re = regex_of(A, a);
      Val rv = eval(A, a->next), tv;
      Str *repl = to_str(A, &rv), *target, *res;
      ANode *tn = na >= 3 ? a->next->next : NULL;
      int count = 0;
      val_free(&rv);
      if (re == NULL) {
        str_unref(repl);
        return val_num(0);
      }
      if (tn) {
        tv = eval(A, tn);
        target = to_str(A, &tv);
        val_free(&tv);
      }
      else {
        rebuild_record(A);
        target = to_str(A, &A->rec);
      }
      res = substitute(A, re, repl, target, n->idx == AB_GSUB, 1, 0, &count);
      if (count > 0) {
        if (tn == NULL) set_record(A, res);
        else if (is_lvalue(tn)) assign(A, tn, val_str(res));
        else str_unref(res);
      }
      else str_unref(res);
      str_unref(target);
      str_unref(repl);
      return val_num(count);
    }
    case AB_GENSUB: {
      Regex *re = regex_of(A, a);
      Val rv = eval(A, a->next), hv = eval(A, a->next->next), tv;
      Str *repl = to_str(A, &rv), *target, *res, *how = to_str(A, &hv);
      int count = 0, global = how->len > 0 && (how->s[0] == 'g' || how->s[0] == 'G'), which = 1;
      if (!global) {
        which = (int)to_num(&hv);
        if (which < 1) which = 1;
      }
      val_free(&rv);
      val_free(&hv);
      str_unref(how);
      if (na >= 4) {
        tv = eval(A, a->next->next->next);
        target = to_str(A, &tv);
        val_free(&tv);
      }
      else {
        rebuild_record(A);
        target = to_str(A, &A->rec);
      }
      if (re == NULL) res = str_ref(target);
      else res = substitute(A, re, repl, target, global, which, 1, &count);
      str_unref(target);
      str_unref(repl);
      return val_str(res);
    }
    case AB_MATCH: {
      Val v = eval(A, a);
      Str *s = to_str(A, &v);
      Regex *re = regex_of(A, a->next);
      double rstart = 0, rlen = -1;
      val_free(&v);
      if (re) {
        size_t *m = (size_t *)xmalloc(2 * ((size_t)regex_nsub(re) + 1) * sizeof(size_t));
        if (regex_match(re, s->s, s->len, 0, 0, m)) {
          rstart = (double)char_len(s->s, m[0]) + 1;
          rlen = (double)char_len(s->s + m[0], m[1] - m[0]);
          if (na >= 3) {	/* gawk: the groups into an array */
            Array *arr = array_of(A, a->next->next);
            int g;
            if (arr) {
              arr_clear(arr);
              for (g = 0; g <= regex_nsub(re); g++) {
                if (m[2 * g] != (size_t)-1) {
                  char num[24];
                  Str *key = str_c(ll_to_str(g, num));
                  AElem *e = arr_get(arr, key);
                  str_unref(key);
                  val_free(&e->v);
                  e->v = val_input(str_new(s->s + m[2 * g], m[2 * g + 1] - m[2 * g]));
                }
              }
            }
          }
        }
        else if (na >= 3) {
          Array *arr = array_of(A, a->next->next);
          if (arr) arr_clear(arr);
        }
        free(m);
      }
      str_unref(s);
      set_num_var(A, AG_RSTART, rstart);
      set_num_var(A, AG_RLENGTH, rlen);
      return val_num(rstart);
    }
    case AB_SPRINTF: {
      Val *args = (Val *)xmalloc((size_t)(na > 0 ? na : 1) * sizeof(Val));
      int i;
      ANode *x;
      Str *f, *r;
      for (i = 0, x = a; x; x = x->next, i++) args[i] = eval(A, x);
      f = na > 0 ? to_str(A, &args[0]) : empty_str();
      r = format(A, f, args + 1, na - 1);
      str_unref(f);
      for (i = 0; i < na; i++) val_free(&args[i]);
      free(args);
      return val_str(r);
    }
    case AB_SIN: case AB_COS: case AB_EXP: case AB_LOG: case AB_SQRT: case AB_INT: {
      Val v = na > 0 ? eval(A, a) : val_uninit();
      double x = to_num(&v), r;
      val_free(&v);
      switch (n->idx) {
        case AB_SIN: r = sin(x); break;
        case AB_COS: r = cos(x); break;
        case AB_EXP: r = exp(x); break;
        case AB_LOG: r = log(x); break;
        case AB_SQRT: r = sqrt(x); break;
        default: r = x < 0 ? ceil(x) : floor(x); break;
      }
      return val_num(r);
    }
    case AB_ATAN2: {
      Val y = eval(A, a), x = eval(A, a->next);
      double r = atan2(to_num(&y), to_num(&x));
      val_free(&y);
      val_free(&x);
      return val_num(r);
    }
    case AB_RAND: {
      A->rseed = A->rseed * 1103515245u + 12345u;
      return val_num((double)((A->rseed >> 1) & 0x3FFFFFFF) / 1073741824.0);
    }
    case AB_SRAND: {
      double prev = A->prev_seed, seed;
      if (na > 0) {
        Val v = eval(A, a);
        seed = to_num(&v);
        val_free(&v);
      }
      else seed = (double)time(NULL);
      A->prev_seed = seed;
      A->rseed = (unsigned)(long long)seed;
      return val_num(prev);
    }
    case AB_TOLOWER: case AB_TOUPPER: {
      Val v = eval(A, a);
      Str *s = to_str(A, &v), *r = str_new(s->s, s->len);
      size_t i;
      val_free(&v);
      for (i = 0; i < r->len; i++)
        r->s[i] = (char)(n->idx == AB_TOLOWER ? tolower((unsigned char)r->s[i]) : toupper((unsigned char)r->s[i]));
      str_unref(s);
      return val_str(r);
    }
    case AB_SYSTEM: {
      Val v = eval(A, a);
      Str *s = to_str(A, &v);
      int st;
      int i;
      val_free(&v);
      for (i = 0; i < A->nouts; i++)
        if (A->outs[i].fd >= 0) out_flush(&A->outs[i].o);
      out_flush(&A->o);
      st = run_command(A, s->s, NULL, 0);
      str_unref(s);
      return val_num(st);
    }
    case AB_CLOSE: {
      Val v = eval(A, a);
      Str *s = to_str(A, &v);
      int r;
      val_free(&v);
      r = do_close(A, s->s);
      str_unref(s);
      return val_num(r);
    }
    case AB_FFLUSH: {
      int i;
      out_flush(&A->o);
      for (i = 0; i < A->nouts; i++)
        if (A->outs[i].fd >= 0) out_flush(&A->outs[i].o);
      return val_num(0);
    }
    case AB_SYSTIME:
      return val_num((double)time(NULL));
    case AB_STRFTIME: {
      char buf[1024];
      Str *fmt = NULL;
      time_t t = time(NULL);
      struct tm *tm;
      int utc = 0;
      if (na >= 1) {
        Val v = eval(A, a);
        fmt = to_str(A, &v);
        val_free(&v);
      }
      if (na >= 2) {
        Val v = eval(A, a->next);
        t = (time_t)to_num(&v);
        val_free(&v);
      }
      if (na >= 3) {
        Val v = eval(A, a->next->next);
        utc = to_bool(&v);
        val_free(&v);
      }
      tm = utc ? gmtime(&t) : localtime(&t);
      if (tm == NULL || strftime(buf, sizeof(buf), fmt ? fmt->s : "%a %b %e %H:%M:%S %Z %Y", tm) == 0)
        buf[0] = '\0';
      str_unref(fmt);
      return val_str(str_c(buf));
    }
  }
  return val_uninit();
}


static Val call_func (Awk *A, ANode *n) {
  AFunc *f = &A->funcs[n->idx];
  AFrame fr, *saved;
  ANode *arg;
  int i;
  Val r;
  static int depth = 0;
  if (n->nargs > f->nparams) {
    fatal(A, "function `%s' called with more arguments than declared", f->name);
    return val_uninit();
  }
  if (++depth > 10000) {
    fatal(A, "function call nesting too deep");
    depth--;
    return val_uninit();
  }
  fr.n = f->nparams;
  fr.locals = (AVar *)xmalloc((size_t)(f->nparams + 1) * sizeof(AVar));
  memset(fr.locals, 0, (size_t)(f->nparams + 1) * sizeof(AVar));
  for (i = 0; i < f->nparams; i++) fr.locals[i].v = val_uninit();
  for (i = 0, arg = n->a; arg; arg = arg->next, i++) {
    AVar *lv = &fr.locals[i];
    if (arg->op == AN_VAR || arg->op == AN_LOCAL) {	/* arrays go by reference */
      AVar *src = var_of(A, arg);
      if (src->kind == AK_ARRAY) {
        lv->kind = AK_ARRAY;
        lv->arr = src->arr;
        lv->own_arr = 0;
        continue;
      }
      if (src->kind == AK_UNKNOWN) {	/* may become an array in there */
        lv->ref = src;
        continue;
      }
    }
    lv->v = eval(A, arg);
    lv->kind = AK_SCALAR;
  }
  saved = A->frame;
  A->frame = &fr;
  A->retval = val_uninit();
  exec(A, f->body);
  r = A->retval;
  A->retval = val_uninit();
  A->frame = saved;
  for (i = 0; i < f->nparams; i++) {
    val_free(&fr.locals[i].v);
    if (fr.locals[i].own_arr) arr_free(fr.locals[i].arr);
  }
  free(fr.locals);
  depth--;
  return r;
}


static Val do_getline (Awk *A, ANode *n) {
  Str *rec = NULL;
  int ok;
  if (n->kind == AG_SIMPLE) {
    ok = next_main_record(A, &rec);
    if (ok < 1) return val_num(A->exiting ? -1 : 0);
    if (n->a) assign(A, n->a, val_input(rec));
    else {
      set_record(A, rec);
    }
    return val_num(1);
  }
  {
    Val nv = eval(A, n->b);
    Str *name = to_str(A, &nv);
    IStream *s;
    val_free(&nv);
    s = get_in(A, name->s, n->kind == AG_CMD);
    str_unref(name);
    if (s->open != 1) return val_num(-1);
    ok = read_record(A, &s->in, &rec);
    if (!ok) return val_num(0);
    if (n->a) assign(A, n->a, val_input(rec));
    else set_record(A, rec);
    if (n->kind == AG_CMD || n->a == NULL) {
      set_num_var(A, AG_NR, to_num(&A->globals[AG_NR].v) + 1);
      if (n->a == NULL && n->kind == AG_CMD) {}
    }
    if (n->kind == AG_FILE && n->a == NULL) {
      /* getline < file: NR and FNR stay */
      set_num_var(A, AG_NR, to_num(&A->globals[AG_NR].v) - 1);
    }
    return val_num(1);
  }
}


static double arith (Awk *A, int op, double x, double y) {
  switch (op) {
    case AN_ADD: return x + y;
    case AN_SUB: return x - y;
    case AN_MUL: return x * y;
    case AN_DIV:
      if (y == 0) {
        fatal(A, "division by zero attempted");
        return 0;
      }
      return x / y;
    case AN_MOD:
      if (y == 0) {
        fatal(A, "division by zero attempted in `%%'");
        return 0;
      }
      return fmod(x, y);
    case AN_POW: {
      if (y == floor(y) && fabs(y) < 1024) {	/* exact for integers */
        double r = 1, b = x;
        long long e = (long long)fabs(y);
        while (e) {
          if (e & 1) r *= b;
          b *= b;
          e >>= 1;
        }
        return y < 0 ? 1 / r : r;
      }
      return pow(x, y);
    }
  }
  return 0;
}


static Val eval (Awk *A, ANode *n) {
  if (A->fatal_err) return val_uninit();
  switch (n->op) {
    case AN_NUM: return val_num(n->num);
    case AN_STR: return val_str(str_ref(n->str));
    case AN_REGEX: return val_num(match_rec(A, regex_of(A, n)));
    case AN_GROUP: return eval(A, n->a);
    case AN_VAR: case AN_LOCAL: {
      AVar *v = var_of(A, n);
      if (v->kind == AK_ARRAY) {
        fatal(A, "attempt to use array `%s' in a scalar context", n->op == AN_VAR ? A->gnames[n->idx] : "parameter");
        return val_uninit();
      }
      if (n->op == AN_VAR && n->idx == AG_NF) split_record(A);
      return val_copy(&v->v);
    }
    case AN_FIELD: {
      Val k = eval(A, n->a);
      double d = to_num(&k);
      val_free(&k);
      return get_field(A, (int)d);
    }
    case AN_INDEX: {
      Array *arr = array_of(A, n->a);
      Str *key;
      AElem *e;
      if (arr == NULL) return val_uninit();
      key = subscript(A, n->b);
      e = arr_get(arr, key);	/* referencing it creates it, as in awk */
      str_unref(key);
      return val_copy(&e->v);
    }
    case AN_ASSIGN: {
      Val v;
      if (n->idx == 0) {
        v = eval(A, n->b);
        if (v.t == AT_UNINIT) {	/* x = y (unset): x is "" and 0 */
          v.t = AT_UNINIT;
        }
      }
      else {
        Val old = eval(A, n->a), rhs = eval(A, n->b);
        double r = arith(A, n->idx, to_num(&old), to_num(&rhs));
        val_free(&old);
        val_free(&rhs);
        v = val_num(r);
      }
      {
        Val ret = val_copy(&v);
        assign(A, n->a, v);
        return ret;
      }
    }
    case AN_COND: {
      Val c = eval(A, n->a);
      int t = to_bool(&c);
      val_free(&c);
      return eval(A, t ? n->b : n->c);
    }
    case AN_OR: case AN_AND: {
      Val a = eval(A, n->a);
      int t = to_bool(&a);
      val_free(&a);
      if (n->op == AN_OR && t) return val_num(1);
      if (n->op == AN_AND && !t) return val_num(0);
      a = eval(A, n->b);
      t = to_bool(&a);
      val_free(&a);
      return val_num(t);
    }
    case AN_IN: {
      Array *arr = array_of(A, n->b);
      Str *key;
      int r;
      if (arr == NULL) return val_num(0);
      key = n->a->op == AN_GROUP ? subscript(A, n->a->a) : subscript(A, n->a);
      r = arr_find(arr, key->s, key->len) != NULL;
      str_unref(key);
      return val_num(r);
    }
    case AN_MATCH: case AN_NOMATCH: {
      Val s = eval(A, n->a);
      Str *str = to_str(A, &s);
      Regex *re = regex_of(A, n->b);
      int r = re ? regex_match(re, str->s, str->len, 0, 0, NULL) : 0;
      val_free(&s);
      str_unref(str);
      return val_num(n->op == AN_MATCH ? r : !r);
    }
    case AN_LT: case AN_LE: case AN_GT: case AN_GE: case AN_EQ: case AN_NE: {
      Val a = eval(A, n->a), b = eval(A, n->b);
      int c = compare(A, &a, &b), r;
      val_free(&a);
      val_free(&b);
      switch (n->op) {
        case AN_LT: r = c < 0; break;
        case AN_LE: r = c <= 0; break;
        case AN_GT: r = c > 0; break;
        case AN_GE: r = c >= 0; break;
        case AN_EQ: r = c == 0; break;
        default: r = c != 0; break;
      }
      return val_num(r);
    }
    case AN_CONCAT: {
      Val a = eval(A, n->a), b = eval(A, n->b);
      Str *x = to_str(A, &a), *y = to_str(A, &b), *r;
      val_free(&a);
      val_free(&b);
      r = (Str *)xmalloc(sizeof(Str) + x->len + y->len);
      r->ref = 1;
      r->len = x->len + y->len;
      memcpy(r->s, x->s, x->len);
      memcpy(r->s + x->len, y->s, y->len);
      r->s[r->len] = '\0';
      str_unref(x);
      str_unref(y);
      return val_str(r);
    }
    case AN_ADD: case AN_SUB: case AN_MUL: case AN_DIV: case AN_MOD: case AN_POW: {
      Val a = eval(A, n->a), b = eval(A, n->b);
      double r = arith(A, n->op, to_num(&a), to_num(&b));
      val_free(&a);
      val_free(&b);
      return val_num(r);
    }
    case AN_NEG: case AN_PLUS: case AN_NOT: {
      Val a = eval(A, n->a);
      double r = n->op == AN_NEG ? -to_num(&a) : n->op == AN_PLUS ? to_num(&a) : !to_bool(&a);
      val_free(&a);
      return val_num(r);
    }
    case AN_PREINC: case AN_PREDEC: case AN_POSTINC: case AN_POSTDEC: {
      Val old = eval(A, n->a);
      double d = to_num(&old);
      double nv = (n->op == AN_PREINC || n->op == AN_POSTINC) ? d + 1 : d - 1;
      val_free(&old);
      assign(A, n->a, val_num(nv));
      return val_num((n->op == AN_PREINC || n->op == AN_PREDEC) ? nv : d);
    }
    case AN_CALL: return call_func(A, n);
    case AN_BUILTIN: return call_builtin(A, n);
    case AN_GETLINE: return do_getline(A, n);
  }
  return val_uninit();
}


static void print_to (Awk *A, ANode *n, Str *text) {	/* takes text */
  if (n->kind == AR_NONE) out_putn(&A->o, text->s, text->len);
  else {
    Val dv = eval(A, n->b);
    Str *dest = to_str(A, &dv);
    OStream *s;
    val_free(&dv);
    s = get_out(A, dest->s, n->kind);
    str_unref(dest);
    if (s) {
      if (s->kind == AR_PIPE) buf_putn(&s->pipe_data, text->s, text->len);
      else if (s->fd == -2) out_putn(&A->o, text->s, text->len);
      else {
        out_putn(&s->o, text->s, text->len);
        if (s->fd == A->err) out_flush(&s->o);
      }
    }
  }
  str_unref(text);
}


static int exec (Awk *A, ANode *n) {
  for (; n; n = n->next) {
    int r;
    if (A->fatal_err) return AX_EXIT;
    A->cur_line = n->line;
    if (tool_stop()) {
      A->exiting = 1;
      A->exit_code = 130;
      return AX_EXIT;
    }
    switch (n->op) {
      case AS_BLOCK:
        if ((r = exec(A, n->a)) != AX_NORMAL) return r;
        break;
      case AS_EXPR: {
        Val v = eval(A, n->a);
        val_free(&v);
        break;
      }
      case AS_PRINT: {
        Buf b;
        Str *ofs, *ors;
        buf_init(&b);
        ofs = to_str(A, &A->globals[AG_OFS].v);
        ors = to_str(A, &A->globals[AG_ORS].v);
        if (n->a == NULL) {
          rebuild_record(A);
          {
            Str *s = to_str(A, &A->rec);
            buf_putn(&b, s->s, s->len);
            str_unref(s);
          }
        }
        else {
          ANode *x;
          for (x = n->a; x; x = x->next) {
            Val v = eval(A, x);
            Str *s = to_str_out(A, &v);
            val_free(&v);
            if (x != n->a) buf_putn(&b, ofs->s, ofs->len);
            buf_putn(&b, s->s, s->len);
            str_unref(s);
          }
        }
        buf_putn(&b, ors->s, ors->len);
        str_unref(ofs);
        str_unref(ors);
        if (!A->fatal_err) print_to(A, n, str_new(b.s ? b.s : "", b.len));
        buf_free(&b);
        break;
      }
      case AS_PRINTF: {
        Val *args = (Val *)xmalloc((size_t)(n->nargs + 1) * sizeof(Val));
        int i;
        ANode *x;
        Str *f;
        for (i = 0, x = n->a; x; x = x->next, i++) args[i] = eval(A, x);
        f = to_str(A, &args[0]);
        if (!A->fatal_err) print_to(A, n, format(A, f, args + 1, n->nargs - 1));
        str_unref(f);
        for (i = 0; i < n->nargs; i++) val_free(&args[i]);
        free(args);
        break;
      }
      case AS_IF: {
        Val c = eval(A, n->a);
        int t = to_bool(&c);
        val_free(&c);
        if (t) {
          if ((r = exec(A, n->b)) != AX_NORMAL) return r;
        }
        else if (n->c && (r = exec(A, n->c)) != AX_NORMAL) return r;
        break;
      }
      case AS_WHILE:
        for (;;) {
          Val c = eval(A, n->a);
          int t = to_bool(&c);
          val_free(&c);
          if (!t) break;
          r = exec(A, n->b);
          if (r == AX_BREAK) break;
          if (r == AX_CONTINUE || r == AX_NORMAL) continue;
          return r;
        }
        break;
      case AS_DO:
        for (;;) {
          Val c;
          int t;
          r = exec(A, n->b);
          if (r == AX_BREAK) break;
          if (r != AX_CONTINUE && r != AX_NORMAL) return r;
          c = eval(A, n->a);
          t = to_bool(&c);
          val_free(&c);
          if (!t) break;
        }
        break;
      case AS_FOR:
        if (n->a && (r = exec(A, n->a)) != AX_NORMAL) return r;
        for (;;) {
          if (n->b) {
            Val c = eval(A, n->b);
            int t = to_bool(&c);
            val_free(&c);
            if (!t) break;
          }
          r = exec(A, n->d);
          if (r == AX_BREAK) break;
          if (r != AX_CONTINUE && r != AX_NORMAL) return r;
          if (n->c && (r = exec(A, n->c)) != AX_NORMAL) return r;
        }
        break;
      case AS_FORIN: {
        Array *arr = array_of(A, n->b);
        Str **keys;
        size_t nk, i;
        if (arr == NULL) break;
        keys = arr_keys(arr, &nk);
        r = AX_NORMAL;
        for (i = 0; i < nk; i++) {
          if (arr_find(arr, keys[i]->s, keys[i]->len) == NULL) continue;	/* deleted meanwhile */
          assign(A, n->a, val_input(str_ref(keys[i])));
          r = exec(A, n->c);
          if (r == AX_BREAK) {
            r = AX_NORMAL;
            break;
          }
          if (r == AX_CONTINUE) r = AX_NORMAL;
          if (r != AX_NORMAL) break;
        }
        for (i = 0; i < nk; i++) str_unref(keys[i]);
        free(keys);
        if (r != AX_NORMAL) return r;
        break;
      }
      case AS_BREAK: return AX_BREAK;
      case AS_CONTINUE: return AX_CONTINUE;
      case AS_NEXT: return AX_NEXT;
      case AS_NEXTFILE: return AX_NEXTFILE;
      case AS_EXIT:
        if (n->a) {
          Val v = eval(A, n->a);
          A->exit_code = (int)to_num(&v);
          val_free(&v);
        }
        A->exiting = 1;
        return AX_EXIT;
      case AS_RETURN:
        val_free(&A->retval);
        A->retval = n->a ? eval(A, n->a) : val_uninit();
        return AX_RETURN;
      case AS_DELETE: {
        Array *arr = array_of(A, n->a);
        if (arr == NULL) break;
        if (n->b) {
          Str *key = subscript(A, n->b);
          arr_delete(arr, key->s, key->len);
          str_unref(key);
        }
        else arr_clear(arr);
        break;
      }
    }
  }
  return AX_NORMAL;
}

/* }================================================================== */


/*
** {==================================================================
** The main loop
** ===================================================================
*/

static void set_gstr (Awk *A, int g, const char *s) {
  AVar *v = &A->globals[g];
  val_free(&v->v);
  v->v = val_str(str_c(s));
  v->kind = AK_SCALAR;
}


static void awk_free (Awk *A) {
  int i;
  for (i = 0; i < A->nrules; i++) {
    node_free(A->rules[i].pat);
    node_free(A->rules[i].pat2);
    node_free(A->rules[i].action);
  }
  free(A->rules);
  for (i = 0; i < A->nfuncs; i++) {
    int k;
    for (k = 0; k < A->funcs[i].nparams; k++) free(A->funcs[i].params[k]);
    free(A->funcs[i].params);
    free(A->funcs[i].name);
    node_free(A->funcs[i].body);
  }
  free(A->funcs);
  for (i = 0; i < A->nglobals; i++) {
    val_free(&A->globals[i].v);
    if (A->globals[i].own_arr) arr_free(A->globals[i].arr);
    free(A->gnames[i]);
  }
  free(A->globals);
  free(A->gnames);
  fields_clear(A);
  free(A->fields);
  val_free(&A->rec);
  for (i = 0; i < 64; i++)
    if (A->rcache[i].key) {
      str_unref(A->rcache[i].key);
      regex_free(A->rcache[i].re);
    }
  str_unref(A->tstr);
  str_unref(A->rec_fs);
  free(A->tname);
  free(A->outs);
  free(A->ins);
  if (A->cur_open) in_close(&A->cur);
}


int t_awk (int argc, char **argv, int in, int out, int err) {
  Awk *A = (Awk *)xmalloc(sizeof(Awk));
  Buf prog;
  Vec assigns;
  int i, have_prog = 0, k;
  const char *fs = NULL;
  memset(A, 0, sizeof(*A));
  g_awk = A;
  A->in = in;
  A->out = out;
  A->err = err;
  A->rec = val_uninit();
  A->retval = val_uninit();
  A->line = 1;
  A->rseed = 0;
  A->prev_seed = 0;
  out_init(&A->o, out);
  buf_init(&prog);
  vec_init(&assigns);
  for (k = 0; special_names[k]; k++) global_index(A, special_names[k]);
  /* options: -F fs, -v a=b, -f file, --; then the program, then operands */
  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--") == 0) {
      i++;
      break;
    }
    if (a[0] != '-' || a[1] == '\0') break;
    if (strcmp(a, "--version") == 0) {
      fd_printf(out, "awk (mmc) %s - POSIX awk with gawk extensions\n", MMC_VERSION);
      awk_free(A);
      free(A);
      buf_free(&prog);
      vec_free(&assigns);
      return 0;
    }
    if (strcmp(a, "--help") == 0) {
      awk_free(A);
      free(A);
      buf_free(&prog);
      vec_free(&assigns);
      return tool_help(out, "awk");
    }
    if (a[1] == 'F') {
      fs = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : NULL);
      continue;
    }
    if (a[1] == 'v') {
      const char *as = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : NULL);
      if (as == NULL || !is_assignment_arg(as)) {
        fd_printf(err, "awk: `%s' argument to `-v' not in `var=value' form\n", as ? as : "");
        awk_free(A);
        free(A);
        buf_free(&prog);
        vec_free(&assigns);
        return 2;
      }
      vec_push(&assigns, xstrdup(as));
      continue;
    }
    if (a[1] == 'f') {
      const char *file = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : NULL);
      char *native, *text;
      size_t len;
      if (file == NULL) break;
      native = path_to_native(file);
      text = strcmp(file, "-") == 0 ? NULL : read_file(native, &len);
      free(native);
      if (text == NULL) {
        fd_printf(err, "awk: fatal: can't open source file `%s' for reading: %s\n", file, os_errmsg());
        awk_free(A);
        free(A);
        buf_free(&prog);
        vec_free(&assigns);
        return 2;
      }
      crlf_to_lf(text, &len);
      buf_putn(&prog, text, len);
      buf_putc(&prog, '\n');
      free(text);
      have_prog = 1;
      A->srcname = file;
      continue;
    }
    if (strcmp(a, "--posix") == 0 || strcmp(a, "--traditional") == 0 || strcmp(a, "-P") == 0 ||
        strncmp(a, "--re-interval", 13) == 0 || strcmp(a, "-b") == 0 || strcmp(a, "--characters-as-bytes") == 0)
      continue;
    fd_printf(err, "awk: invalid option -- '%c'\nusage: awk [-F fs][-v var=value][prog | -f progfile][file ...]\n", a[1]);
    awk_free(A);
    free(A);
    buf_free(&prog);
    vec_free(&assigns);
    return 2;
  }
  if (!have_prog) {
    if (i >= argc) {
      fd_printf(err, "usage: awk [-F fs][-v var=value][prog | -f progfile][file ...]\n");
      awk_free(A);
      free(A);
      buf_free(&prog);
      vec_free(&assigns);
      return 2;
    }
    buf_puts(&prog, argv[i++]);
  }
  /* the special variables */
  set_gstr(A, AG_FS, " ");
  set_gstr(A, AG_OFS, " ");
  set_gstr(A, AG_ORS, "\n");
  set_gstr(A, AG_RS, "\n");
  set_gstr(A, AG_SUBSEP, "\034");
  set_gstr(A, AG_CONVFMT, "%.6g");
  set_gstr(A, AG_OFMT, "%.6g");
  set_gstr(A, AG_FILENAME, "");
  set_num_var(A, AG_NR, 0);
  set_num_var(A, AG_NF, 0);
  set_num_var(A, AG_FNR, 0);
  set_num_var(A, AG_RSTART, 0);
  set_num_var(A, AG_RLENGTH, -1);
  A->globals[AG_IGNORECASE].v = val_uninit();
  {	/* ENVIRON */
    Vec env;
    size_t e;
    Array *arr;
    A->globals[AG_ENVIRON].kind = AK_ARRAY;
    A->globals[AG_ENVIRON].arr = arr = arr_new();
    A->globals[AG_ENVIRON].own_arr = 1;
    vec_init(&env);
    var_env(&env);
    for (e = 0; e < env.n; e++) {
      char *eq = strchr(env.v[e], '=');
      Str *key;
      AElem *el;
      if (!eq) continue;
      key = str_new(env.v[e], (size_t)(eq - env.v[e]));
      el = arr_get(arr, key);
      str_unref(key);
      el->v = val_input(str_c(eq + 1));
    }
    vec_free(&env);
  }
  {	/* ARGV: awk, then the operands */
    Array *arr;
    int n = 1, j;
    char num[24];
    A->globals[AG_ARGV].kind = AK_ARRAY;
    A->globals[AG_ARGV].arr = arr = arr_new();
    A->globals[AG_ARGV].own_arr = 1;
    {
      Str *key = str_c("0");
      AElem *el = arr_get(arr, key);
      str_unref(key);
      el->v = val_str(str_c("awk"));
    }
    for (j = i; j < argc; j++, n++) {
      Str *key = str_c(ll_to_str(n, num));
      AElem *el = arr_get(arr, key);
      str_unref(key);
      el->v = val_input(str_c(argv[j]));
    }
    set_num_var(A, AG_ARGC, n);
  }
  A->argi = 1;
  if (fs) {
    if (strcmp(fs, "t") == 0) set_gstr(A, AG_FS, "\t");
    else {
      Buf b;
      const char *p;
      buf_init(&b);
      for (p = fs; *p; p++) {	/* -F '\t' */
        if (*p == '\\' && p[1] == 't') {
          buf_putc(&b, '\t');
          p++;
        }
        else if (*p == '\\' && p[1] == '\\') {
          buf_putc(&b, '\\');
          p++;
        }
        else buf_putc(&b, *p);
      }
      set_gstr(A, AG_FS, b.s ? b.s : "");
      buf_free(&b);
    }
  }
  for (k = 0; k < (int)assigns.n; k++) cmdline_assign(A, assigns.v[k]);
  /* parse */
  A->src = prog.s ? prog.s : "";
  A->p = A->src;
  A->end = A->src + prog.len;
  parse_program(A);
  if (A->errors) {
    A->exit_code = 1;
    goto done;
  }
  /* BEGIN */
  for (k = 0; k < A->nrules && !A->exiting; k++)
    if (A->rules[k].kind == 1) {
      int r = exec(A, A->rules[k].action);
      if (r == AX_EXIT) break;
    }
  /* the records, unless there are only BEGIN rules */
  {
    int main_rules = 0, end_rules = 0;
    for (k = 0; k < A->nrules; k++) {
      if (A->rules[k].kind == 0) main_rules = 1;
      if (A->rules[k].kind == 2) end_rules = 1;
    }
    if (!A->exiting && (main_rules || end_rules)) {
      Str *rec;
      while (!A->exiting && next_main_record(A, &rec) == 1) {
        set_record(A, rec);
        for (k = 0; k < A->nrules && !A->exiting; k++) {
          ARule *r = &A->rules[k];
          int hit, x;
          if (r->kind != 0) continue;
          if (r->pat2) {	/* a range */
            if (!r->in_range) {
              Val v = eval(A, r->pat);
              hit = to_bool(&v);
              val_free(&v);
              if (hit) {
                Val w = eval(A, r->pat2);
                r->in_range = !to_bool(&w);
                val_free(&w);
              }
            }
            else {
              Val w = eval(A, r->pat2);
              hit = 1;
              if (to_bool(&w)) r->in_range = 0;
              val_free(&w);
            }
          }
          else if (r->pat) {
            Val v = eval(A, r->pat);
            hit = to_bool(&v);
            val_free(&v);
          }
          else hit = 1;
          if (!hit) continue;
          if (r->action == NULL) {
            Str *s;
            Str *ors = to_str(A, &A->globals[AG_ORS].v);
            rebuild_record(A);
            s = to_str(A, &A->rec);
            out_putn(&A->o, s->s, s->len);
            out_putn(&A->o, ors->s, ors->len);
            str_unref(s);
            str_unref(ors);
            continue;
          }
          x = exec(A, r->action);
          if (x == AX_NEXT) break;
          if (x == AX_NEXTFILE) {
            if (A->cur_open) {
              in_close(&A->cur);
              A->cur_open = 0;
            }
            break;
          }
          if (x == AX_EXIT) break;
        }
        if (A->o.failed) break;
      }
    }
  }
  /* END, even after exit (but not after an exit in END, or a fatal error) */
  if (!A->fatal_err && !tool_stop()) {
    A->exiting = 0;
    A->in_end = 1;
    for (k = 0; k < A->nrules && !A->exiting; k++)
      if (A->rules[k].kind == 2) {
        int r = exec(A, A->rules[k].action);
        if (r == AX_EXIT) break;
      }
  }
done:
  /* output files and pipes are closed at the end */
  out_flush(&A->o);
  while (A->nouts > 0) {
    close_out(A, &A->outs[A->nouts - 1]);
    A->nouts--;
  }
  while (A->nins > 0) {
    close_in(A, &A->ins[A->nins - 1]);
    A->nins--;
  }
  out_flush(&A->o);
  k = A->exit_code;
  awk_free(A);
  free(A);
  buf_free(&prog);
  vec_free(&assigns);
  return tool_stop() ? 130 : k;
}

/* }================================================================== */
