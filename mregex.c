/*
** mregex.c - POSIX regular expressions: for [[ str =~ re ]], grep, sed,
** awk and find -regex
**
**   basic (BRE, grep and sed):   \( \) \{m,n\} \| \+ \?  * . [] ^ $
**   extended (ERE, -E, awk):     ( ) {m,n} | + ? * . [] ^ $
**   both, GNU style:  [[:alpha:]] \w \W \s \S \b \B \< \> \` \' \1..\9
** Text is UTF-8: '.' and [...] take a whole character.
**
** Two matchers share one parsed tree. Without back references the tree is
** compiled into a program for a Pike VM: every possible path runs side
** by side, one character at a time, so time grows with the text, never
** exponentially; it gives POSIX's leftmost-longest match. With back
** references a backtracking matcher walks the tree instead.
*/

#include "mmc.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>


enum {
  R_CHAR, R_ANY, R_SET, R_BOL, R_EOL, R_WORDB, R_NWORDB, R_WBEG, R_WEND,
  R_BUFBEG, R_BUFEND, R_NWBEFORE, R_NWAFTER, R_GROUP, R_BACKREF
};

typedef struct RSet {
  unsigned char bits[32];	/* characters below 256 */
  unsigned *ranges;	/* lo, hi pairs, 256 and up */
  int nranges;
  int wide_word;	/* letters beyond 255 count ([:alpha:] \w ...) */
  int neg;
} RSet;

typedef struct RSeq RSeq;

typedef struct RNode {
  int type;
  int min, max;	/* repetition; max -1: no limit */
  unsigned cp;	/* R_CHAR */
  int set;	/* R_SET: index in Regex.sets */
  int group;	/* R_GROUP: capture number, -1 none; R_BACKREF: which */
  RSeq *alts;	/* R_GROUP: alternatives */
  int nalts;
} RNode;

struct RSeq {
  RNode *v;
  int n, cap;
};

enum { I_CHAR, I_ANY, I_SET, I_SPLIT, I_JMP, I_SAVE, I_ASSERT, I_MATCH };

typedef struct RInst {
  unsigned char op;
  unsigned char kind;	/* I_ASSERT: R_BOL ... */
  int x, y;	/* jump targets; I_SAVE: slot; I_SET: set */
  unsigned cp;
} RInst;

typedef struct Thread {
  int pc;
  size_t *caps;
} Thread;

struct Regex {
  RNode top;	/* group 0 */
  int ngroups;
  int flags;
  RSet *sets;
  int nsets;
  int has_backref;
  RInst *prog;
  int nprog, progcap;
  char lit[64];	/* every match starts with these bytes */
  size_t litlen;
  int anchored;	/* starts with ^ */
  /* the VM's working memory, kept between calls */
  Thread *cl, *nl;
  size_t *cstore, *nstore, *scratch;
  int *mark;
  unsigned gen;
  int ncap_alloc;
};

#define PROG_MAX	200000


/*
** {==================================================================
** Characters
** ===================================================================
*/

/* the character at s (before end); *len its bytes. Bad UTF-8: one byte,
** as 0xDC80 + byte, which matches only itself */
static unsigned dec (const char *s, const char *end, int *len) {
  const unsigned char *p = (const unsigned char *)s;
  unsigned c = p[0], cp;
  int n, k;
  if (c < 0x80) { *len = 1; return c; }
  if ((c & 0xE0) == 0xC0) { n = 2; cp = c & 0x1F; }
  else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0F; }
  else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07; }
  else { *len = 1; return 0xDC00 + c; }
  if (end - s < n) { *len = 1; return 0xDC00 + c; }
  for (k = 1; k < n; k++) {
    if ((p[k] & 0xC0) != 0x80) { *len = 1; return 0xDC00 + c; }
    cp = (cp << 6) | (p[k] & 0x3F);
  }
  *len = n;
  return cp;
}


/* the character before s (s > begin) */
static unsigned dec_prev (const char *begin, const char *s) {
  const char *p = s - 1;
  int len;
  int k;
  for (k = 0; k < 3 && p > begin && ((unsigned char)*p & 0xC0) == 0x80; k++) p--;
  {
    unsigned cp = dec(p, s, &len);
    if (p + len != s) return 0xDC00 + (unsigned char)s[-1];
    return cp;
  }
}


static unsigned fold_lower (unsigned c) {
  if (c < 128) return (unsigned)tolower((int)c);
  if ((c >= 0xC0 && c <= 0xDE && c != 0xD7) || (c >= 0x391 && c <= 0x3A9) ||
      (c >= 0x410 && c <= 0x42F))
    return c + 0x20;
  if (c >= 0x400 && c <= 0x40F) return c + 0x50;
  return c;
}


static unsigned fold_upper (unsigned c) {
  if (c < 128) return (unsigned)toupper((int)c);
  if ((c >= 0xE0 && c <= 0xFE && c != 0xF7) || (c >= 0x3B1 && c <= 0x3C9) ||
      (c >= 0x430 && c <= 0x44F))
    return c - 0x20;
  if (c >= 0x450 && c <= 0x45F) return c - 0x50;
  return c;
}


static int is_word (unsigned c) {
  if (c < 128) return isalnum((int)c) || c == '_';
  return c >= 0xC0 && c < 0xDC00 && c != 0xD7 && c != 0xF7;
}


static int in_set_raw (const RSet *s, unsigned c) {
  int i;
  if (c < 256) return (s->bits[c >> 3] >> (c & 7)) & 1;
  for (i = 0; i < s->nranges; i++)
    if (c >= s->ranges[2 * i] && c <= s->ranges[2 * i + 1]) return 1;
  return s->wide_word && c < 0xDC00;
}


static int in_set (const Regex *re, const RSet *s, unsigned c) {
  int hit = in_set_raw(s, c);
  if (!hit && (re->flags & RE_ICASE))
    hit = in_set_raw(s, fold_lower(c)) || in_set_raw(s, fold_upper(c));
  return hit ^ s->neg;
}

