/*
** mregex.c - POSIX extended regular expressions, for [[ str =~ re ]]
**
** A small backtracking matcher: . [] [^] [[:class:]] * + ? {m,n} | ( )
** ^ $ and back references \1..\9, with capture groups for BASH_REMATCH.
** Like bash, the match is unanchored and the leftmost, longest-first
** (greedy) one is taken.
*/

#include "mmc.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>


enum { R_CHAR, R_ANY, R_SET, R_BOL, R_EOL, R_GROUP, R_BACKREF, R_ALT, R_WORDB, R_NWORDB };

typedef struct RNode {
  int type;
  int min, max;	/* repetition; max -1 = no limit */
  int greedy;
  unsigned char ch;
  unsigned char set[32];	/* R_SET: a bit per byte */
  int group;	/* R_GROUP: its number; R_BACKREF: which */
  struct RSeq *alts;	/* R_GROUP / R_ALT: alternatives */
  int nalts;
} RNode;

typedef struct RSeq {
  RNode *v;
  int n, cap;
} RSeq;

struct Regex {
  RSeq top;	/* alternatives of the whole expression, as one group 0 */
  int ngroups;
  int icase;
};

typedef struct RParse {
  const char *p;
  int ngroups;
  int icase;
  char *err;
} RParse;


static RNode *seq_add (RSeq *s) {
  if (s->n + 1 > s->cap) {
    s->cap = s->cap ? s->cap * 2 : 8;
    s->v = (RNode *)xrealloc(s->v, (size_t)s->cap * sizeof(RNode));
  }
  memset(&s->v[s->n], 0, sizeof(RNode));
  s->v[s->n].min = s->v[s->n].max = 1;
  s->v[s->n].greedy = 1;
  return &s->v[s->n++];
}


static void seq_free (RSeq *s);

static void node_free (RNode *n) {
  int i;
  for (i = 0; i < n->nalts; i++) seq_free(&n->alts[i]);
  free(n->alts);
}


static void seq_free (RSeq *s) {
  int i;
  for (i = 0; i < s->n; i++) node_free(&s->v[i]);
  free(s->v);
  s->v = NULL;
  s->n = s->cap = 0;
}


static void set_bit (unsigned char *set, int c) {
  set[(unsigned char)c >> 3] |= (unsigned char)(1 << (c & 7));
}


