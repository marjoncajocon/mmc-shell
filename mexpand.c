/*
** mexpand.c - word expansion
**
** A raw word goes through, in bash's order:
**   brace expansion  {a,b} {1..5}
**   tilde            ~ ~/x ~+ ~-
**   parameters       $x ${x} ${x:-w} ${#x} ${x#p} ${x/p/s} ${a[@]} ...
**   commands         $( ) ` `      arithmetic $(( ))     <( ) >( )
**   field splitting  of unquoted results, on $IFS
**   globbing         * ? [ ] and extglob
**   quote removal
** Characters that came from quotes must not glob, so they are written
** with a QMARK byte in front ("marked") until the very end.
*/

#include "mmc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct Exp {
  Vec fields;	/* finished fields, marked */
  Buf cur;	/* the field being built, marked */
  int live;	/* the current field exists (even if empty: "") */
  int split;	/* unquoted results are split into fields */
  int assign;	/* a=value: tilde after '=' and ':' */
  int pattern;	/* keep marks; for case, [[ ==, ${x#p} */
  int in_dq;	/* inside "..." */
  int at_empty;	/* "$@" with no parameters just happened */
  int split_text;	/* the operand of ${x:-a b}: its own text splits too */
} Exp;

static int g_failed = 0;

static char *expand_to_str (const char *raw, size_t n, int in_dq, int pattern);
static void walk (Exp *e, const char *raw, size_t n);


int expand_failed (void) {
  int f = g_failed;
  g_failed = 0;
  return f;
}


static void fail (void) {
  g_failed = 1;
}


/*
** {==================================================================
** Building fields
** ===================================================================
*/

static int special_char (int c) {
  return c == '*' || c == '?' || c == '[' || c == ']' || c == QMARK ||
         c == '(' || c == ')' || c == '|' || c == '!' || c == '@' ||
         c == '+' || c == '\\' || c == '^' || c == '-' || c == '.' || c == '$' ||
         c == '{' || c == '}' || c == '&';
}


/* a quoted character: never a pattern character */
static void put_lit (Exp *e, char c) {
  if (special_char((unsigned char)c)) buf_putc(&e->cur, QMARK);
  buf_putc(&e->cur, c);
  e->live = 1;
}


static void put_lits (Exp *e, const char *s, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) put_lit(e, s[i]);
  e->live = 1;
}


/* unquoted text of the word itself */
static void put_raw (Exp *e, char c) {
  if (c == QMARK) buf_putc(&e->cur, QMARK);
  buf_putc(&e->cur, c);
  e->live = 1;
}


static void field_end (Exp *e) {
  if (e->live) vec_push(&e->fields, buf_take(&e->cur));
  else buf_free(&e->cur);
  e->live = 0;
}


static const char *ifs_value (void) {
  const char *ifs = var_get("IFS");
  return ifs ? ifs : " \t\n";
}


static int is_ifs_ws (int c, const char *ifs) {
  return (c == ' ' || c == '\t' || c == '\n') && strchr(ifs, c) != NULL;
}


/* the result of an unquoted expansion: split on IFS, globbing stays on */
static void put_split (Exp *e, const char *s) {
  const char *ifs;
  if (e->in_dq) {
    put_lits(e, s, strlen(s));
    return;
  }
  if (!e->split) {
    for (; *s; s++) {
      if (e->pattern && *s == '\\' && s[1] != '\0') {	/* \x in a value: literal */
        put_lit(e, *++s);
        continue;
      }
      put_raw(e, *s);
    }
    return;
  }
  ifs = ifs_value();
  if (*ifs == '\0') {
    for (; *s; s++) put_raw(e, *s);
    return;
  }
  {
    int ws_ended = 0;	/* blanks just ended a field: " , " is one separator */
    while (*s) {
      int c = (unsigned char)*s;
      if (is_ifs_ws(c, ifs)) {
        if (e->live) {
          field_end(e);
          ws_ended = 1;
        }
        while (*s && is_ifs_ws((unsigned char)*s, ifs)) s++;
        continue;
      }
      if (strchr(ifs, c) != NULL) {	/* a non-blank separator: may make "" */
        if (e->live) field_end(e);
        else if (!ws_ended) {
          e->live = 1;
          field_end(e);
        }
        ws_ended = 0;
        s++;
        while (*s && is_ifs_ws((unsigned char)*s, ifs)) s++;
        continue;
      }
      ws_ended = 0;
      put_raw(e, *s++);
    }
  }
}

/* }================================================================== */


/*
** {==================================================================
** Tilde
** ===================================================================
*/

/* ~ at raw[i]: returns bytes used (0: not a tilde expansion) */
static size_t expand_tilde (Exp *e, const char *raw, size_t i) {
  size_t j = i + 1;
  char *user;
  const char *val = NULL;
  char *made = NULL;
  while (raw[j] && raw[j] != '/' && !(e->assign && raw[j] == ':') &&
         (isalnum((unsigned char)raw[j]) || strchr("_.-+", raw[j]) != NULL))
    j++;
  if (raw[j] != '\0' && raw[j] != '/' && !(e->assign && raw[j] == ':')) return 0;
  user = xstrndup(raw + i + 1, j - i - 1);
  if (user[0] == '\0') val = var_get("HOME");
  else if (strcmp(user, "+") == 0) val = var_get("PWD");
  else if (strcmp(user, "-") == 0) val = var_get("OLDPWD");
  else {
    const char *home = var_get("HOME");
    if (home != NULL) {	/* the homes live side by side */
      char *native = path_to_native(home);
      char *parent = path_dirname(native);
      char *theirs = path_join(parent, user);
      OsStat st;
      if (os_stat(theirs, &st) == 0 && st.is_dir) made = path_to_display(theirs);
      free(native);
      free(parent);
      free(theirs);
    }
    val = made;
  }
  free(user);
  if (val == NULL) return 0;
  put_lits(e, val, strlen(val));
  free(made);
  return j - i;
}

/* }================================================================== */


/*
** {==================================================================
** Parameters
** ===================================================================
*/

/* the list behind $@, ${a[@]}, ${!a[@]}, ${!pre@} */
typedef struct List {
  Vec v;
  int is_list;	/* [@] / [*] / $@ / $* */
  int star;	/* [*] / $*: joined in quotes */
  int unset;	/* for :- and friends */
} List;


static void list_init (List *l) {
  vec_init(&l->v);
  l->is_list = l->star = l->unset = 0;
}


static char *join_with (const Vec *v, int star) {
  const char *ifs = var_get("IFS");
  char sep = !star ? ' ' : (ifs == NULL) ? ' ' : ifs[0];
  Buf b;
  size_t i;
  buf_init(&b);
  for (i = 0; i < v->n; i++) {
    if (i > 0 && sep != '\0') buf_putc(&b, sep);
    buf_puts(&b, v->v[i]);
  }
  return buf_take(&b);
}


static void positional (List *l) {
  size_t i;
  for (i = 1; i < sh_pos.n; i++) vec_push(&l->v, xstrdup(sh_pos.v[i]));
}


