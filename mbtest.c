/*
** mbtest.c - test, [ and [[ ]]
*/

#include "mmc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static int file_stat (const char *path, OsStat *st, int link) {
  char *native = path_to_native(path);
  int r = link ? os_lstat(native, st) : os_stat(native, st);
  free(native);
  return r == 0 && st->exists;
}


static int file_access (const char *path, int what) {
  char *native = path_to_native(path);
  int r = os_access(native, what);
  free(native);
  return r;
}


/* -1: not a unary operator */
int test_unary (const char *op, const char *arg) {
  OsStat st;
  if (op[0] != '-' || op[1] == '\0' || op[2] != '\0') return -1;
  switch (op[1]) {
    case 'a':
    case 'e': return file_stat(arg, &st, 0);
    case 'f': return file_stat(arg, &st, 0) && !st.is_dir && !st.is_chr && !st.is_blk &&
                     !st.is_fifo && !st.is_sock;
    case 'd': return file_stat(arg, &st, 0) && st.is_dir;
    case 'b': return file_stat(arg, &st, 0) && st.is_blk;
    case 'c': return file_stat(arg, &st, 0) && st.is_chr;
    case 'p': return file_stat(arg, &st, 0) && st.is_fifo;
    case 'S': return file_stat(arg, &st, 0) && st.is_sock;
    case 'h':
    case 'L': return file_stat(arg, &st, 1) && st.is_link;
    case 's': return file_stat(arg, &st, 0) && st.size > 0;
    case 'r': return file_access(arg, 'r');
    case 'w': return file_access(arg, 'w');
    case 'x': return file_access(arg, 'x');
    case 'g': return file_stat(arg, &st, 0) && (st.mode & 02000);
    case 'u': return file_stat(arg, &st, 0) && (st.mode & 04000);
    case 'k': return file_stat(arg, &st, 0) && (st.mode & 01000);
    case 'O': return file_stat(arg, &st, 0) && st.uid == os_geteuid();
    case 'G': return file_stat(arg, &st, 0);
    case 'N': return file_stat(arg, &st, 0) && st.mtime >= st.atime;
    case 't': {
      long long fd;
      if (str_to_ll(arg, &fd) != 0 || fd < 0 || fd >= MMC_FDS || sh_fd[fd] < 0) return 0;
      return os_is_tty(sh_fd[fd]);
    }
    case 'z': return arg[0] == '\0';
    case 'n': return arg[0] != '\0';
    case 'o': return opt_get(arg);
    case 'v': {
      const char *br = strchr(arg, '[');
      if (br != NULL && arg[strlen(arg) - 1] == ']') {	/* -v a[1] */
        char *name = xstrndup(arg, (size_t)(br - arg));
        char *sub = xstrndup(br + 1, strlen(br + 1) - 1);
        int f = var_flags(name), r;
        if (strcmp(sub, "@") == 0 || strcmp(sub, "*") == 0) r = var_count(name) > 0;
        else if (f >= 0 && (f & V_ASSOC)) r = var_akget(name, sub) != NULL;
        else {
          long long idx = 0;
          arith_eval(sub, &idx);
          r = var_aget(name, idx) != NULL;
        }
        free(name);
        free(sub);
        return r;
      }
      return var_is_set(arg) || (var_flags(arg) >= 0 && (var_flags(arg) & (V_ARRAY | V_ASSOC)) &&
                                 var_count(arg) > 0);
    }
    case 'R': return var_flags(arg) >= 0 && (var_flags(arg) & V_NAMEREF);
  }
  return -1;
}


static int to_int (const char *s, long long *v, int arith) {
  if (arith) return arith_eval(s, v);
  if (str_to_ll(s, v) != 0) {
    sh_error("%s: integer expression expected", s);
    return -1;
  }
  return 0;
}


