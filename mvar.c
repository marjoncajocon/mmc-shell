/*
** mvar.c - shell variables
**
** Variables live in scopes: the global one, and one per running function
** (bash's dynamic scoping: a function sees the locals of its callers).
** Only exported variables go to the programs mmc starts. The environment
** is imported at start; names a shell cannot use, such as Windows'
** "ProgramFiles(x86)", are passed on to programs untouched.
*/

#include "mmc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct Scope {
  Var **tab;
  size_t size, count;
  struct Scope *up;
} Scope;

static Scope global_scope;
static Scope *top = &global_scope;
static int func_depth = 0;
static Vec passthrough;	/* "NAME=value" entries that are not shell names */
static unsigned long rand_state = 1;
static long long seconds_base = 0;


/*
** {==================================================================
** Hash tables
** ===================================================================
*/

static size_t hash (const char *s) {
  size_t h = 2166136261u;
  for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
  return h;
}


static Var *scope_find (Scope *sc, const char *name) {
  Var *v;
  if (sc->size == 0) return NULL;
  for (v = sc->tab[hash(name) & (sc->size - 1)]; v != NULL; v = v->next)
    if (strcmp(v->name, name) == 0) return v;
  return NULL;
}


static void scope_grow (Scope *sc) {
  size_t nsize = sc->size ? sc->size * 2 : 64, i;
  Var **ntab = (Var **)xmalloc(nsize * sizeof(Var *));
  memset(ntab, 0, nsize * sizeof(Var *));
  for (i = 0; i < sc->size; i++) {
    Var *v = sc->tab[i];
    while (v != NULL) {
      Var *next = v->next;
      size_t h = hash(v->name) & (nsize - 1);
      v->next = ntab[h];
      ntab[h] = v;
      v = next;
    }
  }
  free(sc->tab);
  sc->tab = ntab;
  sc->size = nsize;
}


static Var *scope_add (Scope *sc, const char *name) {
  Var *v;
  size_t h;
  if ((sc->count + 1) * 4 > sc->size * 3) scope_grow(sc);
  v = (Var *)xmalloc(sizeof(Var));
  memset(v, 0, sizeof(*v));
  v->name = xstrdup(name);
  v->flags = V_UNSET;
  h = hash(name) & (sc->size - 1);
  v->next = sc->tab[h];
  sc->tab[h] = v;
  sc->count++;
  return v;
}


static void elems_clear (Var *v) {
  size_t i;
  for (i = 0; i < v->n; i++) {
    free(v->el[i].key);
    free(v->el[i].val);
  }
  free(v->el);
  v->el = NULL;
  v->n = v->cap = 0;
}


static void var_free (Var *v) {
  free(v->name);
  free(v->val);
  elems_clear(v);
  free(v);
}


static void scope_remove (Scope *sc, const char *name) {
  Var **pp;
  if (sc->size == 0) return;
  for (pp = &sc->tab[hash(name) & (sc->size - 1)]; *pp != NULL; pp = &(*pp)->next) {
    if (strcmp((*pp)->name, name) == 0) {
      Var *v = *pp;
      *pp = v->next;
      var_free(v);
      sc->count--;
      return;
    }
  }
}


static void scope_clear (Scope *sc) {
  size_t i;
  for (i = 0; i < sc->size; i++) {
    Var *v = sc->tab[i];
    while (v != NULL) {
      Var *next = v->next;
      var_free(v);
      v = next;
    }
  }
  free(sc->tab);
  sc->tab = NULL;
  sc->size = sc->count = 0;
}

/* }================================================================== */


static Var *find_any (const char *name, Scope **where) {
  Scope *sc;
  for (sc = top; sc != NULL; sc = sc->up) {
    Var *v = scope_find(sc, name);
    if (v != NULL) {
      if (where) *where = sc;
      return v;
    }
  }
  return NULL;
}


/* follows "declare -n ref=target" */
static Var *resolve (const char *name, const char **real) {
  int depth;
  Var *v = find_any(name, NULL);
  *real = name;
  for (depth = 0; v != NULL && (v->flags & V_NAMEREF) && v->val != NULL &&
                  depth < 8; depth++) {
    *real = v->val;
    v = find_any(v->val, NULL);
  }
  return v;
}


