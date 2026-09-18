/*
** mpattern.c - shell patterns and filename globbing
**
** Patterns arrive "marked" (see mexpand.c): a QMARK byte in front of a
** character makes it literal, because it was quoted or escaped.
**   *  ?  [abc] [a-z] [!x] [^x] [[:alpha:]]
**   extglob: ?(a|b) *(a|b) +(a|b) @(a|b) !(a|b)
** Globbing walks directories one path component at a time; "**" with
** globstar crosses any number of directories.
*/

#include "mmc.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>


static int fold (int c, int flags) {
  return (flags & PM_NOCASE) ? tolower(c) : c;
}


static int is_ext_open (const char *p, const char *pe, int flags) {
  return (flags & PM_EXTGLOB) && p + 1 < pe && p[1] == '(' &&
         strchr("?*+@!", *p) != NULL;
}


/* the ')' that closes the '(' at p (p points at the '('); pe if none */
static const char *close_paren (const char *p, const char *pe) {
  int depth = 0;
  for (; p < pe; p++) {
    if (*p == QMARK && p + 1 < pe) {
      p++;
      continue;
    }
    if (*p == '(') depth++;
    else if (*p == ')' && --depth == 0) return p;
  }
  return pe;
}


/* [ ... ] at p (the '['); returns the end (after ']') or NULL if it is
** not a bracket expression; *hit tells whether c is in it */
static const char *bracket (const char *p, const char *pe, int c, int flags,
                            int *hit) {
  int neg = 0, first = 1;
  *hit = 0;
  p++;
  if (p < pe && (*p == '!' || *p == '^')) {
    neg = 1;
    p++;
  }
  while (p < pe && (first || *p != ']')) {
    int lo, hi;
    first = 0;
    if (*p == '[' && p + 1 < pe && p[1] == ':') {
      const char *e = p + 2;
      char name[16];
      size_t n;
      while (e + 1 < pe && !(e[0] == ':' && e[1] == ']')) e++;
      if (e + 1 >= pe) return NULL;
      n = (size_t)(e - p - 2);
      if (n < sizeof(name)) {
        memcpy(name, p + 2, n);
        name[n] = '\0';
        if ((strcmp(name, "alpha") == 0 && isalpha(c)) ||
            (strcmp(name, "digit") == 0 && isdigit(c)) ||
            (strcmp(name, "alnum") == 0 && isalnum(c)) ||
            (strcmp(name, "upper") == 0 && (isupper(c) || ((flags & PM_NOCASE) && isalpha(c)))) ||
            (strcmp(name, "lower") == 0 && (islower(c) || ((flags & PM_NOCASE) && isalpha(c)))) ||
            (strcmp(name, "space") == 0 && isspace(c)) ||
            (strcmp(name, "blank") == 0 && (c == ' ' || c == '\t')) ||
            (strcmp(name, "punct") == 0 && ispunct(c)) ||
            (strcmp(name, "print") == 0 && isprint(c)) ||
            (strcmp(name, "graph") == 0 && isgraph(c)) ||
            (strcmp(name, "cntrl") == 0 && iscntrl(c)) ||
            (strcmp(name, "xdigit") == 0 && isxdigit(c)) ||
            (strcmp(name, "word") == 0 && (isalnum(c) || c == '_')))
          *hit = 1;
      }
      p = e + 2;
      continue;
    }
    if (*p == QMARK && p + 1 < pe) p++;
    else if (*p == '\\' && p + 1 < pe) p++;
    lo = (unsigned char)*p++;
    hi = lo;
    if (p + 1 < pe && *p == '-' && p[1] != ']') {
      p++;
      if (*p == QMARK && p + 1 < pe) p++;
      hi = (unsigned char)*p++;
    }
    if (c >= lo && c <= hi) *hit = 1;
    else if ((flags & PM_NOCASE) && fold(c, flags) >= fold(lo, flags) &&
             fold(c, flags) <= fold(hi, flags))
      *hit = 1;
  }
  if (p >= pe) return NULL;	/* no closing ']': a literal '[' */
  if (neg) *hit = !*hit;
  return p + 1;
}