static int binary (const char *l, const char *op, const char *r, int arith, int *err) {
  long long a, b;
  OsStat sa, sb;
  *err = 0;
  if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) return strcmp(l, r) == 0;
  if (strcmp(op, "!=") == 0) return strcmp(l, r) != 0;
  if (strcmp(op, "<") == 0) return strcmp(l, r) < 0;
  if (strcmp(op, ">") == 0) return strcmp(l, r) > 0;
  if (strcmp(op, "-nt") == 0) {
    int ea = file_stat(l, &sa, 0), eb = file_stat(r, &sb, 0);
    return ea && (!eb || sa.mtime > sb.mtime);
  }
  if (strcmp(op, "-ot") == 0) {
    int ea = file_stat(l, &sa, 0), eb = file_stat(r, &sb, 0);
    return eb && (!ea || sa.mtime < sb.mtime);
  }
  if (strcmp(op, "-ef") == 0) {
    char *na, *nb, *ra, *rb;
    int same;
    if (!file_stat(l, &sa, 0) || !file_stat(r, &sb, 0)) return 0;
    if (sa.ino != 0 || sb.ino != 0) return sa.dev == sb.dev && sa.ino == sb.ino;
    na = path_to_native(l);
    nb = path_to_native(r);
    ra = os_realpath(na);
    rb = os_realpath(nb);
    same = ra && rb && m_fncmp(ra, rb) == 0;
    free(na); free(nb); free(ra); free(rb);
    return same;
  }
  if (op[0] == '-' && strlen(op) == 3) {
    if (to_int(l, &a, arith) != 0 || to_int(r, &b, arith) != 0) {
      *err = 1;
      return 0;
    }
    if (strcmp(op, "-eq") == 0) return a == b;
    if (strcmp(op, "-ne") == 0) return a != b;
    if (strcmp(op, "-lt") == 0) return a < b;
    if (strcmp(op, "-le") == 0) return a <= b;
    if (strcmp(op, "-gt") == 0) return a > b;
    if (strcmp(op, "-ge") == 0) return a >= b;
  }
  *err = 2;
  return 0;
}


int test_binary (const char *l, const char *op, const char *r, int *err) {
  return binary(l, op, r, 0, err);
}


static int is_binop (const char *s) {
  static const char *const ops[] = {
    "=", "==", "!=", "<", ">", "-eq", "-ne", "-lt", "-le", "-gt", "-ge",
    "-nt", "-ot", "-ef", NULL
  };
  int i;
  for (i = 0; ops[i]; i++)
    if (strcmp(s, ops[i]) == 0) return 1;
  return 0;
}


/*
** {==================================================================
** test and [
** ===================================================================
*/

typedef struct T {
  char **a;
  int n, i;
  int err;
} T;

static int t_or (T *t);


static int t_primary (T *t) {
  const char *s;
  if (t->i >= t->n) {
    sh_error("test: argument expected");
    t->err = 1;
    return 0;
  }
  s = t->a[t->i];
  if (strcmp(s, "(") == 0 && t->n - t->i >= 3) {
    int r;
    t->i++;
    r = t_or(t);
    if (t->i >= t->n || strcmp(t->a[t->i], ")") != 0) {
      sh_error("test: `)' expected");
      t->err = 1;
      return 0;
    }
    t->i++;
    return r;
  }
  if (t->i + 2 < t->n && is_binop(t->a[t->i + 1])) {
    int e, r = binary(s, t->a[t->i + 1], t->a[t->i + 2], 0, &e);
    t->i += 3;
    if (e) t->err = 1;
    return r;
  }
  if (t->i + 1 < t->n) {
    int r = test_unary(s, t->a[t->i + 1]);
    if (r >= 0 && !(t->i + 2 < t->n && is_binop(t->a[t->i + 2]))) {
      t->i += 2;
      return r;
    }
  }
  t->i++;
  return s[0] != '\0';
}


static int t_not (T *t) {
  if (t->i < t->n && strcmp(t->a[t->i], "!") == 0 && t->i + 1 < t->n) {
    t->i++;
    return !t_not(t);
  }
  return t_primary(t);
}


static int t_and (T *t) {
  int r = t_not(t);
  while (t->i < t->n && strcmp(t->a[t->i], "-a") == 0) {
    int r2;
    t->i++;
    r2 = t_not(t);
    r = r && r2;
  }
  return r;
}


static int t_or (T *t) {
  int r = t_and(t);
  while (t->i < t->n && strcmp(t->a[t->i], "-o") == 0) {
    int r2;
    t->i++;
    r2 = t_and(t);
    r = r || r2;
  }
  return r;
}


/* POSIX: the number of arguments decides first */
static int test_args (char **a, int n, int *err) {
  *err = 0;
  switch (n) {
    case 0: return 0;
    case 1: return a[0][0] != '\0';
    case 2:
      if (strcmp(a[0], "!") == 0) return a[1][0] == '\0';
      {
        int r = test_unary(a[0], a[1]);
        if (r < 0) {
          sh_error("test: %s: unary operator expected", a[0]);
          *err = 1;
          return 0;
        }
        return r;
      }
    case 3:
      if (is_binop(a[1])) {
        int e, r = binary(a[0], a[1], a[2], 0, &e);
        if (e) *err = 1;
        return r;
      }
      if (strcmp(a[1], "-a") == 0) return a[0][0] != '\0' && a[2][0] != '\0';
      if (strcmp(a[1], "-o") == 0) return a[0][0] != '\0' || a[2][0] != '\0';
      if (strcmp(a[0], "!") == 0) return !test_args(a + 1, 2, err);
      if (strcmp(a[0], "(") == 0 && strcmp(a[2], ")") == 0) return a[1][0] != '\0';
      sh_error("test: %s: binary operator expected", a[1]);
      *err = 1;
      return 0;
    case 4:
      if (strcmp(a[0], "!") == 0) {
        int r = test_args(a + 1, 3, err);
        return !r;
      }
      if (strcmp(a[0], "(") == 0 && strcmp(a[3], ")") == 0) return test_args(a + 1, 2, err);
      /* fall through */
    default: {
      T t;
      int r;
      t.a = a;
      t.n = n;
      t.i = 0;
      t.err = 0;
      r = t_or(&t);
      if (!t.err && t.i < t.n) {
        sh_error("test: too many arguments");
        t.err = 1;
      }
      *err = t.err;
      return r;
    }
  }
}