Var *var_lookup (const char *name) {
  const char *real;
  return resolve(name, &real);
}


/* the variable to write to: created in the global scope if it is new */
static Var *writable (const char *name, int *err) {
  const char *real;
  Var *v = resolve(name, &real);
  *err = 0;
  if (v == NULL) {
    if (!is_name(real)) {
      sh_error("`%s': not a valid identifier", real);
      *err = 1;
      return NULL;
    }
    v = scope_add(&global_scope, real);
    if (O("allexport")) v->flags |= V_EXPORT;
  }
  else if (v->flags & V_READONLY) {
    sh_error("%s: readonly variable", v->name);
    *err = 1;
    return NULL;
  }
  return v;
}


/* integer, lower, upper attributes */
static char *convert (Var *v, const char *value) {
  char *r;
  if (v->flags & V_INTEGER) {
    long long n = 0;
    char num[24];
    if (arith_eval(value, &n) != 0) n = 0;
    return xstrdup(ll_to_str(n, num));
  }
  r = xstrdup(value);
  if (v->flags & (V_LOWER | V_UPPER | V_CAPITAL)) {
    char *p;
    for (p = r; *p; p++) {
      if (v->flags & V_UPPER) *p = (char)toupper((unsigned char)*p);
      else *p = (char)tolower((unsigned char)*p);
    }
    if ((v->flags & V_CAPITAL) && r[0]) r[0] = (char)toupper((unsigned char)r[0]);
  }
  return r;
}


static void special_set (Var *v, const char *value) {
  long long n = 0;
  str_to_ll(value, &n);
  if (strcmp(v->name, "RANDOM") == 0) rand_state = (unsigned long)n;
  else if (strcmp(v->name, "SECONDS") == 0)
    seconds_base = os_now_us() / 1000000 - n;
}


static const char *special_get (Var *v) {
  static char buf[4][48];
  static int k = 0;
  char *out = buf[k = (k + 1) & 3];
  const char *n = v->name;
  if (strcmp(n, "RANDOM") == 0) {
    rand_state = rand_state * 1103515245u + 12345u;
    return ll_to_str((long long)((rand_state >> 16) & 0x7FFF), out);
  }
  if (strcmp(n, "SRANDOM") == 0) {
    rand_state = rand_state * 1103515245u + 12345u;
    return ll_to_str((long long)(((rand_state >> 16) & 0xFFFF) |
                                 ((os_now_us() & 0xFFFF) << 16)), out);
  }
  if (strcmp(n, "SECONDS") == 0)
    return ll_to_str(os_now_us() / 1000000 - seconds_base, out);
  if (strcmp(n, "EPOCHSECONDS") == 0) return ll_to_str(os_now_us() / 1000000, out);
  if (strcmp(n, "EPOCHREALTIME") == 0) {
    long long us = os_now_us();
    sprintf(out, "%ld.%06ld", (long)(us / 1000000), (long)(us % 1000000));
    return out;
  }
  if (strcmp(n, "LINENO") == 0) return ll_to_str(sh_lineno, out);
  if (strcmp(n, "BASHPID") == 0) return ll_to_str(os_getpid(), out);
  return v->val;
}


const char *var_get (const char *name) {
  const char *real;
  Var *v = resolve(name, &real);
  if (v == NULL || (v->flags & V_UNSET)) return NULL;
  if (v->flags & V_SPECIAL) return special_get(v);
  if (v->flags & V_ASSOC) return var_akget(real, "0");
  if (v->flags & V_ARRAY) return var_aget(real, 0);
  return v->val;
}


int var_is_set (const char *name) {
  return var_get(name) != NULL;
}


int var_flags (const char *name) {
  Var *v = var_lookup(name);
  return v ? v->flags : -1;
}