/* the subscript of name[sub]: arithmetic for indexed arrays */
static int subscript (const char *name, const char *sub, long long *idx,
                      char **key) {
  int flags = var_flags(name);
  char *exp = expand_to_str(sub, strlen(sub), 1, 0);
  *key = NULL;
  if (flags >= 0 && (flags & V_ASSOC)) {
    *key = exp;
    return 0;
  }
  if (arith_eval(exp, idx) != 0) {
    free(exp);
    fail();
    return -1;
  }
  free(exp);
  return 0;
}


/* looks up a parameter: name, digits, special character, name[sub] */
static void lookup (const char *name, List *l) {
  char num[32];
  const char *br = strchr(name, '[');
  if (br != NULL && name[strlen(name) - 1] == ']') {
    char *base = xstrndup(name, (size_t)(br - name));
    char *sub = xstrndup(br + 1, strlen(br + 1) - 1);
    if (strcmp(sub, "@") == 0 || strcmp(sub, "*") == 0) {
      l->is_list = 1;
      l->star = sub[0] == '*';
      var_values(base, &l->v);
      if (l->v.n == 0 && var_flags(base) < 0) l->unset = 1;
    }
    else {
      long long idx = 0;
      char *key = NULL;
      if (subscript(base, sub, &idx, &key) == 0) {
        const char *v = key ? var_akget(base, key) : var_aget(base, idx);
        if (v != NULL) vec_push(&l->v, xstrdup(v));
        else l->unset = 1;
      }
      else l->unset = 1;
      free(key);
    }
    free(base);
    free(sub);
    return;
  }
  if (strcmp(name, "@") == 0 || strcmp(name, "*") == 0) {
    l->is_list = 1;
    l->star = name[0] == '*';
    positional(l);
    return;
  }
  if (strcmp(name, "#") == 0) {
    vec_push(&l->v, xstrdup(ll_to_str((long long)(sh_pos.n > 0 ? sh_pos.n - 1 : 0), num)));
    return;
  }
  if (strcmp(name, "?") == 0) {
    vec_push(&l->v, xstrdup(ll_to_str(sh_status, num)));
    return;
  }
  if (strcmp(name, "$") == 0) {
    vec_push(&l->v, xstrdup(ll_to_str(sh_pid, num)));
    return;
  }
  if (strcmp(name, "!") == 0) {
    if (sh_last_bg != 0) vec_push(&l->v, xstrdup(ll_to_str(sh_last_bg, num)));
    else l->unset = 1;
    return;
  }
  if (strcmp(name, "-") == 0) {
    vec_push(&l->v, opt_flags());
    return;
  }
  if (isdigit((unsigned char)name[0])) {
    long long k = 0;
    str_to_ll(name, &k);
    if (k >= 0 && (size_t)k < sh_pos.n) vec_push(&l->v, xstrdup(sh_pos.v[k]));
    else if (k == 0) vec_push(&l->v, xstrdup(MMC_NAME));
    else l->unset = 1;
    return;
  }
  {
    const char *v = var_get(name);
    if (v != NULL) vec_push(&l->v, xstrdup(v));
    else l->unset = 1;
  }
}


static void unbound (const char *name) {
  sh_error("%s: unbound variable", name);
  fail();
}


/* the name at s: a name, digits, or one special character; returns length */
static size_t param_name_len (const char *s) {
  size_t i = 0;
  if (isalpha((unsigned char)s[0]) || s[0] == '_') {
    while (isalnum((unsigned char)s[i]) || s[i] == '_') i++;
    return i;
  }
  if (isdigit((unsigned char)s[0])) {
    while (isdigit((unsigned char)s[i])) i++;
    return i;
  }
  if (s[0] && strchr("@*#?$!-", s[0]) != NULL) return 1;
  return 0;
}


/* removes the shortest/longest match of pat from the front/back of s */
static char *trim (const char *s, const char *pat, int back, int longest) {
  size_t n = strlen(s), k;
  int flags = PM_EXTGLOB;
  if (!back) {
    if (longest) {
      for (k = n + 1; k-- > 0;)
        if (pat_match_len(pat, s, k, flags)) return xstrdup(s + k);
    }
    else {
      for (k = 0; k <= n; k++)
        if (pat_match_len(pat, s, k, flags)) return xstrdup(s + k);
    }
  }
  else {
    if (longest) {
      for (k = 0; k <= n; k++)
        if (pat_match(pat, s + k, flags)) return xstrndup(s, k);
    }
    else {
      for (k = n + 1; k-- > 0;)
        if (pat_match(pat, s + k, flags)) return xstrndup(s, k);
    }
  }
  return xstrdup(s);
}


/* the replacement: rep is marked; a plain & is what matched (bash 5.2) */
static void put_rep (Buf *b, const char *rep, const char *match, size_t mlen) {
  int amp = O("patsub_replacement");
  for (; *rep; rep++) {
    if (*rep == QMARK && rep[1] != '\0') buf_putc(b, *++rep);
    else if (*rep == '&' && amp) buf_putn(b, match, mlen);
    else buf_putc(b, *rep);
  }
}


/* ${x/pat/rep}: mode 0 first, 1 all, 2 at the start, 3 at the end */
static char *replace (const char *s, const char *pat, const char *rep, int mode) {
  Buf b;
  size_t n = strlen(s), i = 0, k;
  int flags = PM_EXTGLOB;
  if (*pat == '\0' && mode < 2) return xstrdup(s);
  buf_init(&b);
  if (mode == 2) {
    for (k = n + 1; k-- > 0;)
      if (pat_match_len(pat, s, k, flags)) {
        put_rep(&b, rep, s, k);
        buf_puts(&b, s + k);
        return buf_take(&b);
      }
    return xstrdup(s);
  }
  if (mode == 3) {
    for (k = 0; k <= n; k++)
      if (pat_match(pat, s + k, flags)) {
        buf_putn(&b, s, k);
        put_rep(&b, rep, s + k, n - k);
        return buf_take(&b);
      }
    return xstrdup(s);
  }
  while (i < n) {
    size_t best = 0;
    int found = 0;
    for (k = n - i + 1; k-- > 1;) {	/* the longest non-empty match at i */
      if (pat_match_len(pat, s + i, k, flags)) {
        best = k;
        found = 1;
        break;
      }
    }
    if (found) {
      put_rep(&b, rep, s + i, best);
      i += best;
      if (mode == 0) {
        buf_puts(&b, s + i);
        return buf_take(&b);
      }
      continue;
    }
    buf_putc(&b, s[i]);
    i++;
  }
  return buf_take(&b);
}


static char *change_case (const char *s, const char *pat, int upper, int all,
                          int toggle) {
  char *r = xstrdup(s);
  char *p;
  for (p = r; *p; p++) {
    if (pat != NULL && *pat != '\0') {
      char one[2];
      one[0] = *p;
      one[1] = '\0';
      if (!pat_match(pat, one, PM_EXTGLOB)) {
        if (!all) break;
        continue;
      }
    }
    if (toggle) *p = (char)(isupper((unsigned char)*p) ? tolower((unsigned char)*p)
                                                        : toupper((unsigned char)*p));
    else *p = (char)(upper ? toupper((unsigned char)*p) : tolower((unsigned char)*p));
    if (!all) break;
  }
  return r;
}


