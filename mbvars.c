/*
** mbvars.c - builtins about variables and options:
** declare typeset local export readonly unset set shopt shift getopts let
*/

#include "mmc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* "value" the way declare -p prints it */
static void put_dq (Buf *b, const char *s) {
  buf_putc(b, '"');
  for (; *s; s++) {
    if (*s == '"' || *s == '\\' || *s == '$' || *s == '`') buf_putc(b, '\\');
    buf_putc(b, *s);
  }
  buf_putc(b, '"');
}


static void flag_letters (Buf *b, int f) {
  size_t before = b->len;
  if (f & V_ARRAY) buf_putc(b, 'a');
  if (f & V_ASSOC) buf_putc(b, 'A');
  if (f & V_INTEGER) buf_putc(b, 'i');
  if (f & V_LOWER) buf_putc(b, 'l');
  if (f & V_NAMEREF) buf_putc(b, 'n');
  if (f & V_READONLY) buf_putc(b, 'r');
  if (f & V_UPPER) buf_putc(b, 'u');
  if (f & V_EXPORT) buf_putc(b, 'x');
  if (b->len == before) buf_putc(b, '-');
}


void var_format (Buf *b, Var *v) {
  size_t i;
  char num[24];
  buf_puts(b, "declare -");
  flag_letters(b, v->flags);
  buf_putc(b, ' ');
  buf_puts(b, v->name);
  if (v->flags & V_UNSET) {
    buf_putc(b, '\n');
    return;
  }
  if (v->flags & (V_ARRAY | V_ASSOC)) {
    buf_puts(b, "=(");
    for (i = 0; i < v->n; i++) {
      buf_putc(b, '[');
      if (v->flags & V_ASSOC) {
        const char *k = v->el[i].key;
        int plain = *k != '\0';
        const char *p;
        for (p = k; *p; p++)
          if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == '.')) plain = 0;
        if (plain) buf_puts(b, k);
        else put_dq(b, k);
      }
      else buf_puts(b, ll_to_str(v->el[i].idx, num));
      buf_puts(b, "]=");
      put_dq(b, v->el[i].val);
      if (i + 1 < v->n || (v->flags & V_ASSOC)) buf_putc(b, ' ');
    }
    buf_puts(b, ")\n");
    return;
  }
  buf_putc(b, '=');
  put_dq(b, v->val ? v->val : "");
  buf_putc(b, '\n');
}


static void print_var (int out, Var *v) {
  Buf b;
  buf_init(&b);
  var_format(&b, v);
  os_write(out, b.s, b.len);
  buf_free(&b);
}


/* name[sub] -> name and sub (malloc'd); 0 if there is no subscript */
static int split_sub (const char *s, size_t n, char **name, char **sub) {
  const char *br = memchr(s, '[', n);
  if (br == NULL || s[n - 1] != ']') {
    *name = xstrndup(s, n);
    *sub = NULL;
    return 0;
  }
  *name = xstrndup(s, (size_t)(br - s));
  *sub = xstrndup(br + 1, (size_t)(s + n - 1 - br - 1));
  return 1;
}


static int set_elem (const char *name, const char *sub, const char *value, int append) {
  int f = var_flags(name);
  char *key = expand_str(sub);
  int r;
  if (expand_failed()) {
    free(key);
    return -1;
  }
  if (f >= 0 && (f & V_ASSOC)) {
    if (append) {
      const char *old = var_akget(name, key);
      char *nv = xstrcat3(old ? old : "", value, "");
      r = var_akset(name, key, nv);
      free(nv);
    }
    else r = var_akset(name, key, value);
  }
  else {
    long long idx = 0;
    if (arith_eval(key, &idx) != 0) {
      free(key);
      return -1;
    }
    if (append) {
      const char *old = var_aget(name, idx);
      char *nv = xstrcat3(old ? old : "", value, "");
      r = var_aset(name, idx, nv);
      free(nv);
    }
    else r = var_aset(name, idx, value);
  }
  free(key);
  return r;
}