static int in_set (const unsigned char *set, int c) {
  return (set[(unsigned char)c >> 3] >> (c & 7)) & 1;
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


/* [...] at rp->p (after the '['); fills the set */
static int parse_set (RParse *rp, unsigned char *set) {
  const char *p = rp->p;
  int neg = 0, first = 1, c;
  memset(set, 0, 32);
  if (*p == '^') {
    neg = 1;
    p++;
  }
  while (*p != '\0' && (first || *p != ']')) {
    int lo, hi;
    first = 0;
    if (p[0] == '[' && p[1] == ':') {
      const char *e = strstr(p + 2, ":]");
      char name[16];
      size_t n;
      if (e == NULL || (n = (size_t)(e - p - 2)) >= sizeof(name)) {
        rp->err = xstrdup("bad character class");
        return -1;
      }
      memcpy(name, p + 2, n);
      name[n] = '\0';
      if (class_match(name, 'a') < 0) {
        rp->err = xstrdup("bad character class");
        return -1;
      }
      for (c = 1; c < 256; c++)
        if (class_match(name, c)) set_bit(set, c);
      p = e + 2;
      continue;
    }
    if (p[0] == '[' && (p[1] == '.' || p[1] == '=')) {	/* [.x.] [=x=] */
      char end = p[1];
      lo = (unsigned char)p[2];
      p += 3;
      while (*p && !(p[0] == end && p[1] == ']')) p++;
      if (*p) p += 2;
      set_bit(set, lo);
      continue;
    }
    lo = (unsigned char)*p++;
    hi = lo;
    if (p[0] == '-' && p[1] != ']' && p[1] != '\0') {
      hi = (unsigned char)p[1];
      p += 2;
    }
    for (c = lo; c <= hi; c++) {
      set_bit(set, c);
      if (rp->icase && isalpha(c)) {
        set_bit(set, tolower(c));
        set_bit(set, toupper(c));
      }
    }
  }
  if (*p != ']') {
    rp->err = xstrdup("unmatched [");
    return -1;
  }
  rp->p = p + 1;
  if (neg) {
    for (c = 0; c < 32; c++) set[c] = (unsigned char)~set[c];
    set[0] &= (unsigned char)~1;	/* never NUL */
  }
  return 0;
}


static int parse_alts (RParse *rp, RNode *group);


static int parse_seq (RParse *rp, RSeq *s) {
  while (*rp->p != '\0' && *rp->p != '|' && *rp->p != ')') {
    RNode *n = seq_add(s);
    char c = *rp->p++;
    switch (c) {
      case '.': n->type = R_ANY; break;
      case '^': n->type = R_BOL; break;
      case '$': n->type = R_EOL; break;
      case '[':
        n->type = R_SET;
        if (parse_set(rp, n->set) != 0) return -1;
        break;
      case '(':
        n->type = R_GROUP;
        n->group = ++rp->ngroups;
        if (parse_alts(rp, n) != 0) return -1;
        if (*rp->p != ')') {
          rp->err = xstrdup("unmatched (");
          return -1;
        }
        rp->p++;
        break;
      case '\\':
        c = *rp->p;
        if (c == '\0') {
          rp->err = xstrdup("trailing backslash");
          return -1;
        }
        rp->p++;
        if (c >= '1' && c <= '9') {
          n->type = R_BACKREF;
          n->group = c - '0';
        }
        else if (c == 'b') n->type = R_WORDB;
        else if (c == 'B') n->type = R_NWORDB;
        else if (c == 'w' || c == 'W' || c == 's' || c == 'S' || c == 'd' || c == 'D') {
          int k;
          n->type = R_SET;
          memset(n->set, 0, 32);
          for (k = 1; k < 256; k++) {
            int in = (c == 'w' || c == 'W') ? (isalnum(k) || k == '_')
                   : (c == 's' || c == 'S') ? isspace(k) : isdigit(k);
            if (isupper((unsigned char)c)) in = !in;
            if (in) set_bit(n->set, k);
          }
        }
        else {
          n->type = R_CHAR;
          n->ch = (unsigned char)(c == 'n' ? '\n' : c == 't' ? '\t' : c);
        }
        break;
      case '*': case '+': case '?':	/* nothing to repeat: a literal, like glibc */
        n->type = R_CHAR;
        n->ch = (unsigned char)c;
        break;
      default:
        n->type = R_CHAR;
        n->ch = (unsigned char)c;
        break;
    }
    /* repetition */
    for (;;) {
      char r = *rp->p;
      int min, max;
      if (r == '*') { min = 0; max = -1; rp->p++; }
      else if (r == '+') { min = 1; max = -1; rp->p++; }
      else if (r == '?') { min = 0; max = 1; rp->p++; }
      else if (r == '{' && isdigit((unsigned char)rp->p[1])) {
        const char *q = rp->p + 1;
        min = atoi(q);
        while (isdigit((unsigned char)*q)) q++;
        max = min;
        if (*q == ',') {
          q++;
          if (isdigit((unsigned char)*q)) {
            max = atoi(q);
            while (isdigit((unsigned char)*q)) q++;
          }
          else max = -1;
        }
        if (*q != '}') break;	/* a literal '{' */
        rp->p = q + 1;
      }
      else break;
      if (n->min != 1 || n->max != 1) {	/* x** : wrap in a group */
        RNode inner = *n;
        memset(n, 0, sizeof(*n));
        n->type = R_GROUP;
        n->group = -1;	/* not a capture group */
        n->alts = (RSeq *)xmalloc(sizeof(RSeq));
        memset(n->alts, 0, sizeof(RSeq));
        n->nalts = 1;
        *seq_add(&n->alts[0]) = inner;
      }
      n->min = min;
      n->max = max;
      n->greedy = 1;
      if (*rp->p == '?') rp->p++;	/* lazy marks are accepted, ignored */
    }
  }
  return 0;
}


static int parse_alts (RParse *rp, RNode *group) {
  for (;;) {
    group->alts = (RSeq *)xrealloc(group->alts, (size_t)(group->nalts + 1) * sizeof(RSeq));
    memset(&group->alts[group->nalts], 0, sizeof(RSeq));
    if (parse_seq(rp, &group->alts[group->nalts++]) != 0) return -1;
    if (*rp->p != '|') return 0;
    rp->p++;
  }
}


Regex *regex_compile (const char *pat, int icase, char **err) {
  Regex *re = (Regex *)xmalloc(sizeof(Regex));
  RParse rp;
  RNode *g;
  memset(re, 0, sizeof(*re));
  rp.p = pat;
  rp.ngroups = 0;
  rp.icase = icase;
  rp.err = NULL;
  g = seq_add(&re->top);
  g->type = R_GROUP;
  g->group = 0;
  if (parse_alts(&rp, g) != 0 || *rp.p != '\0') {
    if (rp.err == NULL) rp.err = xstrdup("unmatched )");
    if (err) *err = rp.err;
    else free(rp.err);
    regex_free(re);
    return NULL;
  }
  re->ngroups = rp.ngroups;
  re->icase = icase;
  return re;
}


void regex_free (Regex *re) {
  if (re == NULL) return;
  seq_free(&re->top);
  free(re);
}


/*
** {==================================================================
** Matching: backtracking; 'Cont' is what has to match after this node
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

typedef struct MState {
  const char *begin;	/* the whole subject, for ^ and \b */
  const char *gs[10], *ge[10];	/* group spans */
  int icase;
  long steps;
} MState;