static int match (const char *p, const char *pe, const char *s, const char *se,
                  int flags);


/* does one of the '|' separated alternatives of an extglob match s..se? */
static int alt_match (const char *a, const char *ae, const char *s,
                      const char *se, int flags) {
  const char *start = a;
  int depth = 0;
  for (; a <= ae; a++) {
    if (a < ae && *a == QMARK && a + 1 < ae) {
      a++;
      continue;
    }
    if (a < ae && *a == '(') depth++;
    else if (a < ae && *a == ')') depth--;
    else if (a == ae || (*a == '|' && depth == 0)) {
      if (match(start, a, s, se, flags & ~PM_PERIOD)) return 1;
      start = a + 1;
    }
  }
  return 0;
}


/* kind at p, alternatives in (open+1 .. close), then the rest */
static int ext_match (char kind, const char *alts, const char *alts_end,
                      const char *rest, const char *pe, const char *s,
                      const char *se, int flags) {
  const char *k;
  switch (kind) {
    case '@':
      for (k = s; k <= se; k++)
        if (alt_match(alts, alts_end, s, k, flags) && match(rest, pe, k, se, flags))
          return 1;
      return 0;
    case '?':
      if (match(rest, pe, s, se, flags)) return 1;
      for (k = s; k <= se; k++)
        if (alt_match(alts, alts_end, s, k, flags) && match(rest, pe, k, se, flags))
          return 1;
      return 0;
    case '+':
    case '*':
      if (kind == '*' && match(rest, pe, s, se, flags)) return 1;
      for (k = s + 1; k <= se; k++) {
        if (!alt_match(alts, alts_end, s, k, flags)) continue;
        if (match(rest, pe, k, se, flags)) return 1;
        /* one more repetition from k */
        if (ext_match('*', alts, alts_end, rest, pe, k, se, flags)) return 1;
      }
      return 0;
    case '!':	/* any s..k that no alternative matches as a whole */
      for (k = s; k <= se; k++) {
        if (alt_match(alts, alts_end, s, k, flags)) continue;
        if (match(rest, pe, k, se, flags)) return 1;
      }
      return 0;
  }
  return 0;
}


static int match (const char *p, const char *pe, const char *s, const char *se,
                  int flags) {
  const char *s0 = s;
  while (p < pe) {
    int c = (s < se) ? (unsigned char)*s : -1;
    if (*p == QMARK && p + 1 < pe) {
      if (c < 0 || fold(c, flags) != fold((unsigned char)p[1], flags)) return 0;
      p += 2;
      s++;
      continue;
    }
    if (is_ext_open(p, pe, flags)) {
      const char *cl = close_paren(p + 1, pe);
      if (cl < pe) return ext_match(*p, p + 2, cl, cl + 1, pe, s, se, flags);
    }
    switch (*p) {
      case '*': {
        const char *k;
        while (p < pe && *p == '*' && !is_ext_open(p, pe, flags)) p++;
        if ((flags & PM_PERIOD) && s == s0 && c == '.') return 0;
        if (p == pe) {	/* a trailing star takes the rest */
          if (flags & PM_PATHNAME) {
            for (k = s; k < se; k++)
              if (*k == '/') return 0;
          }
          return 1;
        }
        for (k = s;; k++) {
          if (match(p, pe, k, se, flags & ~PM_PERIOD)) return 1;
          if (k >= se || ((flags & PM_PATHNAME) && *k == '/')) return 0;
        }
      }
      case '?':
        if (c < 0) return 0;
        if ((flags & PM_PATHNAME) && c == '/') return 0;
        if ((flags & PM_PERIOD) && s == s0 && c == '.') return 0;
        p++;
        s += utf8_len(s);
        if (s > se) s = se;
        continue;
      case '[': {
        int hit;
        const char *e;
        if (c < 0) return 0;
        e = bracket(p, pe, c, flags, &hit);
        if (e == NULL) {	/* a literal '[' */
          if (c != '[') return 0;
          p++;
          s++;
          continue;
        }
        if (!hit) return 0;
        if ((flags & PM_PERIOD) && s == s0 && c == '.') return 0;
        p = e;
        s++;
        continue;
      }
      case '\\':	/* an escape that survived (from a variable's value) */
        if (p + 1 < pe) {
          if (c < 0 || fold(c, flags) != fold((unsigned char)p[1], flags)) return 0;
          p += 2;
          s++;
          continue;
        }
        /* fall through */
      default:
        if (c < 0 || fold(c, flags) != fold((unsigned char)*p, flags)) return 0;
        p++;
        s++;
        continue;
    }
  }
  return s == se;
}