/* ${x@op} */
static char *transform (const char *name, const char *s, char op) {
  switch (op) {
    case 'Q': return shell_quote(s);
    case 'E': return expand_ansi_c(s, strlen(s), NULL);
    case 'P': return expand_prompt(s);
    case 'U': return change_case(s, NULL, 1, 1, 0);
    case 'u': return change_case(s, NULL, 1, 0, 0);
    case 'L': return change_case(s, NULL, 0, 1, 0);
    case 'K': return shell_quote(s);
    case 'A': {
      char *q = shell_quote(s);
      char *r = xstrcat3(name, "=", q);
      free(q);
      return r;
    }
    case 'a': {
      int f = var_flags(name);
      Buf b;
      buf_init(&b);
      if (f >= 0) {
        if (f & V_ARRAY) buf_putc(&b, 'a');
        if (f & V_ASSOC) buf_putc(&b, 'A');
        if (f & V_INTEGER) buf_putc(&b, 'i');
        if (f & V_LOWER) buf_putc(&b, 'l');
        if (f & V_NAMEREF) buf_putc(&b, 'n');
        if (f & V_READONLY) buf_putc(&b, 'r');
        if (f & V_UPPER) buf_putc(&b, 'u');
        if (f & V_EXPORT) buf_putc(&b, 'x');
      }
      return buf_take(&b);
    }
  }
  return xstrdup(s);
}


/* s[off:len] in bytes */
static char *substring (const char *s, long long off, int has_len, long long len) {
  long long n = (long long)strlen(s);
  if (off < 0) off += n;
  if (off < 0 || off > n) return xstrdup("");
  if (!has_len) len = n - off;
  else if (len < 0) len = n - off + len;
  if (len < 0) {
    sh_error("%lld: substring expression < 0", len);
    fail();
    return xstrdup("");
  }
  if (off + len > n) len = n - off;
  return xstrndup(s + off, (size_t)len);
}


/* the end of a ${ } operand at s: the next unquoted 'stop' char */
static size_t operand_len (const char *s, const char *stops) {
  size_t i = 0;
  int depth = 0;
  while (s[i]) {
    char c = s[i];
    if (c == '\\' && s[i + 1]) {
      i += 2;
      continue;
    }
    if (c == '\'' || c == '"' || c == '`' || (c == '$' && (s[i + 1] == '(' || s[i + 1] == '{'))) {
      i += parse_skip_subst(s + i, 0);
      continue;
    }
    if (c == '{') depth++;
    else if (c == '}') depth--;
    if (depth <= 0 && strchr(stops, c) != NULL) break;
    i++;
  }
  return i;
}


/* emits a value or a list of values into the fields */
static void emit (Exp *e, List *l) {
  size_t i;
  if (!l->is_list) {
    if (l->v.n > 0) put_split(e, l->v.v[0]);
    return;
  }
  if (e->in_dq) {
    if (l->star || !e->split) {	/* "$*": one word; "$@" in a=... too */
      char *j = join_with(&l->v, l->star);
      put_lits(e, j, strlen(j));
      free(j);
      return;
    }
    if (l->v.n == 0) {
      e->at_empty = 1;
      return;
    }
    for (i = 0; i < l->v.n; i++) {	/* "$@": one field each */
      if (i > 0) {
        e->live = 1;
        field_end(e);
      }
      put_lits(e, l->v.v[i], strlen(l->v.v[i]));
    }
    return;
  }
  if (!e->split) {	/* a=$@ */
    char *j = join_with(&l->v, l->star);
    put_split(e, j);
    free(j);
    return;
  }
  for (i = 0; i < l->v.n; i++) {	/* $@ unquoted: each is split again */
    if (i > 0) field_end(e);
    put_split(e, l->v.v[i]);
  }
}


/* applies f to every value of the list */
typedef char *(*MapFn) (const char *s, void *ud);

static void list_map (List *l, MapFn f, void *ud) {
  size_t i;
  for (i = 0; i < l->v.n; i++) {
    char *r = f(l->v.v[i], ud);
    free(l->v.v[i]);
    l->v.v[i] = r;
  }
}


typedef struct TrimArg { const char *pat; int back, longest; } TrimArg;
static char *map_trim (const char *s, void *ud) {
  TrimArg *a = (TrimArg *)ud;
  return trim(s, a->pat, a->back, a->longest);
}

typedef struct RepArg { const char *pat, *rep; int mode; } RepArg;
static char *map_replace (const char *s, void *ud) {
  RepArg *a = (RepArg *)ud;
  return replace(s, a->pat, a->rep, a->mode);
}

typedef struct CaseArg { const char *pat; int upper, all, toggle; } CaseArg;
static char *map_case (const char *s, void *ud) {
  CaseArg *a = (CaseArg *)ud;
  return change_case(s, a->pat, a->upper, a->all, a->toggle);
}

typedef struct TrArg { const char *name; char op; } TrArg;
static char *map_transform (const char *s, void *ud) {
  TrArg *a = (TrArg *)ud;
  return transform(a->name, s, a->op);
}


/* the default of ${x:-w}: in quotes a string, outside expanded in place */
static void put_operand (Exp *e, const char *w, size_t wl) {
  if (e->in_dq) {
    char *v = expand_to_str(w, wl, 0, 0);
    put_lits(e, v, strlen(v));
    free(v);
  }
  else {	/* unquoted: the result is split, its plain text too */
    int saved = e->split_text;
    e->split_text = e->split;
    walk(e, w, wl);
    e->split_text = saved;
  }
}