/* }================================================================== */


/*
** {==================================================================
** Parsing
** ===================================================================
*/

typedef struct RParse {
  const char *p, *end;
  int ngroups;
  int flags;
  int ere;
  char *err;
  Regex *re;
  int depth;
} RParse;


static RNode *seq_add (RSeq *s) {
  if (s->n + 1 > s->cap) {
    s->cap = s->cap ? s->cap * 2 : 8;
    s->v = (RNode *)xrealloc(s->v, (size_t)s->cap * sizeof(RNode));
  }
  memset(&s->v[s->n], 0, sizeof(RNode));
  s->v[s->n].min = s->v[s->n].max = 1;
  s->v[s->n].group = -1;
  return &s->v[s->n++];
}


static void node_free (RNode *n);

static void seq_free (RSeq *s) {
  int i;
  for (i = 0; i < s->n; i++) node_free(&s->v[i]);
  free(s->v);
  s->v = NULL;
  s->n = s->cap = 0;
}


static void node_free (RNode *n) {
  int i;
  for (i = 0; i < n->nalts; i++) seq_free(&n->alts[i]);
  free(n->alts);
  n->alts = NULL;
  n->nalts = 0;
}


static int new_set (Regex *re) {
  re->sets = (RSet *)xrealloc(re->sets, (size_t)(re->nsets + 1) * sizeof(RSet));
  memset(&re->sets[re->nsets], 0, sizeof(RSet));
  return re->nsets++;
}


static void set_add (RSet *s, unsigned lo, unsigned hi) {
  unsigned c;
  for (c = lo; c <= hi && c < 256; c++) s->bits[c >> 3] |= (unsigned char)(1 << (c & 7));
  if (hi >= 256) {
    if (lo < 256) lo = 256;
    s->ranges = (unsigned *)xrealloc(s->ranges, (size_t)(s->nranges + 1) * 2 * sizeof(unsigned));
    s->ranges[2 * s->nranges] = lo;
    s->ranges[2 * s->nranges + 1] = hi;
    s->nranges++;
  }
}


static int class_match (const char *name, int c) {
  if (strcmp(name, "alpha") == 0) return isalpha(c);
  if (strcmp(name, "digit") == 0) return isdigit(c);
  if (strcmp(name, "alnum") == 0) return isalnum(c);
  if (strcmp(name, "upper") == 0) return isupper(c);
  if (strcmp(name, "lower") == 0) return islower(c);
  if (strcmp(name, "space") == 0) return isspace(c);
  if (strcmp(name, "blank") == 0) return c == ' ' || c == '\t';
  if (strcmp(name, "punct") == 0) return ispunct(c);
  if (strcmp(name, "print") == 0) return isprint(c);
  if (strcmp(name, "graph") == 0) return isgraph(c);
  if (strcmp(name, "cntrl") == 0) return iscntrl(c);
  if (strcmp(name, "xdigit") == 0) return isxdigit(c);
  if (strcmp(name, "word") == 0) return isalnum(c) || c == '_';
  return -1;
}


static void add_class (RSet *s, const char *name) {
  int c;
  for (c = 1; c < 128; c++)
    if (class_match(name, c) > 0) set_add(s, (unsigned)c, (unsigned)c);
  /* letters past ASCII */
  if (strcmp(name, "alpha") == 0 || strcmp(name, "alnum") == 0 || strcmp(name, "word") == 0 ||
      strcmp(name, "print") == 0 || strcmp(name, "graph") == 0 ||
      ((strcmp(name, "upper") == 0 || strcmp(name, "lower") == 0) && 0)) {
    set_add(s, 0xC0, 0xD6);
    set_add(s, 0xD8, 0xF6);
    set_add(s, 0xF8, 0xFF);
    s->wide_word = 1;
  }
  if (strcmp(name, "upper") == 0) {
    set_add(s, 0xC0, 0xD6);
    set_add(s, 0xD8, 0xDE);
    set_add(s, 0x391, 0x3A9);
    set_add(s, 0x400, 0x42F);
  }
  if (strcmp(name, "lower") == 0) {
    set_add(s, 0xDF, 0xF6);
    set_add(s, 0xF8, 0xFF);
    set_add(s, 0x3B1, 0x3C9);
    set_add(s, 0x430, 0x45F);
  }
  if (strcmp(name, "space") == 0) set_add(s, 0xA0, 0xA0);
}


static unsigned take (RParse *rp) {
  int len;
  unsigned c = dec(rp->p, rp->end, &len);
  rp->p += len;
  return c;
}