int pat_match (const char *pat, const char *s, int flags) {
  if (O("nocasematch")) flags |= PM_NOCASE;
  return match(pat, pat + strlen(pat), s, s + strlen(s), flags);
}


int pat_match_len (const char *pat, const char *s, size_t n, int flags) {
  if (O("nocasematch")) flags |= PM_NOCASE;
  return match(pat, pat + strlen(pat), s, s + n, flags);
}


int pat_has_glob (const char *m, int extglob) {
  for (; *m; m++) {
    if (*m == QMARK && m[1] != '\0') {
      m++;
      continue;
    }
    if (*m == '*' || *m == '?') return 1;
    if (*m == '[' && strchr(m + 1, ']') != NULL) return 1;
    if (extglob && *m == '!' && m[1] == '(') return 1;
    if (extglob && (*m == '@' || *m == '+') && m[1] == '(') return 1;
  }
  return 0;
}


/*
** {==================================================================
** Globbing
** ===================================================================
*/

static int c_locale (void) {
  const char *v = var_get("LC_ALL");
  if (v == NULL || *v == '\0') v = var_get("LC_COLLATE");
  if (v == NULL || *v == '\0') v = var_get("LANG");
  return v == NULL || *v == '\0' || strcmp(v, "C") == 0 || strcmp(v, "POSIX") == 0 ||
         strncmp(v, "C.", 2) == 0;
}


static int cmp_c (const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}


/* like a UTF-8 locale: case folded first, then the exact bytes */
static int cmp_locale (const void *a, const void *b) {
  const char *x = *(const char *const *)a, *y = *(const char *const *)b;
  const char *p = x, *q = y;
  for (;; p++, q++) {	/* punctuation is skipped at first, like glibc */
    while (*p && !isalnum((unsigned char)*p) && ((unsigned char)*p) < 0x80) p++;
    while (*q && !isalnum((unsigned char)*q) && ((unsigned char)*q) < 0x80) q++;
    if (*p == '\0' || *q == '\0') break;
    if (tolower((unsigned char)*p) != tolower((unsigned char)*q))
      return tolower((unsigned char)*p) - tolower((unsigned char)*q);
  }
  if (*p != *q) return (*p == '\0') ? -1 : 1;
  {
    int r = m_stricmp(x, y);
    if (r) return r;
  }
  return -strcmp(x, y);	/* lower case first */
}


static void sort_names (Vec *v) {
  if (v->n > 1) qsort(v->v, v->n, sizeof(char *), c_locale() ? cmp_c : cmp_locale);
}


static int exists (const char *posix, int want_dir) {
  OsStat st;
  char *native = path_to_native(posix);
  int r = os_stat(native, &st) == 0 && (!want_dir || st.is_dir);
  if (!r && !want_dir && os_lstat(native, &st) == 0) r = 1;	/* dangling link */
  free(native);
  return r;
}


static void glob_rec (const char *prefix, const char *pat, Vec *out, int flags);