/* a=( w1 [k]=v w2 ... ): 'append' for a+=( ... ) */
static int set_array (const char *name, const char *inner, int append, int assoc_hint) {
  Vec words;
  char *err;
  size_t i;
  int f = var_flags(name), assoc = assoc_hint || (f >= 0 && (f & V_ASSOC));
  long long next;
  char *pending_key = NULL;
  vec_init(&words);
  if ((err = parse_word_list(inner, &words)) != NULL) {
    sh_error("%s: %s", name, err);
    free(err);
    vec_free(&words);
    return -1;
  }
  if (!append) {
    if (var_make_array(name, assoc) != 0) {
      vec_free(&words);
      return -1;
    }
  }
  else if (f < 0 || !(f & (V_ARRAY | V_ASSOC))) {
    var_set_flags(name, assoc ? V_ASSOC : V_ARRAY, 0);
  }
  next = var_anext(name);
  for (i = 0; i < words.n; i++) {
    const char *w = words.v[i];
    if (w[0] == '[') {	/* [key]=value */
      const char *close = NULL, *p;
      int depth = 0;
      for (p = w; *p; p++) {
        if (*p == '[') depth++;
        else if (*p == ']' && --depth == 0) {
          close = p;
          break;
        }
      }
      if (close != NULL && (close[1] == '=' || (close[1] == '+' && close[2] == '='))) {
        char *sub = xstrndup(w + 1, (size_t)(close - w - 1));
        int plus = close[1] == '+';
        char *val = expand_assign(close + (plus ? 3 : 2));
        if (expand_failed() || set_elem(name, sub, val, plus) != 0) {
          free(sub);
          free(val);
          vec_free(&words);
          return -1;
        }
        if (!assoc) {
          long long idx = 0;
          char *k = expand_str(sub);
          arith_eval(k, &idx);
          free(k);
          next = idx + 1;
        }
        free(sub);
        free(val);
        continue;
      }
    }
    {
      Vec vals;
      size_t k;
      vec_init(&vals);
      if (expand_word(w, &vals) < 0 || expand_failed()) {
        vec_free(&vals);
        vec_free(&words);
        return -1;
      }
      for (k = 0; k < vals.n; k++) {
        if (assoc) {	/* ( k1 v1 k2 v2 ): pairs */
          if (pending_key == NULL) pending_key = xstrdup(vals.v[k]);
          else {
            var_akset(name, pending_key, vals.v[k]);
            free(pending_key);
            pending_key = NULL;
          }
        }
        else var_aset(name, next++, vals.v[k]);
      }
      vec_free(&vals);
    }
  }
  if (pending_key != NULL) {
    var_akset(name, pending_key, "");
    free(pending_key);
  }
  vec_free(&words);
  return 0;
}


/*
** NAME=value  NAME+=value  NAME[sub]=value  NAME=(...)  NAME+=(...)  NAME
** 'flags_on': attributes to give first; 'local': make it local first
*/
int assign_word (const char *word, int flags_on, int local) {
  size_t eq = word_assign_pos(word);
  size_t nlen;
  char *name, *sub;
  int append = 0, r = 0, has_sub;
  if (eq == 0) {	/* just a name: "declare -i x", "local y" */
    has_sub = split_sub(word, strlen(word), &name, &sub);
    if (!is_name(name)) {
      sh_error("`%s': not a valid identifier", word);
      free(name);
      free(sub);
      return -1;
    }
    if (local && var_local(name) != 0) r = -1;
    if (r == 0 && flags_on) r = var_set_flags(name, flags_on & ~V_READONLY, 0);
    if (r == 0 && (flags_on & V_READONLY)) r = var_set_flags(name, V_READONLY, 0);
    free(name);
    free(sub);
    (void)has_sub;
    return r;
  }
  nlen = eq;
  if (word[eq - 1] == '+') {
    append = 1;
    nlen--;
  }
  has_sub = split_sub(word, nlen, &name, &sub);
  if (!is_name(name)) {
    sh_error("`%.*s': not a valid identifier", (int)nlen, word);
    free(name);
    free(sub);
    return -1;
  }
  if (local && var_local(name) != 0) {
    free(name);
    free(sub);
    return -1;
  }
  if (flags_on & ~V_READONLY) r = var_set_flags(name, flags_on & ~V_READONLY, 0);
  if (r == 0) {
    const char *val = word + eq + 1;
    size_t vl = strlen(val);
    if (!has_sub && vl >= 2 && val[0] == '(' && val[vl - 1] == ')') {
      char *inner = xstrndup(val + 1, vl - 2);
      r = set_array(name, inner, append, (flags_on & V_ASSOC) != 0);
      free(inner);
    }
    else {
      char *v = expand_assign(val);
      if (expand_failed()) r = -1;
      else if (has_sub) r = set_elem(name, sub, v, append);
      else if (append) r = var_append(name, v);
      else r = var_set(name, v);
      free(v);
    }
  }
  if (r == 0 && (flags_on & V_READONLY)) r = var_set_flags(name, V_READONLY, 0);
  free(name);
  free(sub);
  return r;
}


