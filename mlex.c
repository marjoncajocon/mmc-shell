/*
** mlex.c - tokenizer
**
** Splits one command line into words and operators. Words keep their
** quotes; mexpand.c removes them later.
*/

#include "mmc.h"

#include <stdlib.h>
#include <string.h>


void tok_init (TokVec *t) {
  t->v = NULL;
  t->n = t->cap = 0;
}


void tok_push (TokVec *t, int type, char *text, int depth) {
  if (t->n + 1 > t->cap) {
    t->cap = t->cap ? t->cap * 2 : 16;
    t->v = (Token *)xrealloc(t->v, t->cap * sizeof(Token));
  }
  t->v[t->n].type = type;
  t->v[t->n].text = text;
  t->v[t->n].depth = depth;
  t->n++;
}


void tok_free (TokVec *t) {
  size_t i;
  for (i = 0; i < t->n; i++) free(t->v[i].text);
  free(t->v);
  tok_init(t);
}


static int is_space (int c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}


static int is_meta (int c) {
  return c == '|' || c == '&' || c == ';' || c == '<' || c == '>';
}


/* operator at 's'? returns its length and type */
static size_t lex_operator (const char *s, int *type) {
  static const struct { const char *text; int type; } ops[] = {
    {"2>&1", T_ERROUT}, {"1>&2", T_OUTERR}, {"2>>", T_ERRAPP},
    {">&2", T_OUTERR}, {"&>", T_BOTH}, {"&&", T_AND}, {"||", T_OR},
    {">>", T_GTGT}, {"2>", T_ERR}, {"1>", T_GT},
    {"|", T_PIPE}, {"&", T_BG}, {";", T_SEMI}, {"<", T_LT}, {">", T_GT}
  };
  size_t i;
  for (i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
    size_t n = strlen(ops[i].text);
    if (strncmp(s, ops[i].text, n) == 0) {
      *type = ops[i].type;
      return n;
    }
  }
  return 0;
}


/* returns 0, or -1 after printing a syntax error */
int lex_line (const char *s, TokVec *out) {
  size_t i = 0;
  for (;;) {
    size_t start, n;
    int type;
    while (is_space((unsigned char)s[i])) i++;
    if (s[i] == '\0' || s[i] == '#') return 0;
    if ((n = lex_operator(s + i, &type)) > 0) {
      tok_push(out, type, NULL, 0);
      i += n;
      continue;
    }
    start = i;
    while (s[i] != '\0' && !is_space((unsigned char)s[i]) && !is_meta(s[i])) {
      if (s[i] == '\\' && s[i + 1] != '\0') i += 2;
      else if (s[i] == '\'' || s[i] == '"') {
        char q = s[i++];
        while (s[i] != '\0' && s[i] != q) {
          if (q == '"' && s[i] == '\\' && s[i + 1] != '\0') i++;
          i++;
        }
        if (s[i] == '\0') {
          fd_printf(2, "mmc: syntax error: missing closing %c\n", q);
          return -1;
        }
        i++;
      }
      else i++;
    }
    tok_push(out, T_WORD, xstrndup(s + start, i - start), 0);
  }
}