/* ${ ... }: body is the text between the braces */
static void expand_braced (Exp *e, const char *body) {
  List l;
  const char *p = body;
  char *name = NULL;
  int count = 0, indirect = 0;
  size_t n;
  list_init(&l);
  if (p[0] == '#' && p[1] != '\0' && strchr(":-=?+%/^,", p[1]) == NULL)
    count = 1, p++;	/* ${#x}; but ${#} and ${#-...} are $# */
  else if (p[0] == '#' && p[1] == '#' && p[2] == '\0')
    count = 1, p++;
  if (!count && p[0] == '!' && p[1] != '\0' && strchr("}:-=?+", p[1]) == NULL) {
    indirect = 1;	/* ${!x} ${!x[@]} ${!pre*} */
    p++;
  }
  n = param_name_len(p);
  if (n == 0) {
    sh_error("${%s}: bad substitution", body);
    fail();
    return;
  }
  name = xstrndup(p, n);
  p += n;
  if (*p == '[' && is_name(name)) {	/* a subscript */
    size_t k = 1, nl = strlen(name);
    int depth = 1;
    char *full;
    while (p[k] && depth > 0) {
      if (p[k] == '[') depth++;
      else if (p[k] == ']') depth--;
      if (depth > 0) k++;
    }
    if (p[k] != ']') {
      sh_error("${%s}: bad substitution", body);
      fail();
      free(name);
      return;
    }
    full = (char *)xmalloc(nl + k + 2);
    memcpy(full, name, nl);
    memcpy(full + nl, p, k + 1);
    full[nl + k + 1] = '\0';
    free(name);
    name = full;
    p += k + 1;
  }
  if (indirect) {
    size_t nl = strlen(name);
    if ((*p == '*' || *p == '@') && p[1] == '\0' && is_name(name)) {	/* ${!pre*} */
      l.is_list = 1;
      l.star = (*p == '*');
      var_names(&l.v, name, 0);
      emit(e, &l);
      vec_free(&l.v);
      free(name);
      return;
    }
    if (nl > 3 && name[nl - 1] == ']' && (name[nl - 2] == '@' || name[nl - 2] == '*') &&
        name[nl - 3] == '[') {	/* ${!a[@]}: the keys */
      char *base = xstrndup(name, nl - 3);
      l.is_list = 1;
      l.star = name[nl - 2] == '*';
      var_keys(base, &l.v);
      free(base);
    }
    else if (var_flags(name) >= 0 && (var_flags(name) & V_NAMEREF)) {
      Var *raw = NULL;
      Vec all;
      size_t i;
      vec_init(&all);	/* ${!ref}: the name it points at */
      var_all(&all);
      for (i = 0; i < all.n; i++)
        if (strcmp(((Var *)(void *)all.v[i])->name, name) == 0) raw = (Var *)(void *)all.v[i];
      free(all.v);
      if (raw && raw->val) vec_push(&l.v, xstrdup(raw->val));
      else l.unset = 1;
    }
    else {
      List ref;
      list_init(&ref);
      lookup(name, &ref);
      if (ref.v.n > 0 && ref.v.v[0][0] != '\0') lookup(ref.v.v[0], &l);
      else {
        l.unset = 1;
        if (ref.unset && O("nounset")) {
          unbound(name);
          vec_free(&ref.v);
          free(name);
          return;
        }
      }
      vec_free(&ref.v);
    }
  }
  else lookup(name, &l);
  if (count) {	/* ${#x} ${#a[@]} */
    char num[24];
    long long len;
    if (l.is_list) len = (long long)l.v.n;
    else {
      if (l.unset && O("nounset")) {
        unbound(name);
        vec_free(&l.v);
        free(name);
        return;
      }
      len = l.v.n ? (long long)utf8_count(l.v.v[0], strlen(l.v.v[0])) : 0;
    }
    vec_free(&l.v);
    list_init(&l);
    vec_push(&l.v, xstrdup(ll_to_str(len, num)));
    emit(e, &l);
    vec_free(&l.v);
    free(name);
    return;
  }
  if (*p == '\0') {	/* plain ${x} */
    if (l.unset && !l.is_list && O("nounset")) unbound(name);
    else emit(e, &l);
    vec_free(&l.v);
    free(name);
    return;
  }
  {
    int colon = 0, empty;
    char op = *p;
    if (op == ':' && p[1] != '\0' && strchr("-=?+", p[1]) != NULL) {
      colon = 1;
      op = *++p;
    }
    if (l.is_list) empty = colon ? (l.v.n == 0 || (l.v.n == 1 && l.v.v[0][0] == '\0'))
                                 : (l.v.n == 0 && l.unset);
    else empty = l.unset || (colon && (l.v.n == 0 || l.v.v[0][0] == '\0'));
    switch (op) {
      case '-':
      case '=':
      case '?':
      case '+': {
        const char *w = p + 1;
        size_t wl = strlen(w);
        if (op == '+') {
          if (!empty) put_operand(e, w, wl);	/* ${x:+w} */
          else if (e->in_dq) e->live = 1;
          break;
        }
        if (!empty) {
          emit(e, &l);
          break;
        }
        if (op == '-') put_operand(e, w, wl);
        else if (op == '=') {
          char *v = expand_to_str(w, wl, 0, 0);
          const char *br = strchr(name, '[');
          if (br != NULL) {	/* ${a[1]=x} */
            char *base = xstrndup(name, (size_t)(br - name));
            char *sub = xstrndup(br + 1, strlen(br) - 2);
            long long idx = 0;
            char *key = NULL;
            if (subscript(base, sub, &idx, &key) == 0) {
              if (key) var_akset(base, key, v);
              else var_aset(base, idx, v);
            }
            free(key);
            free(base);
            free(sub);
          }
          else if (!is_name(name)) {
            sh_error("$%s: cannot assign in this way", name);
            fail();
            free(v);
            break;
          }
          else if (var_set(name, v) != 0) fail();
          put_split(e, v);
          free(v);
        }
        else {	/* ? */
          char *v = expand_to_str(w, wl, 0, 0);
          if (*v) sh_error("%s: %s", name, v);
          else sh_error("%s: parameter null or not set", name);
          free(v);
          fail();
        }
        break;
      }
      case '#':
      case '%': {
        int longest = (p[1] == op);
        const char *w = p + 1 + longest;
        char *pat = expand_to_str(w, strlen(w), 0, 1);
        TrimArg a;
        if (l.unset && O("nounset") && !l.is_list) {
          unbound(name);
          free(pat);
          break;
        }
        a.pat = pat;
        a.back = (op == '%');
        a.longest = longest;
        list_map(&l, map_trim, &a);
        emit(e, &l);
        free(pat);
        break;
      }
      case '/': {
        int mode = 0;
        const char *w = p + 1;
        size_t pl;
        char *pat, *rep;
        RepArg a;
        if (*w == '/') mode = 1, w++;
        else if (*w == '#') mode = 2, w++;
        else if (*w == '%') mode = 3, w++;
        pl = operand_len(w, "/");
        pat = expand_to_str(w, pl, 0, 1);
        rep = (w[pl] == '/') ? expand_to_str(w + pl + 1, strlen(w + pl + 1), 0, 1)
                             : xstrdup("");
        a.pat = pat;
        a.rep = rep;
        a.mode = mode;
        if (l.unset && O("nounset") && !l.is_list) unbound(name);
        else {
          list_map(&l, map_replace, &a);
          emit(e, &l);
        }
        free(pat);
        free(rep);
        break;
      }
      case ':': {	/* ${x:off} ${x:off:len} ${a[@]:off:len} */
        const char *w = p + 1;
        size_t ol = operand_len(w, ":");
        char *os = expand_to_str(w, ol, 1, 0);
        long long off = 0, len = 0;
        int has_len = w[ol] == ':';
        if (arith_eval(os, &off) != 0) fail();
        if (has_len) {
          char *ls = expand_to_str(w + ol + 1, strlen(w + ol + 1), 1, 0);
          if (arith_eval(ls, &len) != 0) fail();
          free(ls);
        }
        free(os);
        if (l.is_list) {	/* a slice of the list */
          Vec out;
          long long cnt = (long long)l.v.n, i;
          vec_init(&out);
          if (strcmp(name, "@") == 0 || strcmp(name, "*") == 0) {	/* ${@:0} has $0 */
            vec_insert(&l.v, 0, xstrdup(sh_pos.n > 0 ? sh_pos.v[0] : MMC_NAME));
            cnt++;
          }
          if (off < 0) off += cnt;
          if (off < 0) off = cnt;
          if (!has_len) len = cnt - off;
          if (len < 0) {
            sh_error("%lld: substring expression < 0", len);
            fail();
            vec_free(&out);
            break;
          }
          for (i = off; i < cnt && i < off + len; i++) vec_push(&out, xstrdup(l.v.v[i]));
          vec_free(&l.v);
          l.v = out;
          emit(e, &l);
        }
        else {
          char *sub;
          if (l.unset && O("nounset")) {
            unbound(name);
            break;
          }
          sub = substring(l.v.n ? l.v.v[0] : "", off, has_len, len);
          vec_free(&l.v);
          vec_init(&l.v);
          vec_push(&l.v, sub);
          emit(e, &l);
        }
        break;
      }
      case '^':
      case ',':
      case '~': {
        int all = (p[1] == op);
        const char *w = p + 1 + all;
        char *pat = *w ? expand_to_str(w, strlen(w), 0, 1) : NULL;
        CaseArg a;
        a.pat = pat;
        a.upper = (op == '^');
        a.all = all;
        a.toggle = (op == '~');
        list_map(&l, map_case, &a);
        emit(e, &l);
        free(pat);
        break;
      }
      case '@': {
        TrArg a;
        a.name = name;
        a.op = p[1];
        if (p[1] == '\0' || p[2] != '\0' || strchr("QEPAaULuK", p[1]) == NULL) {
          sh_error("${%s}: bad substitution", body);
          fail();
          break;
        }
        if (l.unset && !l.is_list) {
          if (p[1] == 'a' && var_flags(name) >= 0) {
            char *r = transform(name, "", 'a');
            put_lits(e, r, strlen(r));
            free(r);
          }
          break;
        }
        list_map(&l, map_transform, &a);
        emit(e, &l);
        break;
      }
      default:
        sh_error("${%s}: bad substitution", body);
        fail();
        break;
    }
  }
  vec_free(&l.v);
  free(name);
}