/* the words after a declaration builtin were expanded as assignments,
** except a=( ... ), which comes raw: assign_word does the rest */
static int decl_assign (const char *arg, int on, int off, int local) {
  size_t eq = word_assign_pos(arg);
  int r;
  if (off) {
    char *name = eq ? xstrndup(arg, arg[eq - 1] == '+' ? eq - 1 : eq) : xstrdup(arg);
    if ((off & V_READONLY) && (var_flags(name) & V_READONLY)) {
      sh_error("%s: readonly variable", name);
      free(name);
      return -1;
    }
    var_set_flags(name, 0, off);
    free(name);
  }
  if (eq > 0) {
    const char *val = arg + eq + 1;
    size_t vl = strlen(val);
    if (vl >= 2 && val[0] == '(' && val[vl - 1] == ')') return assign_word(arg, on, local);
    {
      /* the value is already expanded: set it as it is */
      size_t nlen = eq;
      char *name, *sub;
      int append = arg[eq - 1] == '+', has;
      if (append) nlen--;
      has = split_sub(arg, nlen, &name, &sub);
      if (!is_name(name)) {
        sh_error("`%s': not a valid identifier", arg);
        free(name);
        free(sub);
        return -1;
      }
      r = 0;
      if (local) r = var_local(name);
      if (r == 0 && (on & ~V_READONLY)) r = var_set_flags(name, on & ~V_READONLY, 0);
      if (r == 0) {
        if (has) r = set_elem(name, sub, val, append);
        else if (append) r = var_append(name, val);
        else if ((on & (V_ARRAY | V_ASSOC)) && !has) {
          r = var_make_array(name, (on & V_ASSOC) != 0);
          if (r == 0) r = (on & V_ASSOC) ? var_akset(name, "0", val) : var_aset(name, 0, val);
        }
        else r = var_set(name, val);
      }
      if (r == 0 && (on & V_READONLY)) r = var_set_flags(name, V_READONLY, 0);
      free(name);
      free(sub);
      return r;
    }
  }
  return assign_word(arg, on, local);
}


static int parse_attrs (const char *a, int *on, int *off, int *print, int *funcs,
                        int *global, int *names_only) {
  int plus = a[0] == '+', *set = plus ? off : on;
  for (a++; *a; a++) {
    switch (*a) {
      case 'a': *set |= V_ARRAY; break;
      case 'A': *set |= V_ASSOC; break;
      case 'i': *set |= V_INTEGER; break;
      case 'l': *set |= V_LOWER; break;
      case 'u': *set |= V_UPPER; break;
      case 'c': *set |= V_CAPITAL; break;
      case 'n': *set |= V_NAMEREF; break;
      case 'r': *set |= V_READONLY; break;
      case 'x': *set |= V_EXPORT; break;
      case 't': break;
      case 'p': *print = 1; break;
      case 'f': *funcs = 1; break;
      case 'F': *funcs = 1; *names_only = 1; break;
      case 'g': *global = 1; break;
      case 'I': break;
      default:
        sh_error("declare: -%c: invalid option", *a);
        return -1;
    }
  }
  return 0;
}