int var_set (const char *name, const char *value) {
  int err;
  Var *v = writable(name, &err);
  if (v == NULL) return -1;
  if (v->flags & V_SPECIAL) {
    special_set(v, value);
    return 0;
  }
  if (v->flags & (V_ARRAY | V_ASSOC)) {
    return (v->flags & V_ASSOC) ? var_akset(v->name, "0", value)
                                : var_aset(v->name, 0, value);
  }
  free(v->val);
  v->val = convert(v, value);
  v->flags &= ~V_UNSET;
  if (O("allexport")) v->flags |= V_EXPORT;
  if (strcmp(v->name, "PATH") == 0) {	/* keep PATH tidy and Linux style */
    char *norm = path_list_normalize(v->val);
    free(v->val);
    v->val = norm;
    var_path_changed();
  }
  return 0;
}


int var_append (const char *name, const char *value) {
  Var *v = var_lookup(name);
  if (v != NULL && (v->flags & V_INTEGER) && !(v->flags & (V_ARRAY | V_ASSOC))) {
    char *expr = xstrcat3(v->val ? v->val : "0", "+(", value);
    char *full = xstrcat3(expr, ")", "");
    int r = var_set(name, full);
    free(expr);
    free(full);
    return r;
  }
  if (v != NULL && (v->flags & V_ARRAY)) {	/* a+=x appends to a[0] */
    const char *old = var_aget(v->name, 0);
    char *nv = xstrcat3(old ? old : "", value, "");
    int r = var_aset(v->name, 0, nv);
    free(nv);
    return r;
  }
  {
    const char *old = var_get(name);
    char *nv = xstrcat3(old ? old : "", value, "");
    int r = var_set(name, nv);
    free(nv);
    return r;
  }
}


int var_unset (const char *name) {
  Scope *where = NULL;
  const char *real;
  Var *v = resolve(name, &real);
  if (v == NULL) return 0;
  if (v->flags & V_READONLY) {
    sh_error("%s: cannot unset: readonly variable", v->name);
    return -1;
  }
  find_any(v->name, &where);
  if (where != NULL) {
    int path = strcmp(v->name, "PATH") == 0;
    if (where != &global_scope) {	/* a local: stays local, but unset */
      free(v->val);
      v->val = NULL;
      elems_clear(v);
      v->flags = (v->flags & V_EXPORT) | V_UNSET;
    }
    else scope_remove(where, v->name);
    if (path) var_path_changed();
  }
  return 0;
}


/* "unset -n ref": the reference itself */
int var_unset_local (const char *name) {
  Scope *where = NULL;
  Var *v = find_any(name, &where);
  if (v == NULL) return 0;
  if (v->flags & V_READONLY) {
    sh_error("%s: cannot unset: readonly variable", v->name);
    return -1;
  }
  scope_remove(where, name);
  return 0;
}


int var_set_flags (const char *name, int on, int off) {
  const char *real;
  Var *v = resolve(name, &real);
  if ((on & V_NAMEREF) || (off & V_NAMEREF)) {	/* the reference itself */
    v = find_any(name, NULL);
    real = name;
  }
  if (v == NULL) {
    if (!is_name(real)) {
      sh_error("`%s': not a valid identifier", real);
      return -1;
    }
    v = scope_add(func_depth > 0 && top != &global_scope ? &global_scope : &global_scope, real);
  }
  if ((v->flags & V_READONLY) && (off & V_READONLY)) {
    sh_error("%s: readonly variable", v->name);
    return -1;
  }
  v->flags = (v->flags | on) & ~off;
  if (on & V_UPPER) v->flags &= ~V_LOWER;
  if (on & V_LOWER) v->flags &= ~V_UPPER;
  if ((on & (V_ARRAY | V_ASSOC)) && v->val != NULL) {	/* scalar becomes a[0] */
    char *old = v->val;
    v->val = NULL;
    if (on & V_ASSOC) var_akset(v->name, "0", old);
    else var_aset(v->name, 0, old);
    free(old);
  }
  return 0;
}


int var_in_function (void) {
  return func_depth > 0;
}