/* }================================================================== */


/*
** {==================================================================
** The word walker
** ===================================================================
*/

/* $'...' : the ANSI-C escapes; with 'used', stops at the closing quote */
char *expand_ansi_c (const char *s, size_t n, size_t *used) {
  Buf b;
  size_t i = 0;
  buf_init(&b);
  while (i < n) {
    char c = s[i];
    if (used != NULL && c == '\'') {
      i++;
      break;
    }
    if (c != '\\' || i + 1 >= n) {
      buf_putc(&b, c);
      i++;
      continue;
    }
    c = s[++i];
    i++;
    switch (c) {
      case 'a': buf_putc(&b, '\a'); break;
      case 'b': buf_putc(&b, '\b'); break;
      case 'e': case 'E': buf_putc(&b, '\033'); break;
      case 'f': buf_putc(&b, '\f'); break;
      case 'n': buf_putc(&b, '\n'); break;
      case 'r': buf_putc(&b, '\r'); break;
      case 't': buf_putc(&b, '\t'); break;
      case 'v': buf_putc(&b, '\v'); break;
      case '\\': buf_putc(&b, '\\'); break;
      case '\'': buf_putc(&b, '\''); break;
      case '"': buf_putc(&b, '"'); break;
      case '?': buf_putc(&b, '?'); break;
      case 'c':
        if (i < n) {
          buf_putc(&b, (char)(s[i] & 0x1F));
          i++;
        }
        break;
      case 'x': {
        int v = 0, k = 0;
        while (k < 2 && i < n && isxdigit((unsigned char)s[i])) {
          v = v * 16 + (isdigit((unsigned char)s[i]) ? s[i] - '0'
                                                     : tolower((unsigned char)s[i]) - 'a' + 10);
          i++;
          k++;
        }
        if (k == 0) buf_puts(&b, "\\x");
        else buf_putc(&b, (char)v);
        break;
      }
      case 'u':
      case 'U': {
        unsigned long v = 0;
        int k = 0, max = (c == 'u') ? 4 : 8;
        while (k < max && i < n && isxdigit((unsigned char)s[i])) {
          v = v * 16 + (unsigned long)(isdigit((unsigned char)s[i]) ? s[i] - '0'
                                       : tolower((unsigned char)s[i]) - 'a' + 10);
          i++;
          k++;
        }
        if (k == 0) {
          buf_putc(&b, '\\');
          buf_putc(&b, c);
        }
        else if (v < 0x80) buf_putc(&b, (char)v);
        else if (v < 0x800) {
          buf_putc(&b, (char)(0xC0 | (v >> 6)));
          buf_putc(&b, (char)(0x80 | (v & 0x3F)));
        }
        else if (v < 0x10000) {
          buf_putc(&b, (char)(0xE0 | (v >> 12)));
          buf_putc(&b, (char)(0x80 | ((v >> 6) & 0x3F)));
          buf_putc(&b, (char)(0x80 | (v & 0x3F)));
        }
        else {
          buf_putc(&b, (char)(0xF0 | (v >> 18)));
          buf_putc(&b, (char)(0x80 | ((v >> 12) & 0x3F)));
          buf_putc(&b, (char)(0x80 | ((v >> 6) & 0x3F)));
          buf_putc(&b, (char)(0x80 | (v & 0x3F)));
        }
        break;
      }
      default:
        if (c >= '0' && c <= '7') {	/* octal, up to 3 digits */
          int v = c - '0', k = 1;
          while (k < 3 && i < n && s[i] >= '0' && s[i] <= '7') {
            v = v * 8 + (s[i] - '0');
            i++;
            k++;
          }
          buf_putc(&b, (char)v);
        }
        else {
          buf_putc(&b, '\\');
          buf_putc(&b, c);
        }
    }
  }
  if (used != NULL) *used = i;
  return buf_take(&b);
}


/* `...`: backslash only escapes $ ` \ inside */
static char *backquote_src (const char *s, size_t n) {
  Buf b;
  size_t i;
  buf_init(&b);
  for (i = 0; i < n; i++) {
    if (s[i] == '\\' && i + 1 < n && strchr("$`\\", s[i + 1]) != NULL) i++;
    buf_putc(&b, s[i]);
  }
  return buf_take(&b);
}


static void put_capture (Exp *e, const char *src) {
  size_t len = 0;
  char *out = sh_capture(src, &len);
  while (len > 0 && out[len - 1] == '\n') out[--len] = '\0';	/* trailing newlines go */
  if (strlen(out) != len) out[len] = '\0';
  if (e->in_dq) put_lits(e, out, strlen(out));
  else put_split(e, out);
  free(out);
}


static void put_number (Exp *e, long long v) {
  char num[24];
  ll_to_str(v, num);
  if (e->in_dq) put_lits(e, num, strlen(num));
  else put_split(e, num);
}