/* the source of one command, without the blanks and ; around it */
static void put_trimmed (Buf *b, const char *src) {
  size_t n;
  if (src == NULL) return;
  while (*src == ' ' || *src == '\t' || *src == '\n') src++;
  n = strlen(src);
  while (n > 0 && strchr(" \t\n;", src[n - 1]) != NULL) n--;
  buf_putn(b, src, n);
}


/* a function the way bash shows it:  name () \n{ \n    cmd;\n    cmd\n} */
char *func_pretty (Func *f) {
  Buf b;
  Node *body = f->body;
  buf_init(&b);
  buf_printf(&b, "%s () \n", f->name);
  if (body->type != N_GROUP) {
    put_trimmed(&b, body->src);
    return buf_take(&b);
  }
  buf_puts(&b, "{ \n");
  if (body->a != NULL) {
    Node **items = &body->a;
    int n = 1, i;
    if (body->a->type == N_LIST) {
      items = body->a->kids;
      n = body->a->nkids;
    }
    for (i = 0; i < n; i++) {
      buf_puts(&b, "    ");
      put_trimmed(&b, items[i]->src);
      if (items[i]->flags & NF_BG) buf_puts(&b, " &");
      buf_puts(&b, i + 1 < n ? (items[i]->flags & NF_BG ? "\n" : ";\n") : "\n");
    }
  }
  buf_puts(&b, "}");
  return buf_take(&b);
}


static int print_funcs (int out, char **names, int n, int names_only) {
  Vec all;
  size_t i;
  int status = 0;
  vec_init(&all);
  if (n == 0) func_names(&all);
  else vec_copy(&all, names, (size_t)n);
  for (i = 0; i < all.n; i++) {
    Func *f = func_find(all.v[i]);
    if (f == NULL) {
      status = 1;
      continue;
    }
    if (names_only && n > 0) fd_printf(out, "%s\n", f->name);	/* declare -F name */
    else if (names_only) fd_printf(out, "declare -f %s\n", f->name);
    else {
      char *p = func_pretty(f);
      fd_printf(out, "%s\n", p);
      free(p);
    }
  }
  vec_free(&all);
  return status;
}


/* declare / typeset / local; 'is_local': called as local */
static int declare_common (int argc, char **argv, int out, int is_local) {
  int on = 0, off = 0, print = 0, funcs = 0, global = 0, names_only = 0, i, status = 0;
  for (i = 1; i < argc && (argv[i][0] == '-' || argv[i][0] == '+') && argv[i][1] != '\0'; i++) {
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    if (parse_attrs(argv[i], &on, &off, &print, &funcs, &global, &names_only) != 0) return 2;
  }
  if (is_local && !var_in_function()) {
    sh_error("local: can only be used in a function");
    return 1;
  }
  if (funcs) {
    if (i >= argc || print || names_only || on == 0) {
      if (on & V_READONLY || on & V_EXPORT) {
        for (; i < argc; i++) {
          Func *f = func_find(argv[i]);
          if (f) f->flags |= (on & (V_READONLY | V_EXPORT));
          else status = 1;
        }
        return status;
      }
      return print_funcs(out, argv + i, argc - i, names_only);
    }
    for (; i < argc; i++) {
      Func *f = func_find(argv[i]);
      if (f) f->flags |= on & (V_READONLY | V_EXPORT);
      else status = 1;
    }
    return status;
  }
  if (i >= argc) {	/* list */
    Vec vars;
    size_t k;
    vec_init(&vars);
    var_all(&vars);
    for (k = 0; k < vars.n; k++) {
      Var *v = (Var *)(void *)vars.v[k];
      if (v->flags & V_SPECIAL) continue;
      if (on && (v->flags & on) != on) continue;
      if (print || on) print_var(out, v);
      else if (!(v->flags & V_UNSET)) {
        if (v->flags & (V_ARRAY | V_ASSOC)) print_var(out, v);
        else {
          char *q = shell_quote(v->val ? v->val : "");
          fd_printf(out, "%s=%s\n", v->name, (v->val && *v->val) ? q : "''");
          free(q);
        }
      }
    }
    free(vars.v);
    return 0;
  }
  if (print) {
    for (; i < argc; i++) {
      Var *v = var_lookup(argv[i]);
      if (v == NULL) {
        Vec all;
        size_t k;
        int found = 0;
        vec_init(&all);	/* a nameref pointing nowhere */
        var_all(&all);
        for (k = 0; k < all.n; k++)
          if (strcmp(((Var *)(void *)all.v[k])->name, argv[i]) == 0) {
            print_var(out, (Var *)(void *)all.v[k]);
            found = 1;
          }
        free(all.v);
        if (!found) {
          sh_error("declare: %s: not found", argv[i]);
          status = 1;
        }
        continue;
      }
      print_var(out, v);
    }
    return status;
  }
  for (; i < argc; i++) {
    int local = is_local || (var_in_function() && !global);
    if (decl_assign(argv[i], on, off, local) != 0) status = 1;
  }
  return status;
}