/* "**": this directory and every directory below it */
static void globstar (const char *prefix, const char *rest, Vec *out, int flags,
                      int depth) {
  Vec names;
  size_t i;
  char *dir = path_to_native(prefix[0] ? prefix : ".");
  if (rest != NULL) glob_rec(prefix, rest, out, flags);
  vec_init(&names);
  os_listdir(dir, &names);
  sort_names(&names);
  for (i = 0; i < names.n && depth < 64; i++) {
    char *next = xstrcat3(prefix, names.v[i], "/");
    OsStat st, lst;
    char *native = path_to_native(next);
    int is_dir = os_stat(native, &st) == 0 && st.is_dir &&
                 !(os_lstat(native, &lst) == 0 && lst.is_link);
    free(native);
    if (names.v[i][0] == '.' && !O("dotglob")) {
      free(next);
      continue;
    }
    if (is_dir) {
      if (rest == NULL) vec_push(out, xstrcat3(prefix, names.v[i], "/"));
      globstar(next, rest, out, flags, depth + 1);
    }
    else if (rest == NULL) vec_push(out, xstrcat3(prefix, names.v[i], ""));
    free(next);
  }
  free(dir);
  vec_free(&names);
}


/* 'prefix' is literal text already matched; 'pat' is still marked */
static void glob_rec (const char *prefix, const char *pat, Vec *out, int flags) {
  const char *slash = strchr(pat, '/');
  size_t n = slash ? (size_t)(slash - pat) : strlen(pat);
  char *comp = xstrndup(pat, n);
  const char *rest = slash;
  while (rest && *rest == '/') rest++;
  if (rest && *rest == '\0') rest = NULL;
  if (strcmp(comp, "**") == 0 && O("globstar")) {
    if (rest == NULL && slash == NULL) globstar(prefix, NULL, out, flags, 0);
    else if (rest == NULL) {	/* a pattern ending in two stars and a slash: directories only */
      Vec all;
      size_t i;
      vec_init(&all);
      globstar(prefix, NULL, &all, flags, 0);
      if (prefix[0] == '\0' || exists(prefix, 1)) vec_push(out, xstrdup(prefix));
      for (i = 0; i < all.n; i++) {
        size_t len = strlen(all.v[i]);
        if (len > 0 && all.v[i][len - 1] == '/') vec_push(out, xstrdup(all.v[i]));
      }
      vec_free(&all);
    }
    else globstar(prefix, rest, out, flags, 0);
    free(comp);
    return;
  }
  if (!pat_has_glob(comp, O("extglob"))) {
    char *lit = unmark(comp);
    char *next = xstrcat3(prefix, lit, slash ? "/" : "");
    if (rest) glob_rec(next, rest, out, flags);
    else if (exists(next, slash != NULL)) vec_push(out, xstrdup(next));
    free(lit);
    free(next);
  }
  else {
    Vec names;
    size_t i;
    int mflags = flags | PM_PERIOD;
    char *dir = path_to_native(prefix[0] ? prefix : ".");
    if (O("dotglob")) mflags &= ~PM_PERIOD;
    if (comp[0] == '.' || (comp[0] == QMARK && comp[1] == '.')) mflags &= ~PM_PERIOD;
    vec_init(&names);
    os_listdir(dir, &names);
    sort_names(&names);
    for (i = 0; i < names.n; i++) {
      const char *name = names.v[i];
      char *next;
      if (!match(comp, comp + strlen(comp), name, name + strlen(name), mflags)) continue;
      next = xstrcat3(prefix, name, slash ? "/" : "");
      if (rest) {
        if (exists(next, 1)) glob_rec(next, rest, out, flags);
        free(next);
      }
      else if (slash && !exists(next, 1)) free(next);
      else vec_push(out, next);
    }
    free(dir);
    vec_free(&names);
  }
  free(comp);
}


void glob_expand (const char *marked, Vec *out) {
  int flags = 0;
  size_t before = out->n;
  if (O("extglob")) flags |= PM_EXTGLOB;
  if (O("nocaseglob")) flags |= PM_NOCASE;
  if (marked[0] == '/') {	/* absolute: start at the root */
    const char *p = marked;
    while (*p == '/') p++;
    glob_rec("/", p, out, flags);
  }
  else glob_rec("", marked, out, flags);
  /* results of one pattern are sorted as a whole */
  if (out->n - before > 1) {
    Vec part;
    part.v = out->v + before;
    part.n = out->n - before;
    part.cap = part.n;
    sort_names(&part);
  }
}

/* }================================================================== */