/* [...]: rp->p is after the '[' */
static int parse_bracket (RParse *rp, RNode *n) {
  int si = new_set(rp->re), first = 1;
  RSet *s;
  n->type = R_SET;
  n->set = si;
  s = &rp->re->sets[si];
  if (rp->p < rp->end && *rp->p == '^') {
    s->neg = 1;
    rp->p++;
  }
  while (rp->p < rp->end && (first || *rp->p != ']')) {
    unsigned lo, hi;
    first = 0;
    s = &rp->re->sets[si];
    if (rp->p[0] == '[' && rp->p + 1 < rp->end && rp->p[1] == ':') {
      const char *e = rp->p + 2;
      char name[16];
      size_t k;
      while (e + 1 < rp->end && !(e[0] == ':' && e[1] == ']')) e++;
      k = (size_t)(e - rp->p - 2);
      if (e + 1 >= rp->end || k >= sizeof(name)) {
        rp->err = xstrdup("unterminated character class");
        return -1;
      }
      memcpy(name, rp->p + 2, k);
      name[k] = '\0';
      if (class_match(name, 'a') < 0) {
        rp->err = xstrdup("invalid character class");
        return -1;
      }
      add_class(s, name);
      rp->p = e + 2;
      continue;
    }
    if (rp->p[0] == '[' && rp->p + 1 < rp->end && (rp->p[1] == '.' || rp->p[1] == '=')) {
      char end = rp->p[1];
      rp->p += 2;
      lo = take(rp);
      while (rp->p + 1 < rp->end && !(rp->p[0] == end && rp->p[1] == ']')) rp->p++;
      if (rp->p + 1 < rp->end) rp->p += 2;
      set_add(s, lo, lo);
      continue;
    }
    if ((rp->flags & RE_AWK) && rp->p[0] == '\\' && rp->p + 1 < rp->end) {
      char e = rp->p[1];
      rp->p += 2;
      lo = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'f' ? '\f' :
           e == 'v' ? '\v' : e == 'a' ? '\a' : e == 'b' ? '\b' : (unsigned char)e;
    }
    else lo = take(rp);
    hi = lo;
    if (rp->p + 1 < rp->end && rp->p[0] == '-' && rp->p[1] != ']') {
      rp->p++;
      if ((rp->flags & RE_AWK) && rp->p[0] == '\\' && rp->p + 1 < rp->end) {
        rp->p++;
        hi = take(rp);
      }
      else hi = take(rp);
      if (hi < lo) {
        rp->err = xstrdup("invalid range end");
        return -1;
      }
    }
    set_add(s, lo, hi);
  }
  if (rp->p >= rp->end) {
    rp->err = xstrdup("unmatched [, [^, [:, [., or [=");
    return -1;
  }
  rp->p++;
  return 0;
}


static int parse_alts (RParse *rp, RNode *group);


/* at the start of an expression, after ( or | : where * is a literal in BRE */
static int at_expr_start (const RParse *rp, const RSeq *s) {
  return s->n == 0 || (s->n == 1 && s->v[0].type == R_BOL && !rp->ere);
}


static int is_alt_bar (const RParse *rp) {
  if (rp->ere) return rp->p < rp->end && *rp->p == '|';
  return rp->p + 1 < rp->end && rp->p[0] == '\\' && rp->p[1] == '|';
}


static int is_close (const RParse *rp) {
  if (rp->ere) return rp->p < rp->end && *rp->p == ')' && rp->depth > 0;
  return rp->p + 1 < rp->end && rp->p[0] == '\\' && rp->p[1] == ')';
}


/* {m,n} / \{m,n\} at rp->p; 1 found, 0 not an interval, -1 error */
static int parse_interval (RParse *rp, int *min, int *max) {
  const char *q = rp->p;
  int lo = 0, hi, have_lo = 0;
  if (rp->ere) {
    if (*q != '{') return 0;
    q++;
  }
  else {
    if (!(q[0] == '\\' && q + 1 < rp->end && q[1] == '{')) return 0;
    q += 2;
  }
  while (q < rp->end && isdigit((unsigned char)*q)) {
    lo = lo * 10 + (*q++ - '0');
    have_lo = 1;
    if (lo > 32767) goto bad;
  }
  hi = lo;
  if (q < rp->end && *q == ',') {
    q++;
    if (q < rp->end && isdigit((unsigned char)*q)) {
      hi = 0;
      while (q < rp->end && isdigit((unsigned char)*q)) {
        hi = hi * 10 + (*q++ - '0');
        if (hi > 32767) goto bad;
      }
    }
    else hi = -1;
    if (!have_lo) lo = 0;
  }
  else if (!have_lo) {
    if (rp->ere) return 0;	/* a literal { */
    goto bad;
  }
  if (rp->ere) {
    if (q >= rp->end || *q != '}') return 0;
    q++;
  }
  else {
    if (!(q + 1 < rp->end && q[0] == '\\' && q[1] == '}')) goto bad;
    q += 2;
  }
  if (hi >= 0 && hi < lo) {
    rp->err = xstrdup("invalid content of \\{\\}");
    return -1;
  }
  rp->p = q;
  *min = lo;
  *max = hi;
  return 1;
bad:
  rp->err = xstrdup(rp->ere ? "invalid content of {}" : "invalid content of \\{\\}");
  return -1;
}