int b_declare (int argc, char **argv, int in, int out, int err) {
  (void)in; (void)err;
  return declare_common(argc, argv, out, 0);
}


/*
** local: "-" keeps the set options, to come back when the function
** returns; with shopt -s localvar_inherit a new local starts with the
** value of the one it hides
*/
int b_local (int argc, char **argv, int in, int out, int err) {
  char **args = (char **)xmalloc(((size_t)argc + 1) * sizeof(char *));
  int n = 0, i, status, opts = 1, dash = 0;
  Vec made;
  (void)in; (void)err;
  vec_init(&made);
  for (i = 0; i < argc; i++) {
    const char *a = argv[i];
    if (i > 0 && opts && strcmp(a, "-") == 0) {
      dash = 1;
      continue;
    }
    if (i > 0 && (a[0] != '-' && a[0] != '+')) opts = 0;
    if (i > 0 && !opts && opt_get("localvar_inherit") && strchr(a, '=') == NULL &&
        is_name(a) && var_get(a) != NULL) {
      char *w = xstrcat3(a, "=", var_get(a));
      vec_push(&made, w);
      args[n++] = w;
      continue;
    }
    args[n++] = argv[i];
  }
  args[n] = NULL;
  if (dash) {
    if (!var_in_function()) {
      sh_error("local: can only be used in a function");
      free(args);
      vec_free(&made);
      return 1;
    }
    sh_local_dash();
  }
  status = (dash && n == 1) ? 0 : declare_common(n, args, out, 1);
  free(args);
  vec_free(&made);
  return status;
}


int b_export (int argc, char **argv, int in, int out, int err) {
  int i, unexport = 0, print = 0, funcs = 0, status = 0;
  (void)in; (void)err;
  for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
    const char *a = argv[i] + 1;
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    for (; *a; a++) {
      if (*a == 'n') unexport = 1;
      else if (*a == 'p') print = 1;
      else if (*a == 'f') funcs = 1;
      else {
        sh_error("export: -%c: invalid option", *a);
        return 2;
      }
    }
  }
  if (i >= argc || (print && i >= argc)) {
    Vec vars;
    size_t k;
    vec_init(&vars);
    var_all(&vars);
    for (k = 0; k < vars.n; k++) {
      Var *v = (Var *)(void *)vars.v[k];
      if ((v->flags & V_EXPORT) && !(v->flags & V_SPECIAL)) print_var(out, v);
    }
    free(vars.v);
    return 0;
  }
  for (; i < argc; i++) {
    if (funcs) {
      Func *f = func_find(argv[i]);
      if (f) {
        if (unexport) f->flags &= ~V_EXPORT;
        else f->flags |= V_EXPORT;
      }
      else status = 1;
      continue;
    }
    if (unexport) {
      size_t eq = word_assign_pos(argv[i]);
      char *name = eq ? xstrndup(argv[i], eq) : xstrdup(argv[i]);
      if (eq) decl_assign(argv[i], 0, 0, 0);
      var_set_flags(name, 0, V_EXPORT);
      free(name);
      continue;
    }
    if (decl_assign(argv[i], V_EXPORT, 0, 0) != 0) status = 1;
  }
  return status;
}