/* $...: returns bytes used from s (s[0] is '$') */
static size_t walk_dollar (Exp *e, const char *s, size_t avail) {
  int c = (unsigned char)(avail > 1 ? s[1] : '\0');
  if (c == '(') {
    size_t end = parse_skip_subst(s, 0);
    if (end > avail) end = avail;
    if (avail > 2 && s[2] == '(' && end >= 5 && s[end - 1] == ')' && s[end - 2] == ')') {
      char *expr = expand_to_str(s + 3, end - 5, 1, 0);	/* $(( arithmetic )) */
      long long v = 0;
      if (arith_eval(expr, &v) != 0) fail();
      free(expr);
      put_number(e, v);
      return end;
    }
    {
      char *src = xstrndup(s + 2, end >= 3 ? end - 3 : 0);
      put_capture(e, src);
      free(src);
    }
    return end;
  }
  if (c == '[') {	/* $[ expr ]: the old arithmetic */
    size_t end = 2;
    char *expr;
    long long v = 0;
    while (end < avail && s[end] != ']') end++;
    expr = expand_to_str(s + 2, end - 2, 1, 0);
    if (arith_eval(expr, &v) != 0) fail();
    free(expr);
    put_number(e, v);
    return end < avail ? end + 1 : end;
  }
  if (c == '{') {
    size_t end = parse_skip_subst(s, 0);
    char *body;
    if (end > avail) end = avail;
    body = xstrndup(s + 2, end >= 3 ? end - 3 : 0);
    expand_braced(e, body);
    free(body);
    return end;
  }
  if (c == '\'' && !e->in_dq) {	/* $'...' */
    size_t used = 0;
    char *v = expand_ansi_c(s + 2, avail - 2, &used);
    put_lits(e, v, strlen(v));
    free(v);
    return 2 + used;
  }
  {
    size_t n = (avail > 1) ? param_name_len(s + 1) : 0;
    List l;
    char *name;
    if (n == 0) {
      if (e->in_dq) put_lit(e, '$');
      else put_raw(e, '$');
      return 1;
    }
    if (isdigit((unsigned char)s[1])) n = 1;	/* $10 is ${1}0 */
    if (n > avail - 1) n = avail - 1;
    name = xstrndup(s + 1, n);
    list_init(&l);
    lookup(name, &l);
    if (l.unset && !l.is_list && O("nounset") && strcmp(name, "!") != 0) unbound(name);
    else emit(e, &l);
    vec_free(&l.v);
    free(name);
    return 1 + n;
  }
}


static void walk_dq (Exp *e, const char *s, size_t n) {
  size_t i = 0;
  int was_dq = e->in_dq;
  size_t before = e->cur.len, had_fields = e->fields.n;
  e->in_dq = 1;
  e->at_empty = 0;
  while (i < n) {
    char c = s[i];
    if (c == '\\' && i + 1 < n && strchr("$`\"\\\n", s[i + 1]) != NULL) {
      if (s[i + 1] != '\n') put_lit(e, s[i + 1]);
      i += 2;
    }
    else if (c == '$') i += walk_dollar(e, s + i, n - i);
    else if (c == '`') {
      size_t end = parse_skip_subst(s + i, 0);
      char *src;
      if (end > n - i) end = n - i;
      src = backquote_src(s + i + 1, end >= 2 ? end - 2 : 0);
      put_capture(e, src);
      free(src);
      i += end;
    }
    else {
      put_lit(e, c);
      i++;
    }
  }
  e->in_dq = was_dq;
  /* "" makes a field; "$@" with nothing to give does not */
  if (!(e->at_empty && e->cur.len == before && e->fields.n == had_fields)) e->live = 1;
  e->at_empty = 0;
}


static void walk (Exp *e, const char *raw, size_t n) {
  size_t i = 0;
  if (!e->in_dq && n > 0 && raw[0] == '~') i += expand_tilde(e, raw, 0);
  while (i < n) {
    char c = raw[i];
    if (c == '\\') {
      if (i + 1 < n) {
        if (raw[i + 1] != '\n') put_lit(e, raw[i + 1]);
        i += 2;
      }
      else {
        put_lit(e, '\\');
        i++;
      }
    }
    else if (c == '\'') {
      size_t j = i + 1;
      while (j < n && raw[j] != '\'') j++;
      put_lits(e, raw + i + 1, j - i - 1);
      i = (j < n) ? j + 1 : j;
    }
    else if (c == '"') {
      size_t end = parse_skip_subst(raw + i, 0);
      if (end > n - i) end = n - i;
      walk_dq(e, raw + i + 1, end >= 2 ? end - 2 : 0);
      i += end;
    }
    else if (c == '$' && i + 1 < n && raw[i + 1] == '"') {	/* $"..." */
      size_t end = parse_skip_subst(raw + i + 1, 0);
      if (end > n - i - 1) end = n - i - 1;
      walk_dq(e, raw + i + 2, end >= 2 ? end - 2 : 0);
      i += 1 + end;
    }
    else if (c == '$') i += walk_dollar(e, raw + i, n - i);
    else if (c == '`') {
      size_t end = parse_skip_subst(raw + i, 0);
      char *src;
      if (end > n - i) end = n - i;
      src = backquote_src(raw + i + 1, end >= 2 ? end - 2 : 0);
      put_capture(e, src);
      free(src);
      i += end;
    }
    else if ((c == '<' || c == '>') && i + 1 < n && raw[i + 1] == '(') {
      size_t end = parse_skip_subst(raw + i, 0);
      char *src, *path;
      if (end > n - i) end = n - i;
      src = xstrndup(raw + i + 2, end >= 3 ? end - 3 : 0);
      path = sh_procsubst(src, c == '>');
      if (path != NULL) put_lits(e, path, strlen(path));
      else fail();
      free(path);
      free(src);
      i += end;
    }
    else {
      if (e->split_text) {
        char one[2];
        one[0] = c;
        one[1] = '\0';
        put_split(e, one);
      }
      else put_raw(e, c);
      i++;
      /* a=~/x and PATH=~/a:~/b */
      if (e->assign && (c == ':' || (c == '=' && i == word_assign_pos(raw) + 1)) &&
          i < n && raw[i] == '~')
        i += expand_tilde(e, raw, i);
    }
  }
}


/* expands text to one string; pattern: keep the marks */
static char *expand_to_str (const char *raw, size_t n, int in_dq, int pattern) {
  Exp e;
  size_t i;
  Buf all;
  memset(&e, 0, sizeof(e));
  vec_init(&e.fields);
  buf_init(&e.cur);
  e.pattern = pattern;
  if (in_dq) walk_dq(&e, raw, n);
  else walk(&e, raw, n);
  field_end(&e);
  buf_init(&all);
  for (i = 0; i < e.fields.n; i++) {
    if (i > 0) buf_putc(&all, ' ');
    buf_puts(&all, e.fields.v[i]);
  }
  vec_free(&e.fields);
  if (pattern) return buf_take(&all);
  {
    char *m = buf_take(&all);
    char *r = unmark(m);
    free(m);
    return r;
  }
}