static int parse_seq (RParse *rp, RSeq *s) {
  while (rp->p < rp->end && !is_alt_bar(rp) && !is_close(rp)) {
    RNode *n;
    unsigned c;
    int is_atom = 1;
    /* a repetition with nothing before it is a literal */
    if (at_expr_start(rp, s) && (*rp->p == '*' || (rp->ere && (*rp->p == '+' || *rp->p == '?')))) {
      n = seq_add(s);
      n->type = R_CHAR;
      n->cp = (unsigned char)*rp->p++;
      goto repeat;
    }
    n = seq_add(s);
    c = (unsigned char)*rp->p;
    if (c == '.') { rp->p++; n->type = R_ANY; }
    else if (c == '[') {
      rp->p++;
      if (parse_bracket(rp, n) != 0) return -1;
    }
    else if (c == '^' && (rp->ere || s->n == 1)) {	/* BRE: only at the start */
      rp->p++;
      n->type = R_BOL;
      is_atom = 0;
    }
    else if (c == '$' && (rp->ere || rp->p + 1 == rp->end ||
                          (rp->p + 2 < rp->end && rp->p[1] == '\\' &&
                           (rp->p[2] == ')' || rp->p[2] == '|')))) {
      rp->p++;
      n->type = R_EOL;
      is_atom = 0;
    }
    else if (rp->ere && c == '(') {
      rp->p++;
      goto group;
    }
    else if (c == '\\' && rp->p + 1 < rp->end) {
      char e = rp->p[1];
      rp->p += 2;
      if (!rp->ere && e == '(') goto group;
      if (e >= '1' && e <= '9') {
        n->type = R_BACKREF;
        n->group = e - '0';
        if (n->group > rp->ngroups) {
          rp->err = xstrdup("invalid back reference");
          return -1;
        }
        rp->re->has_backref = 1;
      }
      else if (e == 'b') { n->type = R_WORDB; is_atom = 0; }
      else if (e == 'B') { n->type = R_NWORDB; is_atom = 0; }
      else if (e == '<') { n->type = R_WBEG; is_atom = 0; }
      else if (e == '>') { n->type = R_WEND; is_atom = 0; }
      else if (e == '`') { n->type = R_BUFBEG; is_atom = 0; }
      else if (e == '\'') { n->type = R_BUFEND; is_atom = 0; }
      else if (strchr("wWsSdD", e)) {
        int si = new_set(rp->re);
        RSet *set = &rp->re->sets[si];
        n->type = R_SET;
        n->set = si;
        if (e == 'w' || e == 'W') add_class(set, "word");
        else if (e == 's' || e == 'S') add_class(set, "space");
        else add_class(set, "digit");
        set->neg = isupper((unsigned char)e) != 0;
      }
      else {
        n->type = R_CHAR;
        n->cp = e == 'n' ? '\n' : e == 't' ? '\t' : (unsigned char)e;
        if ((unsigned char)e >= 0x80) {	/* an escaped UTF-8 character */
          rp->p--;
          n->cp = take(rp);
        }
      }
    }
    else {
      n->type = R_CHAR;
      n->cp = take(rp);
    }
    goto repeat;
  group:
    n->type = R_GROUP;
    n->group = ++rp->ngroups;
    rp->depth++;
    if (parse_alts(rp, n) != 0) return -1;
    rp->depth--;
    if (rp->ere) {
      if (rp->p >= rp->end || *rp->p != ')') {
        rp->err = xstrdup("unmatched ( or \\(");
        return -1;
      }
      rp->p++;
    }
    else {
      if (!(rp->p + 1 < rp->end && rp->p[0] == '\\' && rp->p[1] == ')')) {
        rp->err = xstrdup("unmatched ( or \\(");
        return -1;
      }
      rp->p += 2;
    }
  repeat:
    for (;;) {
      int min, max, r;
      if (rp->p >= rp->end) break;
      if (*rp->p == '*') { min = 0; max = -1; rp->p++; }
      else if (rp->ere && *rp->p == '+') { min = 1; max = -1; rp->p++; }
      else if (rp->ere && *rp->p == '?') { min = 0; max = 1; rp->p++; }
      else if (!rp->ere && rp->p + 1 < rp->end && rp->p[0] == '\\' && rp->p[1] == '+') {
        min = 1; max = -1; rp->p += 2;
      }
      else if (!rp->ere && rp->p + 1 < rp->end && rp->p[0] == '\\' && rp->p[1] == '?') {
        min = 0; max = 1; rp->p += 2;
      }
      else if ((r = parse_interval(rp, &min, &max)) != 0) {
        if (r < 0) return -1;
      }
      else break;
      if (!is_atom) continue;	/* ^* : GNU ignores it */
      if (n->min != 1 || n->max != 1) {	/* x** or x{2}{3}: wrap in a group */
        RNode inner = *n;
        memset(n, 0, sizeof(*n));
        n->type = R_GROUP;
        n->group = -1;
        n->alts = (RSeq *)xmalloc(sizeof(RSeq));
        memset(n->alts, 0, sizeof(RSeq));
        n->nalts = 1;
        *seq_add(&n->alts[0]) = inner;
      }
      n->min = min;
      n->max = max;
    }
  }
  return 0;
}


static int parse_alts (RParse *rp, RNode *group) {
  for (;;) {
    group->alts = (RSeq *)xrealloc(group->alts, (size_t)(group->nalts + 1) * sizeof(RSeq));
    memset(&group->alts[group->nalts], 0, sizeof(RSeq));
    if (parse_seq(rp, &group->alts[group->nalts++]) != 0) return -1;
    if (!is_alt_bar(rp)) return 0;
    rp->p += rp->ere ? 1 : 2;
  }
}

/* }================================================================== */


/*
** {==================================================================
** Compiling for the Pike VM
** ===================================================================
*/

static int emit (Regex *re, int op) {
  if (re->nprog >= re->progcap) {
    re->progcap = re->progcap ? re->progcap * 2 : 64;
    re->prog = (RInst *)xrealloc(re->prog, (size_t)re->progcap * sizeof(RInst));
  }
  memset(&re->prog[re->nprog], 0, sizeof(RInst));
  re->prog[re->nprog].op = (unsigned char)op;
  return re->nprog++;
}


static int comp_node (Regex *re, const RNode *n);

static int comp_seq (Regex *re, const RSeq *s) {
  int i;
  for (i = 0; i < s->n; i++)
    if (comp_node(re, &s->v[i]) != 0) return -1;
  return 0;
}


/* one occurrence of n, no repetition */
static int comp_once (Regex *re, const RNode *n) {
  int k;
  if (re->nprog > PROG_MAX) return -1;
  switch (n->type) {
    case R_CHAR:
      k = emit(re, I_CHAR);
      re->prog[k].cp = n->cp;
      return 0;
    case R_ANY:
      emit(re, I_ANY);
      return 0;
    case R_SET:
      k = emit(re, I_SET);
      re->prog[k].x = n->set;
      return 0;
    case R_GROUP: {
      int a, *jumps, nj = 0;
      if (n->group >= 0) {
        k = emit(re, I_SAVE);
        re->prog[k].x = 2 * n->group;
      }
      jumps = (int *)xmalloc((size_t)(n->nalts + 1) * sizeof(int));
      for (a = 0; a < n->nalts; a++) {
        int split = -1;
        if (a + 1 < n->nalts) split = emit(re, I_SPLIT);
        if (split >= 0) re->prog[split].x = re->nprog;
        if (comp_seq(re, &n->alts[a]) != 0) {
          free(jumps);
          return -1;
        }
        if (a + 1 < n->nalts) {
          jumps[nj++] = emit(re, I_JMP);
          re->prog[split].y = re->nprog;
        }
      }
      for (a = 0; a < nj; a++) re->prog[jumps[a]].x = re->nprog;
      free(jumps);
      if (n->group >= 0) {
        k = emit(re, I_SAVE);
        re->prog[k].x = 2 * n->group + 1;
      }
      return 0;
    }
    default:	/* anchors */
      k = emit(re, I_ASSERT);
      re->prog[k].kind = (unsigned char)n->type;
      return 0;
  }
}