int var_local (const char *name) {
  Var *v;
  if (func_depth == 0 || top == &global_scope) return -1;
  if (!is_name(name)) {
    sh_error("`%s': not a valid identifier", name);
    return -1;
  }
  if ((v = scope_find(top, name)) == NULL) {
    Var *outer = find_any(name, NULL);
    v = scope_add(top, name);
    /* bash: a local copies the export attribute, not the value */
    if (outer != NULL) v->flags |= outer->flags & V_EXPORT;
  }
  return 0;
}


void var_scope_push (void) {
  Scope *sc = (Scope *)xmalloc(sizeof(Scope));
  memset(sc, 0, sizeof(*sc));
  sc->up = top;
  top = sc;
  func_depth++;
}


void var_scope_pop (void) {
  Scope *sc = top;
  if (sc == &global_scope) return;
  top = sc->up;
  scope_clear(sc);
  free(sc);
  func_depth--;
}


/*
** {==================================================================
** Arrays
** ===================================================================
*/

static Var *array_var (const char *name, int assoc, int *err) {
  Var *v = writable(name, err);
  if (v == NULL) return NULL;
  if (!(v->flags & (V_ARRAY | V_ASSOC))) {
    char *old = v->val;
    v->val = NULL;
    v->flags |= assoc ? V_ASSOC : V_ARRAY;
    v->flags &= ~V_UNSET;
    if (old != NULL) {	/* x=1; x[1]=2: the old value is x[0] */
      Elem *e;
      v->el = (Elem *)xmalloc(sizeof(Elem) * 4);
      v->cap = 4;
      e = &v->el[v->n++];
      e->idx = 0;
      e->key = assoc ? xstrdup("0") : NULL;
      e->val = old;
    }
  }
  v->flags &= ~V_UNSET;
  return v;
}


static Elem *elem_add (Var *v, size_t at) {
  if (v->n + 1 > v->cap) {
    v->cap = v->cap ? v->cap * 2 : 8;
    v->el = (Elem *)xrealloc(v->el, v->cap * sizeof(Elem));
  }
  memmove(v->el + at + 1, v->el + at, (v->n - at) * sizeof(Elem));
  v->n++;
  memset(&v->el[at], 0, sizeof(Elem));
  return &v->el[at];
}


/* index of idx in the sorted elements, or where it would go */
static size_t elem_search (const Var *v, long long idx, int *found) {
  size_t lo = 0, hi = v->n;
  *found = 0;
  if (v->n > 0 && v->el[v->n - 1].idx < idx) return v->n;	/* appending */
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (v->el[mid].idx < idx) lo = mid + 1;
    else hi = mid;
  }
  if (lo < v->n && v->el[lo].idx == idx) *found = 1;
  return lo;
}


int var_aset (const char *name, long long idx, const char *value) {
  int err, found;
  size_t at;
  Var *v = array_var(name, 0, &err);
  Elem *e;
  if (v == NULL) return -1;
  if (v->flags & V_ASSOC) {
    char num[24];
    return var_akset(v->name, ll_to_str(idx, num), value);
  }
  if (idx < 0) {	/* a[-1]: from the end */
    idx += var_anext(v->name);
    if (idx < 0) {
      sh_error("%s: bad array subscript", v->name);
      return -1;
    }
  }
  at = elem_search(v, idx, &found);
  e = found ? &v->el[at] : elem_add(v, at);
  e->idx = idx;
  free(e->val);
  e->val = convert(v, value);
  return 0;
}


int var_akset (const char *name, const char *key, const char *value) {
  int err;
  size_t i;
  Var *v = array_var(name, 1, &err);
  Elem *e = NULL;
  if (v == NULL) return -1;
  if (!(v->flags & V_ASSOC)) {
    long long idx = 0;
    arith_eval(key, &idx);
    return var_aset(v->name, idx, value);
  }
  for (i = 0; i < v->n; i++)
    if (strcmp(v->el[i].key, key) == 0) e = &v->el[i];
  if (e == NULL) {
    e = elem_add(v, v->n);
    e->key = xstrdup(key);
  }
  free(e->val);
  e->val = convert(v, value);
  return 0;
}