int b_readonly (int argc, char **argv, int in, int out, int err) {
  int i, status = 0, on = V_READONLY, funcs = 0;
  (void)in; (void)err;
  for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
    const char *a = argv[i] + 1;
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    for (; *a; a++) {
      if (*a == 'a') on |= V_ARRAY;
      else if (*a == 'A') on |= V_ASSOC;
      else if (*a == 'f') funcs = 1;
      else if (*a != 'p') {
        sh_error("readonly: -%c: invalid option", *a);
        return 2;
      }
    }
  }
  if (i >= argc) {
    Vec vars;
    size_t k;
    vec_init(&vars);
    var_all(&vars);
    for (k = 0; k < vars.n; k++) {
      Var *v = (Var *)(void *)vars.v[k];
      if (v->flags & V_READONLY) print_var(out, v);
    }
    free(vars.v);
    return 0;
  }
  for (; i < argc; i++) {
    if (funcs) {
      Func *f = func_find(argv[i]);
      if (f) f->flags |= V_READONLY;
      else status = 1;
    }
    else if (decl_assign(argv[i], on, 0, 0) != 0) status = 1;
  }
  return status;
}


int b_unset (int argc, char **argv, int in, int out, int err) {
  int i, mode = 0, status = 0;	/* 0 any, 'v' variables, 'f' functions, 'n' nameref */
  (void)in; (void)out; (void)err;
  for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    if (strchr(argv[i], 'f')) mode = 'f';
    else if (strchr(argv[i], 'v')) mode = 'v';
    else if (strchr(argv[i], 'n')) mode = 'n';
  }
  for (; i < argc; i++) {
    const char *a = argv[i];
    if (mode == 'f') {
      if (func_unset(a) != 0) status = 1;
      continue;
    }
    if (mode == 'n') {
      if (var_unset_local(a) != 0) status = 1;
      continue;
    }
    {
      char *name, *sub;
      if (split_sub(a, strlen(a), &name, &sub)) {	/* unset a[1] */
        int f = var_flags(name);
        if (strcmp(sub, "@") == 0 || strcmp(sub, "*") == 0) {
          if (var_unset(name) != 0) status = 1;
        }
        else if (f >= 0 && (f & V_ASSOC)) {
          char *k = expand_str(sub);
          if (var_akunset(name, k) != 0) status = 1;
          free(k);
        }
        else {
          long long idx = 0;
          char *k = expand_str(sub);
          if (arith_eval(k, &idx) != 0 || var_aunset(name, idx) != 0) status = 1;
          free(k);
        }
      }
      else if (!is_name(name)) {
        sh_error("unset: `%s': not a valid identifier", a);
        status = 1;
      }
      else if (var_flags(name) < 0 && mode == 0 && func_find(name) != NULL) func_unset(name);
      else if (var_unset(name) != 0) status = 1;
      free(name);
      free(sub);
    }
  }
  return status;
}


static void set_positional (char **args, int n) {
  int i;
  char *zero = xstrdup(sh_pos.n > 0 ? sh_pos.v[0] : MMC_NAME);
  vec_free(&sh_pos);
  vec_init(&sh_pos);
  vec_push(&sh_pos, zero);
  for (i = 0; i < n; i++) vec_push(&sh_pos, xstrdup(args[i]));
}


static void list_options (int out, int as_commands, int shopt, int only) {
  ShOpt *o;
  for (o = sh_opts; o->name; o++) {
    if (o->shopt != shopt) continue;
    if (only == 1 && !o->value) continue;
    if (only == 2 && o->value) continue;
    if (as_commands) {
      if (shopt) fd_printf(out, "shopt -%c %s\n", o->value ? 's' : 'u', o->name);
      else fd_printf(out, "set %co %s\n", o->value ? '-' : '+', o->name);
    }
    else if (shopt) fd_printf(out, "%-20s\t%s\n", o->name, o->value ? "on" : "off");
    else fd_printf(out, "%-15s\t%s\n", o->name, o->value ? "on" : "off");
  }
}