static int comp_node (Regex *re, const RNode *n) {
  int i, k;
  if (n->min == 1 && n->max == 1) return comp_once(re, n);
  for (i = 0; i < n->min; i++)
    if (comp_once(re, n) != 0) return -1;
  if (n->max < 0) {	/* L: split body, out; body; jmp L */
    int split = emit(re, I_SPLIT);
    re->prog[split].x = re->nprog;
    if (comp_once(re, n) != 0) return -1;
    k = emit(re, I_JMP);
    re->prog[k].x = split;
    re->prog[split].y = re->nprog;
  }
  else {	/* (body (body ...)?)? */
    int opt = n->max - n->min, *splits = (int *)xmalloc((size_t)(opt + 1) * sizeof(int));
    for (i = 0; i < opt; i++) {
      splits[i] = emit(re, I_SPLIT);
      re->prog[splits[i]].x = re->nprog;
      if (comp_once(re, n) != 0) {
        free(splits);
        return -1;
      }
    }
    for (i = 0; i < opt; i++) re->prog[splits[i]].y = re->nprog;
    free(splits);
  }
  return re->nprog > PROG_MAX ? -1 : 0;
}


/* a literal every match starts with, for a quick skip */
/* the literal bytes that start every match in seq s (from item i on);
** 0 when it stops early, 1 when all of s was literal */
static int lit_walk (Regex *re, const RSeq *s, int i, int *started) {
  for (; i < s->n; i++) {
    const RNode *n = &s->v[i];
    char buf[4];
    int len;
    unsigned c;
    if (n->min == 1 && n->max == 1) {
      if (n->type == R_BOL || n->type == R_BUFBEG) {
        if (!*started && !(re->flags & RE_NEWLINE)) re->anchored = 1;
        continue;
      }
      if (n->type == R_NWBEFORE || n->type == R_WBEG || n->type == R_WORDB) continue;
      if (n->type == R_GROUP && n->nalts == 1) {
        if (!lit_walk(re, &n->alts[0], 0, started)) return 0;
        continue;
      }
    }
    if (n->type != R_CHAR || n->min < 1 || (re->flags & RE_ICASE)) return 0;
    *started = 1;
    c = n->cp;
    if (c >= 0xDC00 && c < 0xDD00) { buf[0] = (char)(c - 0xDC00); len = 1; }
    else if (c < 0x80) { buf[0] = (char)c; len = 1; }
    else if (c < 0x800) { buf[0] = (char)(0xC0 | (c >> 6)); buf[1] = (char)(0x80 | (c & 0x3F)); len = 2; }
    else if (c < 0x10000) {
      buf[0] = (char)(0xE0 | (c >> 12)); buf[1] = (char)(0x80 | ((c >> 6) & 0x3F));
      buf[2] = (char)(0x80 | (c & 0x3F)); len = 3;
    }
    else {
      buf[0] = (char)(0xF0 | (c >> 18)); buf[1] = (char)(0x80 | ((c >> 12) & 0x3F));
      buf[2] = (char)(0x80 | ((c >> 6) & 0x3F)); buf[3] = (char)(0x80 | (c & 0x3F)); len = 4;
    }
    if (re->litlen + (size_t)len > sizeof(re->lit)) return 0;
    memcpy(re->lit + re->litlen, buf, (size_t)len);
    re->litlen += (size_t)len;
    if (n->max != 1) return 0;	/* a+ : only the first one is sure */
  }
  return 1;
}


static void find_literal (Regex *re) {
  int started = 0;
  re->litlen = 0;
  re->anchored = 0;
  if (re->top.nalts != 1) return;
  lit_walk(re, &re->top.alts[0], 0, &started);
}


Regex *regex_new (const char *pat, int flags, char **err) {
  return regex_new_n(pat, strlen(pat), flags, err);
}


Regex *regex_new_n (const char *pat, size_t len, int flags, char **err) {
  Regex *re = (Regex *)xmalloc(sizeof(Regex));
  RParse rp;
  int k;
  memset(re, 0, sizeof(*re));
  re->flags = flags;
  rp.p = pat;
  rp.end = pat + len;
  rp.ngroups = 0;
  rp.flags = flags;
  rp.ere = (flags & RE_EXTENDED) != 0;
  rp.err = NULL;
  rp.re = re;
  rp.depth = 0;
  re->top.type = R_GROUP;
  re->top.group = 0;
  re->top.min = re->top.max = 1;
  if (err) *err = NULL;
  if (parse_alts(&rp, &re->top) != 0 || rp.p < rp.end) {
    if (rp.err == NULL) rp.err = xstrdup("unmatched ) or \\)");
    if (err) *err = rp.err;
    else free(rp.err);
    regex_free(re);
    return NULL;
  }
  re->ngroups = rp.ngroups;
  if (flags & (RE_WORDS | RE_WHOLE)) {	/* grep -w -x: the match inside checks */
    RNode inner = re->top;
    RSeq *s;
    RNode *n;
    inner.group = -1;
    memset(&re->top, 0, sizeof(re->top));
    re->top.type = R_GROUP;
    re->top.group = 0;
    re->top.min = re->top.max = 1;
    re->top.alts = (RSeq *)xmalloc(sizeof(RSeq));
    memset(re->top.alts, 0, sizeof(RSeq));
    re->top.nalts = 1;
    s = &re->top.alts[0];
    n = seq_add(s);
    n->type = (flags & RE_WHOLE) ? R_BUFBEG : R_NWBEFORE;
    *seq_add(s) = inner;
    n = seq_add(s);
    n->type = (flags & RE_WHOLE) ? R_BUFEND : R_NWAFTER;
  }
  if (!re->has_backref) {
    if (comp_once(re, &re->top) != 0) {
      if (err) *err = xstrdup("regular expression too big");
      regex_free(re);
      return NULL;
    }
    emit(re, I_MATCH);
    re->mark = (int *)xmalloc((size_t)re->nprog * sizeof(int));
    for (k = 0; k < re->nprog; k++) re->mark[k] = 0;
  }
  find_literal(re);
  return re;
}