/* }================================================================== */


/*
** {==================================================================
** Brace expansion: a{b,c}d and {1..10..2}
** ===================================================================
*/

/* skips a quoted part or a substitution at w[i]; returns the new index */
static size_t skip_quoted (const char *w, size_t i) {
  char c = w[i];
  if (c == '\\' && w[i + 1]) return i + 2;
  if (c == '\'' || c == '"' || c == '`' ||
      (c == '$' && (w[i + 1] == '{' || w[i + 1] == '(')))
    return i + parse_skip_subst(w + i, 0);
  return i;
}


/* the matching '}' of the '{' at s[i] */
static size_t brace_close (const char *s, size_t i, int *has_comma) {
  int depth = 0;
  *has_comma = 0;
  while (s[i]) {
    size_t j = skip_quoted(s, i);
    char c;
    if (j != i) {
      i = j;
      continue;
    }
    c = s[i];
    if (c == '{') depth++;
    else if (c == '}') {
      if (--depth == 0) return i;
    }
    else if (c == ',' && depth == 1) *has_comma = 1;
    i++;
  }
  return 0;
}


static int seq_part (const char *s, size_t n, long long *v, int *width, int *is_char) {
  char buf[32];
  if (n == 0 || n >= sizeof(buf)) return 0;
  memcpy(buf, s, n);
  buf[n] = '\0';
  if (n == 1 && isalpha((unsigned char)buf[0])) {
    *v = (unsigned char)buf[0];
    *is_char = 1;
    *width = 0;
    return 1;
  }
  *is_char = 0;
  if (str_to_ll(buf, v) != 0) return 0;
  *width = ((buf[0] == '0' || (buf[0] == '-' && buf[1] == '0')) && n > 1) ? (int)n : 0;
  return 1;
}


/* {a..b[..step]} -> the items; 0 if it is not a sequence */
static int brace_seq (const char *s, size_t n, Vec *items) {
  const char *d1 = NULL, *d2 = NULL;
  size_t i;
  long long a, b, step = 1, k, count = 0;
  int wa, wb, ca, cb, cs = 0, ws, width;
  char num[48];
  for (i = 0; i + 1 < n; i++) {
    if (s[i] == '.' && s[i + 1] == '.') {
      if (d1 == NULL) d1 = s + i;
      else if (d2 == NULL) d2 = s + i;
      i++;
    }
  }
  if (d1 == NULL) return 0;
  if (!seq_part(s, (size_t)(d1 - s), &a, &wa, &ca)) return 0;
  if (d2 == NULL) {
    if (!seq_part(d1 + 2, (size_t)(s + n - d1 - 2), &b, &wb, &cb)) return 0;
  }
  else {
    if (!seq_part(d1 + 2, (size_t)(d2 - d1 - 2), &b, &wb, &cb)) return 0;
    if (!seq_part(d2 + 2, (size_t)(s + n - d2 - 2), &step, &ws, &cs) || cs) return 0;
  }
  if (ca != cb) return 0;
  if (step == 0) step = 1;
  if (step < 0) step = -step;
  width = wa > wb ? wa : wb;
  for (k = a; (a <= b ? k <= b : k >= b) && count < 100000; k += (a <= b ? step : -step), count++) {
    if (ca) {
      num[0] = (char)k;
      num[1] = '\0';
    }
    else if (width) sprintf(num, "%0*lld", width, k);
    else ll_to_str(k, num);
    vec_push(items, xstrdup(num));
  }
  return 1;
}


static void brace_expand (const char *w, Vec *out, int depth) {
  size_t i = 0, close;
  int has_comma = 0;
  if (depth > 32) {
    vec_push(out, xstrdup(w));
    return;
  }
  while (w[i]) {
    size_t j = skip_quoted(w, i);
    if (j != i) {
      i = j;
      continue;
    }
    if (w[i] == '{' && !(i > 0 && w[i - 1] == '$') &&
        (close = brace_close(w, i, &has_comma)) > 0) {
      Vec items;
      size_t k;
      char *pre = xstrndup(w, i);
      const char *post = w + close + 1;
      vec_init(&items);
      if (has_comma) {
        size_t start = i + 1, m = i + 1;
        int d = 0;
        while (m <= close) {
          size_t q = (m < close) ? skip_quoted(w, m) : m;
          char cm;
          if (q != m) {
            m = q;
            continue;
          }
          cm = w[m];
          if (cm == '{') d++;
          else if (cm == '}' && m < close) d--;
          if ((cm == ',' && d == 0) || m == close) {
            vec_push(&items, xstrndup(w + start, m - start));
            start = m + 1;
          }
          m++;
        }
      }
      else if (!brace_seq(w + i + 1, close - i - 1, &items)) {
        vec_free(&items);
        free(pre);
        i++;
        continue;	/* {x} or {a..}: stays literal, look further */
      }
      for (k = 0; k < items.n; k++) {
        char *joined = xstrcat3(pre, items.v[k], post);
        brace_expand(joined, out, depth + 1);
        free(joined);
      }
      vec_free(&items);
      free(pre);
      return;
    }
    i++;
  }
  vec_push(out, xstrdup(w));
}

/* }================================================================== */


char *unmark (const char *marked) {
  Buf b;
  buf_init(&b);
  for (; *marked; marked++) {
    if (*marked == QMARK && marked[1] != '\0') marked++;
    buf_putc(&b, *marked);
  }
  return buf_take(&b);
}


/* index of '=' when the word looks like NAME=, NAME+=, NAME[..]=, else 0 */
size_t word_assign_pos (const char *w) {
  size_t i;
  if (!(isalpha((unsigned char)w[0]) || w[0] == '_')) return 0;
  for (i = 1; isalnum((unsigned char)w[i]) || w[i] == '_'; i++)
    ;
  if (w[i] == '[') {
    int depth = 0;
    for (; w[i]; i++) {
      if (w[i] == '[') depth++;
      else if (w[i] == ']' && --depth == 0) break;
    }
    if (w[i] != ']') return 0;
    i++;
  }
  if (w[i] == '+' && w[i + 1] == '=') return i + 1;
  return (w[i] == '=') ? i : 0;
}


static void expand_one (const char *raw, Vec *out) {
  Exp e;
  size_t i;
  memset(&e, 0, sizeof(e));
  vec_init(&e.fields);
  buf_init(&e.cur);
  e.split = 1;
  walk(&e, raw, strlen(raw));
  field_end(&e);
  for (i = 0; i < e.fields.n; i++) {
    const char *m = e.fields.v[i];
    if (!O("noglob") && pat_has_glob(m, O("extglob"))) {
      size_t before = out->n;
      glob_expand(m, out);
      if (out->n == before) {
        if (O("failglob")) {
          char *u = unmark(m);
          sh_error("no match: %s", u);
          free(u);
          fail();
        }
        else if (!O("nullglob")) vec_push(out, unmark(m));
      }
    }
    else vec_push(out, unmark(m));
  }
  vec_free(&e.fields);
}