int b_set (int argc, char **argv, int in, int out, int err) {
  int i;
  (void)in; (void)err;
  if (argc == 1) {	/* every variable, then the functions */
    Vec vars;
    size_t k;
    vec_init(&vars);
    var_all(&vars);
    for (k = 0; k < vars.n; k++) {
      Var *v = (Var *)(void *)vars.v[k];
      if (v->flags & (V_UNSET | V_SPECIAL)) continue;
      if (v->flags & (V_ARRAY | V_ASSOC)) {
        Buf b;
        buf_init(&b);
        var_format(&b, v);
        /* "declare -a a=(...)" -> "a=(...)" */
        {
          const char *s = b.s + strlen("declare -");
          s = strchr(s, ' ') + 1;
          fd_puts(out, s);
        }
        buf_free(&b);
      }
      else {
        char *q = shell_quote(v->val ? v->val : "");
        fd_printf(out, "%s=%s\n", v->name, (v->val && *v->val) ? q : "''");
        free(q);
      }
    }
    free(vars.v);
    return 0;
  }
  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    int on = a[0] == '-';
    if (strcmp(a, "--") == 0) {
      set_positional(argv + i + 1, argc - i - 1);
      return 0;
    }
    if (strcmp(a, "-") == 0) {	/* "set -": -x and -v off, the rest positional */
      opt_set("xtrace", 0);
      opt_set("verbose", 0);
      if (i + 1 < argc) set_positional(argv + i + 1, argc - i - 1);
      return 0;
    }
    if (a[0] != '-' && a[0] != '+') {
      set_positional(argv + i, argc - i);
      return 0;
    }
    if (a[1] == 'o' && a[2] == '\0') {
      if (i + 1 >= argc) {
        list_options(out, !on, 0, 0);
        return 0;
      }
      if (opt_set(argv[i + 1], on) != 0) {
        const char *n = argv[i + 1];
        ShOpt *o;
        int found = 0;
        for (o = sh_opts; o->name; o++)
          if (!o->shopt && strcmp(o->name, n) == 0) found = 1;
        if (!found) {
          sh_error("set: %s: invalid option name", n);
          return 2;
        }
      }
      i++;
      continue;
    }
    {
      const char *p;
      for (p = a + 1; *p; p++) {
        if (*p == 'o') {
          if (i + 1 < argc && opt_set(argv[++i], on) == 0) continue;
          sh_error("set: -o: option requires an argument");
          return 2;
        }
        if (*p == 'i' || *p == 's' || *p == 'l' || *p == 'c' || *p == 'r') continue;
        if (opt_letter(*p, on) != 0) {
          sh_error("set: %c%c: invalid option", a[0], *p);
          return 2;
        }
      }
    }
  }
  return 0;
}


int b_shopt (int argc, char **argv, int in, int out, int err) {
  int i, mode = 0, quiet = 0, print = 0, setopt = 0, status = 0;	/* mode: 's' 'u' */
  (void)in; (void)err;
  for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
    const char *a = argv[i] + 1;
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    for (; *a; a++) {
      if (*a == 's') mode = 's';
      else if (*a == 'u') mode = 'u';
      else if (*a == 'q') quiet = 1;
      else if (*a == 'p') print = 1;
      else if (*a == 'o') setopt = 1;
      else {
        sh_error("shopt: -%c: invalid option", *a);
        return 2;
      }
    }
  }
  if (i >= argc) {
    if (quiet) return 0;
    list_options(out, print, !setopt, mode == 's' ? 1 : mode == 'u' ? 2 : 0);
    return 0;
  }
  for (; i < argc; i++) {
    ShOpt *o, *found = NULL;
    for (o = sh_opts; o->name; o++)
      if (o->shopt == !setopt && strcmp(o->name, argv[i]) == 0) found = o;
    if (found == NULL) {
      sh_error("shopt: %s: invalid shell option name", argv[i]);
      status = 1;
      continue;
    }
    if (mode == 's') found->value = 1;
    else if (mode == 'u') found->value = 0;
    else {
      if (!found->value) status = 1;
      if (!quiet) {
        if (print) fd_printf(out, "shopt -%c %s\n", found->value ? 's' : 'u', found->name);
        else fd_printf(out, "%-20s\t%s\n", found->name, found->value ? "on" : "off");
      }
    }
  }
  return status;
}