void regex_free (Regex *re) {
  int i;
  if (re == NULL) return;
  node_free(&re->top);
  for (i = 0; i < re->nsets; i++) free(re->sets[i].ranges);
  free(re->sets);
  free(re->prog);
  free(re->cl);
  free(re->nl);
  free(re->cstore);
  free(re->nstore);
  free(re->scratch);
  free(re->mark);
  free(re);
}


int regex_nsub (const Regex *re) {
  return re->ngroups;
}

/* }================================================================== */


/*
** {==================================================================
** Assertions, shared by both matchers
** ===================================================================
*/

typedef struct MCtx {
  const char *begin, *end;	/* the whole subject */
  int eflags, flags;
} MCtx;


static int assert_ok (const MCtx *m, int kind, const char *p) {
  unsigned prev = p > m->begin ? dec_prev(m->begin, p) : 0, next = 0;
  int len;
  int pw, nw;
  if (p < m->end) next = dec(p, m->end, &len);
  switch (kind) {
    case R_BOL:
      if (p == m->begin) return !(m->eflags & RE_NOTBOL);
      return (m->flags & RE_NEWLINE) && prev == '\n';
    case R_EOL:
      if (p == m->end) return !(m->eflags & RE_NOTEOL);
      return (m->flags & RE_NEWLINE) && next == '\n';
    case R_BUFBEG: return p == m->begin && !(m->eflags & RE_NOTBOL);
    case R_BUFEND: return p == m->end;
  }
  pw = p > m->begin && is_word(prev);
  nw = p < m->end && is_word(next);
  switch (kind) {
    case R_WORDB: return pw != nw;
    case R_NWORDB: return pw == nw;
    case R_WBEG: return !pw && nw;
    case R_WEND: return pw && !nw;
    case R_NWBEFORE: return !pw;	/* grep -w */
    case R_NWAFTER: return !nw;
  }
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** The Pike VM
** ===================================================================
*/

typedef struct VM {
  Regex *re;
  MCtx m;
  int ncap;
  int want_any;	/* only: is there a match? */
  int found;
  size_t *best;
} VM;


static void add_thread (VM *vm, Thread *list, int *n, size_t *store, int pc, size_t *caps,
                        const char *p) {
  Regex *re = vm->re;
  RInst *in;
  if (re->mark[pc] == (int)re->gen) return;
  re->mark[pc] = (int)re->gen;
  in = &re->prog[pc];
  switch (in->op) {
    case I_JMP:
      add_thread(vm, list, n, store, in->x, caps, p);
      return;
    case I_SPLIT:
      add_thread(vm, list, n, store, in->x, caps, p);
      add_thread(vm, list, n, store, in->y, caps, p);
      return;
    case I_SAVE:
      if (in->x < vm->ncap) {
        size_t old = caps[in->x];
        caps[in->x] = (size_t)(p - vm->m.begin);
        add_thread(vm, list, n, store, pc + 1, caps, p);
        caps[in->x] = old;
      }
      else add_thread(vm, list, n, store, pc + 1, caps, p);
      return;
    case I_ASSERT:
      if (assert_ok(&vm->m, in->kind, p)) add_thread(vm, list, n, store, pc + 1, caps, p);
      return;
    case I_MATCH: {
      size_t start = caps[0], end = (size_t)(p - vm->m.begin);
      if (!vm->found || start < vm->best[0] || (start == vm->best[0] && end > vm->best[1])) {
        memcpy(vm->best, caps, (size_t)vm->ncap * sizeof(size_t));
        vm->best[1] = end;
        vm->found = 1;
      }
      return;
    }
    default: {
      Thread *t = &list[*n];
      t->pc = pc;
      t->caps = store + (size_t)*n * (size_t)vm->ncap;
      memcpy(t->caps, caps, (size_t)vm->ncap * sizeof(size_t));
      (*n)++;
    }
  }
}


static int char_ok (Regex *re, const RInst *in, unsigned c) {
  switch (in->op) {
    case I_CHAR:
      return c == in->cp || ((re->flags & RE_ICASE) && fold_lower(c) == fold_lower(in->cp));
    case I_ANY:
      return !((re->flags & RE_NEWLINE) && c == '\n');
    case I_SET:
      return in_set(re, &re->sets[in->x], c);
  }
  return 0;
}


static const char *find_lit (const Regex *re, const char *p, const char *end) {
  size_t n = re->litlen;
  while ((size_t)(end - p) >= n) {
    const char *q = (const char *)memchr(p, re->lit[0], (size_t)(end - p) - n + 1);
    if (q == NULL) return NULL;
    if (memcmp(q, re->lit, n) == 0) return q;
    p = q + 1;
  }
  return NULL;
}


static int vm_run (Regex *re, const char *s, size_t len, size_t start, int eflags,
                   size_t *m) {
  VM vm;
  int cn = 0, nn = 0, k;
  const char *p = s + start, *end = s + len;
  size_t best[2 * 64];
  vm.re = re;
  vm.m.begin = s;
  vm.m.end = end;
  vm.m.eflags = eflags;
  vm.m.flags = re->flags;
  vm.ncap = m ? 2 * (re->ngroups + 1) : 2;
  if (vm.ncap > 2 * 64) vm.ncap = 2 * 64;
  vm.want_any = m == NULL;
  vm.found = 0;
  vm.best = best;
  if (re->ncap_alloc < vm.ncap || re->cl == NULL) {
    free(re->cl);
    free(re->nl);
    free(re->cstore);
    free(re->nstore);
    free(re->scratch);
    re->cl = (Thread *)xmalloc((size_t)re->nprog * sizeof(Thread));
    re->nl = (Thread *)xmalloc((size_t)re->nprog * sizeof(Thread));
    re->cstore = (size_t *)xmalloc((size_t)re->nprog * (size_t)vm.ncap * sizeof(size_t));
    re->nstore = (size_t *)xmalloc((size_t)re->nprog * (size_t)vm.ncap * sizeof(size_t));
    re->scratch = (size_t *)xmalloc((size_t)vm.ncap * sizeof(size_t));
    re->ncap_alloc = vm.ncap;
  }
  if (re->anchored && start > 0 && !(re->flags & RE_NEWLINE)) return 0;
  if (re->anchored && (eflags & RE_NOTBOL)) return 0;
  for (;;) {
    Thread *tl;
    size_t *ts;
    unsigned c = 0;
    int clen = 1;
    /* a new start here, unless a match is already known */
    if (!vm.found && !(re->anchored && p > s + start)) {
      if (cn == 0) {
        if (re->litlen > 0 && !re->anchored) {
          const char *q = find_lit(re, p, end);
          if (q == NULL) break;
          p = q;
        }
      }
      if (++re->gen == 0x7fffffff) {
        re->gen = 1;
        for (k = 0; k < re->nprog; k++) re->mark[k] = 0;
      }
      /* the old threads were added at this position already: mark them */
      for (k = 0; k < cn; k++) re->mark[re->cl[k].pc] = (int)re->gen;
      for (k = 0; k < vm.ncap; k++) re->scratch[k] = (size_t)-1;
      re->scratch[0] = (size_t)(p - s);
      add_thread(&vm, re->cl, &cn, re->cstore, 0, re->scratch, p);
      if (vm.found && vm.want_any) break;
    }
    if (cn == 0 && (vm.found || re->anchored)) break;
    if (p >= end) break;
    if (cn == 0) {	/* nothing alive: the next start */
      dec(p, end, &clen);
      p += clen;
      continue;
    }
    c = dec(p, end, &clen);
    if (++re->gen == 0x7fffffff) {
      re->gen = 1;
      for (k = 0; k < re->nprog; k++) re->mark[k] = 0;
    }
    nn = 0;
    for (k = 0; k < cn; k++) {
      Thread *t = &re->cl[k];
      if (vm.found && t->caps[0] > vm.best[0]) continue;	/* starts later: cannot win */
      if (char_ok(re, &re->prog[t->pc], c))
        add_thread(&vm, re->nl, &nn, re->nstore, t->pc + 1, t->caps, p + clen);
    }
    if (vm.found && vm.want_any) break;
    tl = re->cl; re->cl = re->nl; re->nl = tl;
    ts = re->cstore; re->cstore = re->nstore; re->nstore = ts;
    cn = nn;
    p += clen;
  }
  if (!vm.found) return 0;
  if (m) {
    for (k = 0; k < 2 * (re->ngroups + 1); k++) m[k] = k < vm.ncap ? vm.best[k] : (size_t)-1;
  }
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** The backtracking matcher (back references)
** ===================================================================
*/

enum { K_SEQ, K_GROUP };

typedef struct Cont {
  int kind;
  const RSeq *seq;	/* K_SEQ: go on with seq from i */
  int i;
  const RNode *g;	/* K_GROUP: one iteration of g ended */
  int count;
  const char *start;	/* where that iteration started */
  const struct Cont *up;
} Cont;

typedef struct BState {
  Regex *re;
  MCtx m;
  const char **gs, **ge;	/* group spans */
  long steps;
} BState;

static int bt_seq (BState *b, const RSeq *seq, int i, const char *p, const Cont *k);
static int bt_group (BState *b, const RNode *g, const char *p, int count, const Cont *k);


static int bt_cont (BState *b, const Cont *k, const char *p) {
  const RNode *g;
  const char *os = NULL, *oe = NULL;
  int r;
  if (k == NULL) {	/* the whole thing matched: group 0 ends here */
    b->ge[0] = p;
    return 1;
  }
  if (k->kind == K_SEQ) return bt_seq(b, k->seq, k->i, p, k->up);
  g = k->g;
  if (g->group > 0) {	/* the last iteration is captured */
    os = b->gs[g->group];
    oe = b->ge[g->group];
    b->gs[g->group] = k->start;
    b->ge[g->group] = p;
  }
  if (p == k->start && k->count >= g->min) r = bt_cont(b, k->up, p);	/* no empty loops */
  else r = bt_group(b, g, p, k->count, k->up);
  if (!r && g->group > 0) {
    b->gs[g->group] = os;
    b->ge[g->group] = oe;
  }
  return r;
}


static int bt_group (BState *b, const RNode *g, const char *p, int count, const Cont *k) {
  if (++b->steps > 20000000) return 0;
  if (g->max < 0 || count < g->max) {	/* greedy: one more iteration first */
    Cont gk;
    int a;
    gk.kind = K_GROUP;
    gk.seq = NULL;
    gk.i = 0;
    gk.g = g;
    gk.count = count + 1;
    gk.start = p;
    gk.up = k;
    for (a = 0; a < g->nalts; a++)
      if (bt_seq(b, &g->alts[a], 0, p, &gk)) return 1;
  }
  if (count >= g->min) return bt_cont(b, k, p);
  return 0;
}


/* one occurrence of a simple node at p: its length, or 0 */
static int bt_single (BState *b, const RNode *n, const char *p) {
  unsigned c;
  int len;
  if (p >= b->m.end) return 0;
  c = dec(p, b->m.end, &len);
  switch (n->type) {
    case R_CHAR:
      return (c == n->cp || ((b->re->flags & RE_ICASE) && fold_lower(c) == fold_lower(n->cp))) ? len : 0;
    case R_ANY:
      return ((b->re->flags & RE_NEWLINE) && c == '\n') ? 0 : len;
    case R_SET:
      return in_set(b->re, &b->re->sets[n->set], c) ? len : 0;
  }
  return 0;
}


static int bt_seq (BState *b, const RSeq *seq, int i, const char *p, const Cont *k) {
  const RNode *n;
  Cont next;
  if (++b->steps > 20000000) return 0;
  if (i == seq->n) return bt_cont(b, k, p);
  n = &seq->v[i];
  switch (n->type) {
    case R_BOL: case R_EOL: case R_WORDB: case R_NWORDB: case R_WBEG: case R_WEND:
    case R_BUFBEG: case R_BUFEND: case R_NWBEFORE: case R_NWAFTER:
      return assert_ok(&b->m, n->type, p) && bt_seq(b, seq, i + 1, p, k);
    case R_BACKREF: {
      const char *gs = b->gs[n->group], *ge = b->ge[n->group];
      size_t len;
      int count = 0;
      const char *q = p;
      if (gs == NULL || ge == NULL) return 0;
      len = (size_t)(ge - gs);
      /* \1* and the like: greedy over whole copies */
      for (;;) {
        if (n->max >= 0 && count >= n->max) break;
        if ((size_t)(b->m.end - q) < len) break;
        if ((b->re->flags & RE_ICASE) ? m_strnicmp(q, gs, len) != 0 : memcmp(q, gs, len) != 0) break;
        q += len;
        count++;
        if (len == 0) break;
      }
      for (;; count--, q -= len) {
        if (count < n->min) return 0;
        if (bt_seq(b, seq, i + 1, q, k)) return 1;
        if (count == 0 || len == 0) return 0;
      }
    }
    case R_GROUP:
      next.kind = K_SEQ;
      next.seq = seq;
      next.i = i + 1;
      next.g = NULL;
      next.count = 0;
      next.start = NULL;
      next.up = k;
      return bt_group(b, n, p, 0, &next);
    default: {	/* a simple node, maybe repeated: greedy, then give back */
      const char *small[64];
      const char **pos = small;
      int count = 0, cap = 64, len;
      const char *q = p;
      int r = 0;
      pos[0] = q;
      while ((n->max < 0 || count < n->max) && (len = bt_single(b, n, q)) > 0) {
        q += len;
        count++;
        if (count >= cap) {
          const char **np = (const char **)xmalloc((size_t)cap * 2 * sizeof(char *));
          memcpy(np, pos, (size_t)cap * sizeof(char *));
          if (pos != small) free(pos);
          pos = np;
          cap *= 2;
        }
        pos[count] = q;
      }
      for (; count >= n->min; count--) {
        if (bt_seq(b, seq, i + 1, pos[count], k)) {
          r = 1;
          break;
        }
      }
      if (pos != small) free(pos);
      return r;
    }
  }
}


static int bt_run (Regex *re, const char *s, size_t len, size_t start, int eflags, size_t *m) {
  BState b;
  const char *p = s + start, *end = s + len;
  const char **spans;
  int ng = re->ngroups + 1, k;
  spans = (const char **)xmalloc((size_t)ng * 2 * sizeof(char *));
  b.re = re;
  b.m.begin = s;
  b.m.end = end;
  b.m.eflags = eflags;
  b.m.flags = re->flags;
  b.gs = spans;
  b.ge = spans + ng;
  b.steps = 0;
  for (;; p++) {
    for (k = 0; k < ng; k++) b.gs[k] = b.ge[k] = NULL;
    if (re->anchored && p > s + start) break;
    b.gs[0] = p;
    if (bt_group(&b, &re->top, p, 0, NULL)) {
      if (m) {
        for (k = 0; k < ng; k++) {
          m[2 * k] = b.gs[k] ? (size_t)(b.gs[k] - s) : (size_t)-1;
          m[2 * k + 1] = b.ge[k] ? (size_t)(b.ge[k] - s) : (size_t)-1;
        }
        m[0] = (size_t)(p - s);
      }
      free(spans);
      return 1;
    }
    if (p >= end || b.steps > 20000000) break;
    /* the next start: a whole character on */
    {
      int l;
      dec(p, end, &l);
      p += l - 1;
    }
  }
  free(spans);
  return 0;
}

/* }================================================================== */


int regex_match (Regex *re, const char *s, size_t len, size_t start, int eflags, size_t *m) {
  if (start > len) return 0;
  if (re->has_backref) return bt_run(re, s, len, start, eflags, m);
  return vm_run(re, s, len, start, eflags, m);
}


/* the old interface, for [[ =~ ]]: ERE, the groups as strings */
Regex *regex_compile (const char *pat, int icase, char **err) {
  return regex_new(pat, RE_EXTENDED | (icase ? RE_ICASE : 0), err);
}


int regex_exec (Regex *re, const char *s, Vec *groups) {
  size_t n = (size_t)(re->ngroups + 1), *m = (size_t *)xmalloc(2 * n * sizeof(size_t)), g;
  size_t len = strlen(s);
  if (!regex_match(re, s, len, 0, 0, m)) {
    free(m);
    return 0;
  }
  if (groups != NULL) {
    for (g = 0; g < n; g++) {
      if (m[2 * g] != (size_t)-1 && m[2 * g + 1] != (size_t)-1)
        vec_push(groups, xstrndup(s + m[2 * g], m[2 * g + 1] - m[2 * g]));
      else vec_push(groups, xstrdup(""));
    }
  }
  free(m);
  return 1;
}