int expand_word (const char *raw, Vec *out) {
  size_t before = out->n;
  Vec braces;
  size_t i;
  vec_init(&braces);
  if (O("braceexpand") && strchr(raw, '{') != NULL) brace_expand(raw, &braces, 0);
  else vec_push(&braces, xstrdup(raw));
  for (i = 0; i < braces.n && !g_failed; i++) expand_one(braces.v[i], out);
  vec_free(&braces);
  return (int)(out->n - before);
}


int expand_words (char **raw, int n, Vec *out) {
  int i;
  g_failed = 0;
  for (i = 0; i < n && !g_failed; i++) expand_word(raw[i], out);
  return g_failed ? -1 : 0;
}


char *expand_str (const char *raw) {
  return expand_to_str(raw, strlen(raw), 0, 0);
}


char *expand_assign (const char *raw) {
  Exp e;
  size_t i;
  Buf all;
  char *m, *r;
  memset(&e, 0, sizeof(e));
  vec_init(&e.fields);
  buf_init(&e.cur);
  e.assign = 1;
  walk(&e, raw, strlen(raw));
  field_end(&e);
  buf_init(&all);
  for (i = 0; i < e.fields.n; i++) {
    if (i > 0) buf_putc(&all, ' ');
    buf_puts(&all, e.fields.v[i]);
  }
  vec_free(&e.fields);
  m = buf_take(&all);
  r = unmark(m);
  free(m);
  return r;
}


char *expand_pattern (const char *raw) {
  return expand_to_str(raw, strlen(raw), 0, 1);
}


/* here-document bodies: only $ ` and \ are special */
char *expand_heredoc (const char *text) {
  Exp e;
  size_t i = 0, n = strlen(text);
  char *m, *r;
  memset(&e, 0, sizeof(e));
  vec_init(&e.fields);
  buf_init(&e.cur);
  e.in_dq = 1;
  while (i < n) {
    char c = text[i];
    if (c == '\\' && i + 1 < n && strchr("$`\\\n", text[i + 1]) != NULL) {
      if (text[i + 1] != '\n') put_lit(&e, text[i + 1]);
      i += 2;
    }
    else if (c == '$') i += walk_dollar(&e, text + i, n - i);
    else if (c == '`') {
      size_t end = parse_skip_subst(text + i, 0);
      char *src;
      if (end > n - i) end = n - i;
      src = backquote_src(text + i + 1, end >= 2 ? end - 2 : 0);
      put_capture(&e, src);
      free(src);
      i += end;
    }
    else {
      put_lit(&e, c);
      i++;
    }
  }
  m = buf_take(&e.cur);
  r = unmark(m);
  free(m);
  vec_free(&e.fields);
  return r;
}


/* 'it''s' -> 'it'\''s', like bash's printf %q and declare -p */
char *shell_quote (const char *s) {
  Buf b;
  const char *p;
  int plain = *s != '\0';
  for (p = s; *p; p++)
    if (!(isalnum((unsigned char)*p) || strchr("_-./:,+=@%^", *p) != NULL)) plain = 0;
  if (plain) return xstrdup(s);
  buf_init(&b);
  buf_putc(&b, '\'');
  for (p = s; *p; p++) {
    if (*p == '\'') buf_puts(&b, "'\\''");
    else buf_putc(&b, *p);
  }
  buf_putc(&b, '\'');
  return buf_take(&b);
}


/* PS1-style escapes: \u \h \H \w \W \$ \n \t \d \e \[ \] \\, then $VAR */
char *expand_prompt (const char *ps) {
  Buf b;
  const char *p;
  char *s;
  buf_init(&b);
  for (p = ps; *p; p++) {
    if (*p != '\\' || p[1] == '\0') {
      buf_putc(&b, *p);
      continue;
    }
    p++;
    switch (*p) {
      case 'u': buf_puts(&b, var_get("USER") ? var_get("USER") : ""); break;
      case 'h': {
        const char *h = var_get("HOSTNAME");
        if (h) buf_putn(&b, h, strcspn(h, "."));
        break;
      }
      case 'H': buf_puts(&b, var_get("HOSTNAME") ? var_get("HOSTNAME") : ""); break;
      case 'w':
      case 'W': {
        const char *pwd = var_get("PWD"), *home = var_get("HOME");
        char *cwd = NULL;
        if (pwd == NULL) {
          char *native = os_getcwd();
          cwd = path_to_display(native);
          free(native);
          pwd = cwd;
        }
        if (*p == 'W') {
          const char *slash = strrchr(pwd, '/');
          if (home && strcmp(pwd, home) == 0) buf_putc(&b, '~');
          else buf_puts(&b, (slash && slash[1]) ? slash + 1 : pwd);
        }
        else if (home && *home && strncmp(pwd, home, strlen(home)) == 0 &&
                 (pwd[strlen(home)] == '\0' || pwd[strlen(home)] == '/')) {
          buf_putc(&b, '~');
          buf_puts(&b, pwd + strlen(home));
        }
        else buf_puts(&b, pwd);
        free(cwd);
        break;
      }
      case '$': buf_putc(&b, os_geteuid() == 0 ? '#' : '$'); break;
      case 'n': buf_putc(&b, '\n'); break;
      case 'r': buf_putc(&b, '\r'); break;
      case 'a': buf_putc(&b, '\a'); break;
      case 'e': buf_putc(&b, '\033'); break;
      case 's': buf_puts(&b, MMC_NAME); break;
      case 'v':
      case 'V': buf_puts(&b, MMC_VERSION); break;
      case 'j': {
        char num[24];
        buf_puts(&b, ll_to_str(job_count(), num));
        break;
      }
      case '#': {	/* this command's number in this shell */
        char num[24];
        buf_puts(&b, ll_to_str(sh_command_number + 1, num));
        break;
      }
      case '!': {	/* its number in the history */
        char num[24];
        buf_puts(&b, ll_to_str((long long)line_hist()->n + 1, num));
        break;
      }
      case 't':
      case 'T':
      case '@':
      case 'A':
      case 'd': {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char out[64];
        const char *fmt = (*p == 't') ? "%H:%M:%S" : (*p == 'T') ? "%I:%M:%S"
                        : (*p == '@') ? "%I:%M %p" : (*p == 'A') ? "%H:%M" : "%a %b %d";
        if (tm && strftime(out, sizeof(out), fmt, tm)) buf_puts(&b, out);
        break;
      }
      case '[':
      case ']': break;	/* markers for the line editor: nothing */
      case '\\': buf_putc(&b, '\\'); break;
      default:
        if (*p >= '0' && *p <= '7') {
          int v = 0, k = 0;
          while (k < 3 && *p >= '0' && *p <= '7') {
            v = v * 8 + (*p - '0');
            p++;
            k++;
          }
          p--;
          buf_putc(&b, (char)v);
        }
        else {
          buf_putc(&b, '\\');
          buf_putc(&b, *p);
        }
    }
  }
  s = buf_take(&b);
  if (O("promptvars")) {
    char *r = expand_to_str(s, strlen(s), 1, 0);
    free(s);
    return r;
  }
  return s;
}