int b_test (int argc, char **argv, int in, int out, int err) {
  int e, r;
  (void)in; (void)out; (void)err;
  r = test_args(argv + 1, argc - 1, &e);
  return e ? 2 : !r;
}


int b_bracket (int argc, char **argv, int in, int out, int err) {
  int e, r;
  (void)in; (void)out; (void)err;
  if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
    sh_error("[: missing `]'");
    return 2;
  }
  r = test_args(argv + 1, argc - 2, &e);
  return e ? 2 : !r;
}

/* }================================================================== */


/*
** {==================================================================
** [[ ]]
** ===================================================================
*/

/* a marked pattern (quoted parts literal) as an ERE: quoted -> escaped */
static char *marked_to_regex (const char *m) {
  Buf b;
  buf_init(&b);
  for (; *m; m++) {
    if (*m == QMARK && m[1] != '\0') {
      m++;
      if (strchr("\\^$.|?*+()[]{}", *m) != NULL) buf_putc(&b, '\\');
      buf_putc(&b, *m);
    }
    else buf_putc(&b, *m);
  }
  return buf_take(&b);
}


static int cond_node (Node *n, int *err) {
  switch (n->kind) {
    case C_AND: {
      int l = cond_node(n->a, err);
      if (*err || !l) return 0;
      return cond_node(n->b, err);
    }
    case C_OR: {
      int l = cond_node(n->a, err);
      if (*err) return 0;
      if (l) return 1;
      return cond_node(n->b, err);
    }
    case C_NOT: return !cond_node(n->a, err);
    case C_WORD: {
      char *w = expand_str(n->words[0]);
      int r = w[0] != '\0';
      if (expand_failed()) *err = 1;
      free(w);
      return r;
    }
    case C_UNARY: {
      char *w = expand_str(n->words[0]);
      int r;
      if (expand_failed()) {
        free(w);
        *err = 1;
        return 0;
      }
      r = test_unary(n->str, w);
      free(w);
      if (r < 0) {
        sh_error("[[: %s: unary operator expected", n->str);
        *err = 1;
        return 0;
      }
      return r;
    }
    case C_BINARY: {
      const char *op = n->str;
      char *l = expand_str(n->words[0]);
      int r = 0, e = 0;
      if (expand_failed()) {
        free(l);
        *err = 1;
        return 0;
      }
      if (strcmp(op, "==") == 0 || strcmp(op, "=") == 0 || strcmp(op, "!=") == 0) {
        char *pat = expand_pattern(n->words[1]);
        r = pat_match(pat, l, PM_EXTGLOB);
        if (op[0] == '!') r = !r;
        free(pat);
      }
      else if (strcmp(op, "=~") == 0) {
        /* compat31: quoting the right side does not make it literal */
        char *m = sh_compat() <= 31 ? expand_str(n->words[1]) : expand_pattern(n->words[1]);
        char *re_src = sh_compat() <= 31 ? xstrdup(m) : marked_to_regex(m);
        char *emsg = NULL;
        Regex *re = regex_compile(re_src, O("nocasematch"), &emsg);
        free(m);
        if (re == NULL) {
          sh_error("[[: %s: %s", re_src, emsg ? emsg : "bad regular expression");
          free(emsg);
          free(re_src);
          free(l);
          *err = 1;
          return 0;
        }
        {
          Vec groups;
          size_t i;
          vec_init(&groups);
          r = regex_exec(re, l, &groups);
          var_make_array("BASH_REMATCH", 0);
          if (r)
            for (i = 0; i < groups.n; i++) var_aset("BASH_REMATCH", (long long)i, groups.v[i]);
          vec_free(&groups);
        }
        regex_free(re);
        free(re_src);
      }
      else {
        char *rs = expand_str(n->words[1]);
        r = binary(l, op, rs, 1, &e);
        free(rs);
        if (e) *err = 1;
      }
      free(l);
      return r;
    }
  }
  return 0;
}


int cond_eval (Node *n) {
  int err = 0, r;
  if (O("xtrace")) fd_puts(sh_fd[2] >= 0 ? sh_fd[2] : 2, "+ [[ ... ]]\n");
  r = cond_node(n, &err);
  return err ? 2 : !r;
}

/* }================================================================== */