const char *var_aget (const char *name, long long idx) {
  const char *real;
  Var *v = resolve(name, &real);
  int found;
  size_t at;
  if (v == NULL || (v->flags & V_UNSET)) return NULL;
  if (v->flags & V_ASSOC) {
    char num[24];
    return var_akget(real, ll_to_str(idx, num));
  }
  if (!(v->flags & V_ARRAY)) {
    if (idx == 0 || idx == -1) return (v->flags & V_SPECIAL) ? special_get(v) : v->val;
    return NULL;
  }
  if (idx < 0) idx += (v->n > 0) ? v->el[v->n - 1].idx + 1 : 0;
  at = elem_search(v, idx, &found);
  return found ? v->el[at].val : NULL;
}


const char *var_akget (const char *name, const char *key) {
  const char *real;
  Var *v = resolve(name, &real);
  size_t i;
  if (v == NULL || (v->flags & V_UNSET)) return NULL;
  if (!(v->flags & V_ASSOC)) {
    long long idx = 0;
    arith_eval(key, &idx);
    return var_aget(real, idx);
  }
  for (i = 0; i < v->n; i++)
    if (strcmp(v->el[i].key, key) == 0) return v->el[i].val;
  return NULL;
}


static int elem_remove (Var *v, size_t at) {
  free(v->el[at].key);
  free(v->el[at].val);
  memmove(v->el + at, v->el + at + 1, (v->n - at - 1) * sizeof(Elem));
  v->n--;
  return 0;
}


int var_aunset (const char *name, long long idx) {
  const char *real;
  Var *v = resolve(name, &real);
  int found;
  size_t at;
  if (v == NULL) return 0;
  if (v->flags & V_READONLY) {
    sh_error("%s: cannot unset: readonly variable", v->name);
    return -1;
  }
  if (v->flags & V_ASSOC) {
    char num[24];
    return var_akunset(real, ll_to_str(idx, num));
  }
  if (!(v->flags & V_ARRAY)) return (idx == 0) ? var_unset(real) : 0;
  if (idx < 0) idx += (v->n > 0) ? v->el[v->n - 1].idx + 1 : 0;
  at = elem_search(v, idx, &found);
  return found ? elem_remove(v, at) : 0;
}


int var_akunset (const char *name, const char *key) {
  const char *real;
  Var *v = resolve(name, &real);
  size_t i;
  if (v == NULL) return 0;
  if (!(v->flags & V_ASSOC)) {
    long long idx = 0;
    arith_eval(key, &idx);
    return var_aunset(real, idx);
  }
  for (i = 0; i < v->n; i++)
    if (strcmp(v->el[i].key, key) == 0) return elem_remove(v, i);
  return 0;
}


void var_values (const char *name, Vec *out) {
  const char *real;
  Var *v = resolve(name, &real);
  size_t i;
  if (v == NULL || (v->flags & V_UNSET)) return;
  if (!(v->flags & (V_ARRAY | V_ASSOC))) {
    const char *s = var_get(real);
    if (s != NULL) vec_push(out, xstrdup(s));
    return;
  }
  for (i = 0; i < v->n; i++) vec_push(out, xstrdup(v->el[i].val));
}


void var_keys (const char *name, Vec *out) {
  const char *real;
  Var *v = resolve(name, &real);
  size_t i;
  char num[24];
  if (v == NULL || (v->flags & V_UNSET)) return;
  if (!(v->flags & (V_ARRAY | V_ASSOC))) {
    if (var_get(real) != NULL) vec_push(out, xstrdup("0"));
    return;
  }
  for (i = 0; i < v->n; i++)
    vec_push(out, (v->flags & V_ASSOC) ? xstrdup(v->el[i].key)
                                       : xstrdup(ll_to_str(v->el[i].idx, num)));
}


size_t var_count (const char *name) {
  const char *real;
  Var *v = resolve(name, &real);
  if (v == NULL || (v->flags & V_UNSET)) return 0;
  if (!(v->flags & (V_ARRAY | V_ASSOC))) return var_get(real) != NULL;
  return v->n;
}