static int match_seq (MState *m, const RSeq *seq, int i, const char *p,
                      const Cont *k);
static int group_iter (MState *m, const RNode *g, const char *p, int count,
                       const Cont *k);


static int run_cont (MState *m, const Cont *k, const char *p) {
  const RNode *g;
  const char *os = NULL, *oe = NULL;
  int r;
  if (k == NULL) return 1;
  if (k->kind == K_SEQ) return match_seq(m, k->seq, k->i, p, k->up);
  g = k->g;
  if (g->group >= 0 && g->group < 10) {	/* the last iteration is captured */
    os = m->gs[g->group];
    oe = m->ge[g->group];
    m->gs[g->group] = k->start;
    m->ge[g->group] = p;
  }
  if (p == k->start && k->count >= g->min) r = run_cont(m, k->up, p);	/* no empty loops */
  else r = group_iter(m, g, p, k->count, k->up);
  if (!r && g->group >= 0 && g->group < 10) {
    m->gs[g->group] = os;
    m->ge[g->group] = oe;
  }
  return r;
}


static int group_iter (MState *m, const RNode *g, const char *p, int count,
                       const Cont *k) {
  if (++m->steps > 5000000) return 0;
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
      if (match_seq(m, &g->alts[a], 0, p, &gk)) return 1;
  }
  if (count >= g->min) return run_cont(m, k, p);
  return 0;
}


static int eq_ch (const MState *m, int a, int b) {
  return a == b || (m->icase && tolower(a) == tolower(b));
}


static int is_word_ch (int c) {
  return isalnum(c) || c == '_';
}


/* one occurrence of a simple node at p: 1 if it matches */
static int single (const MState *m, const RNode *n, const char *p) {
  int c = (unsigned char)*p;
  if (c == 0) return 0;
  switch (n->type) {
    case R_CHAR: return eq_ch(m, c, n->ch);
    case R_ANY: return 1;
    case R_SET:
      return in_set(n->set, c) ||
             (m->icase && (in_set(n->set, tolower(c)) || in_set(n->set, toupper(c))));
  }
  return 0;
}


static int match_seq (MState *m, const RSeq *seq, int i, const char *p,
                      const Cont *k) {
  const RNode *n;
  Cont next;
  if (++m->steps > 5000000) return 0;
  if (i == seq->n) return run_cont(m, k, p);
  n = &seq->v[i];
  switch (n->type) {
    case R_BOL:
      return p == m->begin && match_seq(m, seq, i + 1, p, k);
    case R_EOL:
      return *p == '\0' && match_seq(m, seq, i + 1, p, k);
    case R_WORDB:
    case R_NWORDB: {
      int before = p > m->begin && is_word_ch((unsigned char)p[-1]);
      int after = is_word_ch((unsigned char)*p);
      if ((n->type == R_WORDB) != (before != after)) return 0;
      return match_seq(m, seq, i + 1, p, k);
    }
    case R_BACKREF: {
      const char *gs = m->gs[n->group], *ge = m->ge[n->group];
      size_t len;
      if (gs == NULL || ge == NULL) return 0;
      len = (size_t)(ge - gs);
      if (m->icase ? m_strnicmp(p, gs, len) != 0 : strncmp(p, gs, len) != 0) return 0;
      return match_seq(m, seq, i + 1, p + len, k);
    }
    case R_GROUP:
      next.kind = K_SEQ;
      next.seq = seq;
      next.i = i + 1;
      next.g = NULL;
      next.count = 0;
      next.start = NULL;
      next.up = k;
      return group_iter(m, n, p, 0, &next);
    default: {	/* a simple node, maybe repeated: greedy, then give back */
      const char *q = p;
      int count = 0;
      while ((n->max < 0 || count < n->max) && single(m, n, q)) {
        q++;
        count++;
      }
      if (count < n->min) return 0;
      for (;; count--, q--) {
        if (match_seq(m, seq, i + 1, q, k)) return 1;
        if (count == n->min) return 0;
      }
    }
  }
}


int regex_exec (Regex *re, const char *s, Vec *groups) {
  MState m;
  const char *start;
  int g;
  memset(&m, 0, sizeof(m));
  m.begin = s;
  m.icase = re->icase;
  for (start = s;; start++) {
    int k;
    for (k = 0; k < 10; k++) m.gs[k] = m.ge[k] = NULL;
    if (match_seq(&m, &re->top, 0, start, NULL)) break;
    if (*start == '\0' || m.steps > 5000000) return 0;
  }
  if (groups != NULL) {
    for (g = 0; g <= re->ngroups && g < 10; g++) {
      if (m.gs[g] != NULL && m.ge[g] != NULL)
        vec_push(groups, xstrndup(m.gs[g], (size_t)(m.ge[g] - m.gs[g])));
      else vec_push(groups, xstrdup(""));
    }
  }
  return 1;
}

/* }================================================================== */