int b_shift (int argc, char **argv, int in, int out, int err) {
  long long n = 1;
  size_t i;
  (void)in; (void)out; (void)err;
  if (argc > 1 && (arith_eval(argv[1], &n) != 0 || n < 0)) {
    sh_error("shift: %s: shift count out of range", argv[1]);
    return 1;
  }
  if (sh_pos.n == 0 || (size_t)n > sh_pos.n - 1) {
    if (O("shift_verbose")) sh_error("shift: shift count out of range");
    return 1;
  }
  for (i = 1; i <= (size_t)n; i++) free(sh_pos.v[i]);
  memmove(sh_pos.v + 1, sh_pos.v + 1 + n, (sh_pos.n - 1 - (size_t)n + 1) * sizeof(char *));
  sh_pos.n -= (size_t)n;
  return 0;
}


int b_getopts (int argc, char **argv, int in, int out, int err) {
  static int optpos = 1;	/* inside a cluster like -abc */
  static long long last_ind = 1;
  const char *opts, *name;
  char **args;
  int nargs, silent;
  long long ind = 1;
  const char *ind_s = var_get("OPTIND");
  char optbuf[3];
  (void)in; (void)out; (void)err;
  if (argc < 3) {
    sh_error("getopts: usage: getopts optstring name [arg ...]");
    return 2;
  }
  opts = argv[1];
  name = argv[2];
  silent = opts[0] == ':';
  if (silent) opts++;
  if (argc > 3) {
    args = argv + 3;
    nargs = argc - 3;
  }
  else {
    args = sh_pos.v + 1;
    nargs = sh_pos.n > 0 ? (int)sh_pos.n - 1 : 0;
  }
  if (ind_s == NULL || str_to_ll(ind_s, &ind) != 0 || ind < 1) ind = 1;
  if (ind != last_ind || ind == 1) {	/* OPTIND was reset or moved: start fresh */
    if (ind == 1 && last_ind != 1) optpos = 1;
    if (ind != last_ind) optpos = 1;
  }
  if (ind > nargs) goto done;
  {
    const char *a = args[ind - 1];
    const char *spec;
    char c;
    if (optpos == 1) {
      if (a[0] != '-' || a[1] == '\0') goto done;
      if (strcmp(a, "--") == 0) {
        ind++;
        goto done;
      }
    }
    c = a[optpos];
    optbuf[0] = c;
    optbuf[1] = '\0';
    spec = (c != ':') ? strchr(opts, c) : NULL;
    optpos++;
    if (a[optpos] == '\0') {
      ind++;
      optpos = 1;
    }
    if (spec == NULL) {	/* unknown option */
      if (!silent) {
        sh_error("illegal option -- %c", c);
        var_unset("OPTARG");
      }
      else var_set("OPTARG", optbuf);
      var_set(name, "?");
    }
    else if (spec[1] == ':') {	/* needs an argument */
      if (optpos > 1) {	/* the rest of this word: -ofile */
        var_set("OPTARG", a + optpos);
        ind++;
        optpos = 1;
        var_set(name, optbuf);
      }
      else if (ind <= nargs) {
        var_set("OPTARG", args[ind - 1]);
        ind++;
        var_set(name, optbuf);
      }
      else {
        if (!silent) {
          sh_error("option requires an argument -- %c", c);
          var_unset("OPTARG");
          var_set(name, "?");
        }
        else {
          var_set("OPTARG", optbuf);
          var_set(name, ":");
        }
      }
    }
    else {
      var_unset("OPTARG");
      var_set(name, optbuf);
    }
    {
      char num[24];
      var_set("OPTIND", ll_to_str(ind, num));
      last_ind = ind;
    }
    return 0;
  }
 done:
  {
    char num[24];
    var_set("OPTIND", ll_to_str(ind, num));
    last_ind = ind;
    optpos = 1;
    var_set(name, "?");
    var_unset("OPTARG");
  }
  return 1;
}


int b_let (int argc, char **argv, int in, int out, int err) {
  long long v = 0;
  int i;
  (void)in; (void)out; (void)err;
  if (argc < 2) {
    sh_error("let: expression expected");
    return 1;
  }
  for (i = 1; i < argc; i++)
    if (arith_eval(argv[i], &v) != 0) return 1;
  return v == 0;
}