long long var_anext (const char *name) {
  Var *v = var_lookup(name);
  if (v == NULL || (v->flags & V_UNSET)) return 0;
  if (!(v->flags & (V_ARRAY | V_ASSOC))) return v->val != NULL ? 1 : 0;
  if (v->flags & V_ASSOC) return (long long)v->n;
  return v->n > 0 ? v->el[v->n - 1].idx + 1 : 0;
}


int var_make_array (const char *name, int assoc) {
  int err;
  Var *v = writable(name, &err);
  if (v == NULL) return -1;
  if (assoc && (v->flags & V_ARRAY)) {
    sh_error("%s: cannot convert indexed to associative array", v->name);
    return -1;
  }
  free(v->val);
  v->val = NULL;
  elems_clear(v);
  if (!(v->flags & V_ASSOC)) v->flags |= assoc ? V_ASSOC : V_ARRAY;
  v->flags &= ~V_UNSET;
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** Listing, environment, subshell copies
** ===================================================================
*/

static int cmp_var (const void *a, const void *b) {
  const Var *x = *(Var *const *)a, *y = *(Var *const *)b;
  return strcmp(x->name, y->name);
}


/* every visible variable (inner scopes hide outer ones), sorted */
void var_all (Vec *out) {
  Scope *sc;
  Vec seen;
  size_t i;
  vec_init(&seen);
  for (sc = top; sc != NULL; sc = sc->up) {
    for (i = 0; i < sc->size; i++) {
      Var *v;
      for (v = sc->tab[i]; v != NULL; v = v->next) {
        size_t k;
        int dup = 0;
        for (k = 0; k < out->n && !dup; k++)
          dup = strcmp(((Var *)(void *)out->v[k])->name, v->name) == 0;
        if (!dup) {
          if (out->n + 2 > out->cap) {
            out->cap = out->cap ? out->cap * 2 : 64;
            out->v = (char **)xrealloc(out->v, out->cap * sizeof(char *));
          }
          out->v[out->n++] = (char *)(void *)v;
          out->v[out->n] = NULL;
        }
      }
    }
  }
  if (out->n > 1) qsort(out->v, out->n, sizeof(char *), cmp_var);
  vec_free(&seen);
}


/* names only; 'all' also lists unset (declared) ones */
void var_names (Vec *out, const char *prefix, int all) {
  Vec vars;
  size_t i, n = prefix ? strlen(prefix) : 0;
  vec_init(&vars);
  var_all(&vars);
  for (i = 0; i < vars.n; i++) {
    Var *v = (Var *)(void *)vars.v[i];
    if (!all && (v->flags & V_UNSET)) continue;
    if (n == 0 || strncmp(v->name, prefix, n) == 0) vec_push(out, xstrdup(v->name));
  }
  free(vars.v);	/* the Var pointers are not owned */
}


void var_env (Vec *out) {
  Vec vars;
  size_t i;
  vec_init(&vars);
  var_all(&vars);
  for (i = 0; i < vars.n; i++) {
    Var *v = (Var *)(void *)vars.v[i];
    const char *val;
    if (!(v->flags & V_EXPORT) || (v->flags & (V_UNSET | V_ARRAY | V_ASSOC)))
      continue;
    val = (v->flags & V_SPECIAL) ? special_get(v) : v->val;
    if (val == NULL) continue;
    vec_push(out, xstrcat3(v->name, "=", val));
  }
  free(vars.v);
  for (i = 0; i < passthrough.n; i++) vec_push(out, xstrdup(passthrough.v[i]));
}


static Var *var_copy (const Var *v) {
  Var *c = (Var *)xmalloc(sizeof(Var));
  size_t i;
  *c = *v;
  c->name = xstrdup(v->name);
  c->val = v->val ? xstrdup(v->val) : NULL;
  c->next = NULL;
  if (v->n > 0) {
    c->el = (Elem *)xmalloc(v->cap * sizeof(Elem));
    for (i = 0; i < v->n; i++) {
      c->el[i].idx = v->el[i].idx;
      c->el[i].key = v->el[i].key ? xstrdup(v->el[i].key) : NULL;
      c->el[i].val = xstrdup(v->el[i].val);
    }
  }
  else {
    c->el = NULL;
    c->cap = 0;
  }
  return c;
}


static void scope_copy (Scope *dst, const Scope *src) {
  size_t i;
  memset(dst, 0, sizeof(*dst));
  if (src->size == 0) return;
  dst->size = src->size;
  dst->count = src->count;
  dst->tab = (Var **)xmalloc(src->size * sizeof(Var *));
  for (i = 0; i < src->size; i++) {
    Var *v, **tail = &dst->tab[i];
    *tail = NULL;
    for (v = src->tab[i]; v != NULL; v = v->next) {
      *tail = var_copy(v);
      tail = &(*tail)->next;
    }
  }
}


typedef struct Saved {
  Scope *scopes;	/* top first */
  int n, depth;
  unsigned long rand_state;
} Saved;


void *var_save (void) {
  Saved *s = (Saved *)xmalloc(sizeof(Saved));
  Scope *sc;
  int n = 0, i;
  for (sc = top; sc != NULL; sc = sc->up) n++;
  s->scopes = (Scope *)xmalloc((size_t)n * sizeof(Scope));
  for (sc = top, i = 0; sc != NULL; sc = sc->up, i++) scope_copy(&s->scopes[i], sc);
  s->n = n;
  s->depth = func_depth;
  s->rand_state = rand_state;
  return s;
}


void var_restore (void *saved) {
  Saved *s = (Saved *)saved;
  int i;
  int path_differs = 0;
  {
    const char *now = var_get("PATH");
    Var *old = scope_find(&s->scopes[s->n - 1], "PATH");
    path_differs = (now == NULL) != (old == NULL || old->val == NULL) ||
                   (now != NULL && old != NULL && old->val != NULL &&
                    strcmp(now, old->val) != 0);
  }
  while (top != &global_scope) var_scope_pop();
  scope_clear(&global_scope);
  global_scope = s->scopes[s->n - 1];
  global_scope.up = NULL;
  for (i = s->n - 2; i >= 0; i--) {
    Scope *sc = (Scope *)xmalloc(sizeof(Scope));
    *sc = s->scopes[i];
    sc->up = top;
    top = sc;
  }
  func_depth = s->depth;
  (void)rand_state;
  free(s->scopes);
  free(s);
  if (path_differs) var_path_changed();
}


void var_path_changed (void) {
  sh_hash_clear();
}


void var_init (void) {
  Vec env;
  size_t i;
  static const char *const specials[] = {
    "RANDOM", "SRANDOM", "SECONDS", "EPOCHSECONDS", "EPOCHREALTIME", "LINENO",
    "BASHPID", NULL
  };
  vec_init(&env);
  vec_init(&passthrough);
  os_env_list(&env);
  for (i = 0; i < env.n; i++) {
    char *eq = strchr(env.v[i], '=');
    Var *v;
    if (eq == NULL || eq == env.v[i]) continue;
    *eq = '\0';
    if (!is_name(env.v[i])) {	/* ProgramFiles(x86) and friends */
      *eq = '=';
      vec_push(&passthrough, xstrdup(env.v[i]));
      continue;
    }
#ifdef _WIN32
    if (m_stricmp(env.v[i], "PATH") == 0) strcpy(env.v[i], "PATH");
#endif
    if ((v = scope_find(&global_scope, env.v[i])) == NULL)
      v = scope_add(&global_scope, env.v[i]);
    free(v->val);
    v->val = xstrdup(eq + 1);
    v->flags = V_EXPORT;
  }
  vec_free(&env);
  rand_state = (unsigned long)(os_now_us() ^ (long long)os_getpid() * 2654435761u);
  seconds_base = os_now_us() / 1000000;
  for (i = 0; specials[i] != NULL; i++) {
    Var *v = scope_find(&global_scope, specials[i]);
    if (v == NULL) v = scope_add(&global_scope, specials[i]);
    v->flags = V_SPECIAL;
  }
}

/* }================================================================== */
