/*
** mbuiltin.c - builtin commands and aliases
**
** The table of every builtin is here, with the general ones. The others
** live next to what they work on: mbio.c (echo printf read mapfile),
** mbtest.c (test [ [[), mbvars.c (declare set shopt ...), mjobs.c (jobs
** wait kill trap ...). Builtins write to the descriptors they are given,
** so they work in pipelines and with redirections like any program.
*/

#include "mmc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
** {==================================================================
** Aliases
** ===================================================================
*/

static Vec alias_keys, alias_vals;


static int alias_index (const char *name) {
  size_t i;
  for (i = 0; i < alias_keys.n; i++)
    if (strcmp(alias_keys.v[i], name) == 0) return (int)i;
  return -1;
}


const char *alias_get (const char *name) {
  int i = alias_index(name);
  return (i < 0) ? NULL : alias_vals.v[i];
}


void alias_names (Vec *out) {
  size_t i;
  for (i = 0; i < alias_keys.n; i++) vec_push(out, xstrdup(alias_keys.v[i]));
}


static void alias_set (const char *name, const char *value) {
  int i = alias_index(name);
  if (i >= 0) {
    free(alias_vals.v[i]);
    alias_vals.v[i] = xstrdup(value);
  }
  else {
    vec_push(&alias_keys, xstrdup(name));
    vec_push(&alias_vals, xstrdup(value));
  }
}


static int alias_unset (const char *name) {
  int i = alias_index(name);
  size_t k;
  if (i < 0) return -1;
  free(alias_keys.v[i]);
  free(alias_vals.v[i]);
  for (k = (size_t)i; k + 1 <= alias_keys.n; k++) {	/* moves the NULL too */
    alias_keys.v[k] = alias_keys.v[k + 1];
    alias_vals.v[k] = alias_vals.v[k + 1];
  }
  alias_keys.n--;
  alias_vals.n--;
  return 0;
}


typedef struct AliasSave {
  Vec keys, vals;
} AliasSave;


void *alias_save (void) {
  AliasSave *s = (AliasSave *)xmalloc(sizeof(AliasSave));
  vec_init(&s->keys);
  vec_init(&s->vals);
  vec_copy(&s->keys, alias_keys.v, alias_keys.n);
  vec_copy(&s->vals, alias_vals.v, alias_vals.n);
  return s;
}


void alias_restore (void *saved) {
  AliasSave *s = (AliasSave *)saved;
  vec_free(&alias_keys);
  vec_free(&alias_vals);
  alias_keys = s->keys;
  alias_vals = s->vals;
  free(s);
}


/* prints a value inside single quotes, shell style */
static void put_quoted (Buf *b, const char *s) {
  buf_putc(b, '\'');
  for (; *s; s++) {
    if (*s == '\'') buf_puts(b, "'\\''");
    else buf_putc(b, *s);
  }
  buf_putc(b, '\'');
}


void alias_dump (Buf *b) {
  size_t k;
  for (k = 0; k < alias_keys.n; k++) {
    buf_puts(b, "alias ");
    buf_puts(b, alias_keys.v[k]);
    buf_putc(b, '=');
    put_quoted(b, alias_vals.v[k]);
    buf_putc(b, '\n');
  }
}

/* }================================================================== */


static int b_true (int argc, char **argv, int in, int out, int err) {
  (void)argc; (void)argv; (void)in; (void)out; (void)err;
  return 0;
}


static int b_false (int argc, char **argv, int in, int out, int err) {
  (void)argc; (void)argv; (void)in; (void)out; (void)err;
  return 1;
}


static int b_exit (int argc, char **argv, int in, int out, int err) {
  long long n = sh_status;
  (void)in; (void)out; (void)err;
  if (argc > 1 && str_to_ll(argv[1], &n) != 0) {
    sh_error("exit: %s: numeric argument required", argv[1]);
    n = 2;
  }
  sh_exit = 1;
  sh_status = (int)(n & 0xFF);
  return sh_status;
}


static int b_logout (int argc, char **argv, int in, int out, int err) {
  if (!sh_login) {
    sh_error("logout: not login shell: use `exit'");
    return 1;
  }
  return b_exit(argc, argv, in, out, err);
}


static int b_return (int argc, char **argv, int in, int out, int err) {
  long long n = sh_status;
  (void)in; (void)out; (void)err;
  if (!sh_can_return()) {
    sh_error("return: can only `return' from a function or sourced script");
    return 2;
  }
  if (argc > 1 && arith_eval(argv[1], &n) != 0) {
    sh_error("return: %s: numeric argument required", argv[1]);
    n = 2;
  }
  sh_do_return((int)(n & 0xFF));
  return (int)(n & 0xFF);
}


static int loop_ctl (int argc, char **argv, int is_continue) {
  long long n = 1;
  if (argc > 1 && (str_to_ll(argv[1], &n) != 0 || n < 1)) {
    sh_error("%s: %s: loop count out of range", argv[0], argv[1]);
    return 1;
  }
  if (sh_loop_depth() == 0) {
    sh_error("%s: only meaningful in a `for', `while', or `until' loop", argv[0]);
    return 0;
  }
  sh_do_break((int)n, is_continue);
  return 0;
}


static int b_break (int argc, char **argv, int in, int out, int err) {
  (void)in; (void)out; (void)err;
  return loop_ctl(argc, argv, 0);
}


static int b_continue (int argc, char **argv, int in, int out, int err) {
  (void)in; (void)out; (void)err;
  return loop_ctl(argc, argv, 1);
}


/*
** {==================================================================
** Directories: cd pwd pushd popd dirs
** ===================================================================
*/

static char *cwd_display (void) {
  char *native = os_getcwd();
  char *d = path_to_display(native);
  free(native);
  return d;
}


/* $DIRSTACK: the current directory, then the stack */
static void dirstack_sync (void) {
  char *now = cwd_display();
  size_t i;
  var_make_array("DIRSTACK", 0);
  var_aset("DIRSTACK", 0, now);
  for (i = 0; i < sh_dirstack.n; i++) var_aset("DIRSTACK", (long long)i + 1, sh_dirstack.v[i]);
  free(now);
}


/* changes directory and keeps PWD and OLDPWD; 0 on success */
static int change_dir (const char *target, int quiet) {
  char *native = path_to_native(target);
  char *old = cwd_display();
  if (os_chdir(native) != 0) {
    OsStat st;
    if (!quiet) {
      if (os_stat(native, &st) == 0 && !st.is_dir) sh_error("cd: %s: Not a directory", target);
      else sh_error("cd: %s: No such file or directory", target);
    }
    free(native);
    free(old);
    return -1;
  }
  free(native);
  var_set("OLDPWD", old);
  free(old);
  {
    char *now = cwd_display();
    var_set("PWD", now);
    free(now);
  }
  if (var_flags("DIRSTACK") >= 0) dirstack_sync();
  return 0;
}


static char *home_tilde (const char *p, int longform) {
  const char *home = var_get("HOME");
  size_t n = home ? strlen(home) : 0;
  if (!longform && n > 0 && strncmp(p, home, n) == 0 && (p[n] == '\0' || p[n] == '/'))
    return xstrcat3("~", p + n, "");
  return xstrdup(p);
}


static int b_cd (int argc, char **argv, int in, int out, int err) {
  const char *target;
  int i = 1, print = 0;
  (void)in; (void)err;
  while (i < argc && argv[i][0] == '-' && argv[i][1] != '\0' && strcmp(argv[i], "-") != 0) {
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    i++;	/* -L -P -e -@: the path is always the physical one here */
  }
  if (argc - i > 1) {
    sh_error("cd: too many arguments");
    return 1;
  }
  if (i >= argc) {
    target = var_get("HOME");
    if (target == NULL) {
      sh_error("cd: HOME not set");
      return 1;
    }
  }
  else if (strcmp(argv[i], "-") == 0) {
    target = var_get("OLDPWD");
    if (target == NULL) {
      sh_error("cd: OLDPWD not set");
      return 1;
    }
    print = 1;
  }
  else target = argv[i];
  if (target[0] == '\0') return 0;
  /* CDPATH for names that do not start with / . or .. */
  if (target[0] != '/' && strncmp(target, "./", 2) != 0 && strncmp(target, "../", 3) != 0 &&
      strcmp(target, ".") != 0 && strcmp(target, "..") != 0 &&
#ifdef _WIN32
      !(isalpha((unsigned char)target[0]) && target[1] == ':') &&
#endif
      var_get("CDPATH") != NULL && *var_get("CDPATH") != '\0') {
    Vec dirs;
    size_t k;
    vec_init(&dirs);
    path_list_split(var_get("CDPATH"), &dirs);
    for (k = 0; k < dirs.n; k++) {
      char *cand = xstrcat3(dirs.v[k], "/", target);
      if (change_dir(cand, 1) == 0) {
        char *now = cwd_display();
        fd_printf(out, "%s\n", now);
        free(now);
        free(cand);
        vec_free(&dirs);
        return 0;
      }
      free(cand);
    }
    vec_free(&dirs);
  }
  if (change_dir(target, 0) != 0) return 1;
  if (print) {
    char *now = cwd_display();
    fd_printf(out, "%s\n", now);
    free(now);
  }
  return 0;
}


static int b_pwd (int argc, char **argv, int in, int out, int err) {
  const char *pwd = var_get("PWD");
  char *now = cwd_display();
  int physical = 0, i;
  (void)in; (void)err;
  for (i = 1; i < argc; i++)
    if (strcmp(argv[i], "-P") == 0) physical = 1;
  /* $PWD if it still names where we are */
  if (!physical && pwd != NULL && pwd[0] == '/') {
    char *a = path_to_native(pwd), *b = path_to_native(now);
    int same = m_fncmp(a, b) == 0;
    free(a);
    free(b);
    if (same) {
      fd_printf(out, "%s\n", pwd);
      free(now);
      return 0;
    }
  }
  fd_printf(out, "%s\n", now);
  free(now);
  return 0;
}


static void dirs_print (int out, int longform, int vertical, int numbered) {
  size_t i;
  char *now = cwd_display();
  Vec all;
  dirstack_sync();
  vec_init(&all);
  vec_push(&all, now);
  vec_copy(&all, sh_dirstack.v, sh_dirstack.n);
  for (i = 0; i < all.n; i++) {
    char *s = home_tilde(all.v[i], longform);
    if (numbered) fd_printf(out, "%2lu  %s\n", (unsigned long)i, s);
    else if (vertical) fd_printf(out, "%s\n", s);
    else fd_printf(out, "%s%s", i ? " " : "", s);
    free(s);
  }
  if (!vertical && !numbered) fd_puts(out, "\n");
  vec_free(&all);
}


static int b_dirs (int argc, char **argv, int in, int out, int err) {
  int i, longform = 0, vertical = 0, numbered = 0;
  (void)in; (void)err;
  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (a[0] == '-' && strchr(a, 'c')) {
      vec_free(&sh_dirstack);
      vec_init(&sh_dirstack);
      return 0;
    }
    if (a[0] == '-' && strchr(a, 'l')) longform = 1;
    if (a[0] == '-' && strchr(a, 'p')) vertical = 1;
    if (a[0] == '-' && strchr(a, 'v')) numbered = 1;
    if ((a[0] == '+' || a[0] == '-') && isdigit((unsigned char)a[1])) {
      long n = atol(a + 1);
      size_t total = sh_dirstack.n + 1, k;
      char *s;
      if ((size_t)n >= total) {
        sh_error("dirs: %s: directory stack index out of range", a);
        return 1;
      }
      k = (a[0] == '+') ? (size_t)n : total - 1 - (size_t)n;
      if (k == 0) {
        char *now = cwd_display();
        s = home_tilde(now, longform);
        free(now);
      }
      else s = home_tilde(sh_dirstack.v[k - 1], longform);
      fd_printf(out, "%s\n", s);
      free(s);
      return 0;
    }
  }
  dirs_print(out, longform, vertical, numbered);
  return 0;
}


static int b_pushd (int argc, char **argv, int in, int out, int err) {
  int i = 1, noswitch = 0;
  (void)in; (void)err;
  if (i < argc && strcmp(argv[i], "-n") == 0) {
    noswitch = 1;
    i++;
  }
  if (i >= argc) {	/* swap the top two */
    char *now, *top;
    if (sh_dirstack.n == 0) {
      sh_error("pushd: no other directory");
      return 1;
    }
    now = cwd_display();
    top = xstrdup(sh_dirstack.v[0]);
    if (change_dir(top, 0) != 0) {
      free(now);
      free(top);
      return 1;
    }
    free(sh_dirstack.v[0]);
    sh_dirstack.v[0] = now;
    free(top);
    dirs_print(out, 0, 0, 0);
    return 0;
  }
  if ((argv[i][0] == '+' || argv[i][0] == '-') && isdigit((unsigned char)argv[i][1])) {
    long n = atol(argv[i] + 1);	/* rotate the stack */
    Vec all;
    size_t total, k, j;
    vec_init(&all);
    vec_push(&all, cwd_display());
    vec_copy(&all, sh_dirstack.v, sh_dirstack.n);
    total = all.n;
    if ((size_t)n >= total) {
      sh_error("pushd: %s: directory stack index out of range", argv[i]);
      vec_free(&all);
      return 1;
    }
    k = (argv[i][0] == '+') ? (size_t)n : total - 1 - (size_t)n;
    if (change_dir(all.v[k], 0) != 0) {
      vec_free(&all);
      return 1;
    }
    vec_free(&sh_dirstack);
    vec_init(&sh_dirstack);
    for (j = 1; j < total; j++) vec_push(&sh_dirstack, xstrdup(all.v[(k + j) % total]));
    vec_free(&all);
    dirs_print(out, 0, 0, 0);
    return 0;
  }
  {
    char *now = cwd_display();
    if (!noswitch) {
      if (change_dir(argv[i], 0) != 0) {
        free(now);
        return 1;
      }
      vec_insert(&sh_dirstack, 0, now);
    }
    else {
      char *native = path_to_native(argv[i]);
      char *d = path_to_display(native);
      vec_insert(&sh_dirstack, 0, d);
      free(native);
      free(now);
    }
  }
  dirs_print(out, 0, 0, 0);
  return 0;
}


static int b_popd (int argc, char **argv, int in, int out, int err) {
  int i = 1, noswitch = 0;
  (void)in; (void)err;
  if (i < argc && strcmp(argv[i], "-n") == 0) {
    noswitch = 1;
    i++;
  }
  if (sh_dirstack.n == 0) {
    sh_error("popd: directory stack empty");
    return 1;
  }
  if (i < argc && (argv[i][0] == '+' || argv[i][0] == '-') &&
      isdigit((unsigned char)argv[i][1])) {
    long n = atol(argv[i] + 1);
    size_t total = sh_dirstack.n + 1, k;
    if ((size_t)n >= total) {
      sh_error("popd: %s: directory stack index out of range", argv[i]);
      return 1;
    }
    k = (argv[i][0] == '+') ? (size_t)n : total - 1 - (size_t)n;
    if (k == 0) {
      if (change_dir(sh_dirstack.v[0], 0) != 0) return 1;
      k = 1;
    }
    free(sh_dirstack.v[k - 1]);
    memmove(sh_dirstack.v + k - 1, sh_dirstack.v + k, (sh_dirstack.n - k + 1) * sizeof(char *));
    sh_dirstack.n--;
    dirs_print(out, 0, 0, 0);
    return 0;
  }
  if (!noswitch && change_dir(sh_dirstack.v[0], 0) != 0) return 1;
  free(sh_dirstack.v[0]);
  memmove(sh_dirstack.v, sh_dirstack.v + 1, sh_dirstack.n * sizeof(char *));
  sh_dirstack.n--;
  dirs_print(out, 0, 0, 0);
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** source, eval, command, builtin, type, hash
** ===================================================================
*/

static int b_source (int argc, char **argv, int in, int out, int err) {
  char *native = NULL;
  const char *name;
  int status;
  (void)in; (void)out; (void)err;
  if (argc < 2) {
    sh_error("%s: filename argument required", argv[0]);
    return 2;
  }
  name = argv[1];
  if (strcmp(name, "--") == 0 && argc > 2) {
    name = argv[2];
    argv++;
    argc--;
  }
  if (strchr(name, '/') == NULL && O("sourcepath")) {	/* look in PATH first */
    Vec dirs;
    size_t k;
    const char *path = var_get("PATH");
    vec_init(&dirs);
    if (path) path_list_split(path, &dirs);
    for (k = 0; k < dirs.n && native == NULL; k++) {
      char *d = path_to_native(dirs.v[k]);
      char *cand = path_join(d, name);
      OsStat st;
      if (os_stat(cand, &st) == 0 && !st.is_dir) native = cand;
      else free(cand);
      free(d);
    }
    vec_free(&dirs);
  }
  if (native == NULL) native = path_to_native(name);
  status = sh_source(native, 1, argv + 2, argc - 2);
  free(native);
  return status;
}


static int b_eval (int argc, char **argv, int in, int out, int err) {
  Buf b;
  int i, status;
  (void)in; (void)out; (void)err;
  buf_init(&b);
  for (i = 1; i < argc; i++) {
    if (i > 1) buf_putc(&b, ' ');
    buf_puts(&b, argv[i]);
  }
  if (b.len == 0) {
    buf_free(&b);
    return 0;
  }
  status = sh_run_string(b.s, "eval", sh_lineno);
  buf_free(&b);
  return status;
}


static int b_exec_stub (int argc, char **argv, int in, int out, int err) {
  (void)in; (void)out; (void)err;
  return sh_eval_argv(argc - 1, argv + 1, 0, 1, 2, EX_NOFUNC);
}


/* what a name is, for type and command -v */
enum { K_NONE, K_ALIAS, K_KEYWORD, K_FUNCTION, K_BUILTIN, K_FILE };

static int kind_of (const char *name, int skip_funcs, char **path) {
  *path = NULL;
  if (!skip_funcs && alias_get(name) != NULL) return K_ALIAS;
  if (!skip_funcs && parse_is_keyword(name)) return K_KEYWORD;
  if (!skip_funcs && func_find(name) != NULL) return K_FUNCTION;
  if (builtin_find(name, 0) != NULL && builtin_enabled(name)) return K_BUILTIN;
  if ((*path = sh_find_command(name)) != NULL) return K_FILE;
  if (builtin_find(name, 1) != NULL) return K_BUILTIN;
  return K_NONE;
}


static int b_command (int argc, char **argv, int in, int out, int err) {
  int i = 1, show = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    if (strchr(argv[i], 'v')) show = 'v';
    if (strchr(argv[i], 'V')) show = 'V';
  }
  if (i >= argc) return 0;
  if (show) {
    int status = 0;
    for (; i < argc; i++) {
      char *path;
      int k = kind_of(argv[i], 0, &path);
      if (k == K_NONE) {
        if (show == 'V') sh_error("command: %s: not found", argv[i]);
        status = 1;
      }
      else if (show == 'v') {
        if (k == K_ALIAS) {
          Buf b;
          buf_init(&b);
          put_quoted(&b, alias_get(argv[i]));
          fd_printf(out, "alias %s=%s\n", argv[i], b.s);
          buf_free(&b);
        }
        else if (k == K_FILE) {
          char *d = path_to_display(path);
          fd_printf(out, "%s\n", d);
          free(d);
        }
        else fd_printf(out, "%s\n", argv[i]);
      }
      else {
        if (k == K_ALIAS) fd_printf(out, "%s is aliased to `%s'\n", argv[i], alias_get(argv[i]));
        else if (k == K_KEYWORD) fd_printf(out, "%s is a shell keyword\n", argv[i]);
        else if (k == K_FUNCTION) fd_printf(out, "%s is a function\n", argv[i]);
        else if (k == K_BUILTIN) fd_printf(out, "%s is a shell builtin\n", argv[i]);
        else {
          char *d = path_to_display(path);
          fd_printf(out, "%s is %s\n", argv[i], d);
          free(d);
        }
      }
      free(path);
    }
    return status;
  }
  return sh_eval_argv(argc - i, argv + i, in, out, err, EX_NOFUNC);
}


static int b_builtin (int argc, char **argv, int in, int out, int err) {
  if (argc < 2) return 0;
  return sh_eval_argv(argc - 1, argv + 1, in, out, err, EX_BUILTIN_ONLY);
}


static int b_type (int argc, char **argv, int in, int out, int err) {
  int i = 1, terse = 0, path_only = 0, force_path = 0, all = 0, funcs_skip = 0, status = 0;
  (void)in; (void)err;
  for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
    const char *a = argv[i] + 1;
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    for (; *a; a++) {
      if (*a == 't') terse = 1;
      else if (*a == 'p') path_only = 1;
      else if (*a == 'P') force_path = 1;
      else if (*a == 'a') all = 1;
      else if (*a == 'f') funcs_skip = 1;
    }
  }
  for (; i < argc; i++) {
    const char *name = argv[i];
    char *path = NULL;
    int k, found = 0;
    if (force_path) {
      path = sh_find_command(name);
      if (path) {
        char *d = path_to_display(path);
        fd_printf(out, "%s\n", d);
        free(d);
        free(path);
        continue;
      }
      status = 1;
      continue;
    }
    k = kind_of(name, funcs_skip, &path);
    if (all) {	/* every meaning, in order */
      if (!funcs_skip && alias_get(name)) {
        fd_printf(out, terse ? "alias\n" : "%s is aliased to `%s'\n", name, alias_get(name));
        found = 1;
      }
      if (!funcs_skip && parse_is_keyword(name)) {
        fd_printf(out, terse ? "keyword\n" : "%s is a shell keyword\n", name);
        found = 1;
      }
      if (!funcs_skip && func_find(name)) {
        if (terse) fd_puts(out, "function\n");
        else {
          char *pf = func_pretty(func_find(name));
          fd_printf(out, "%s is a function\n%s\n", name, pf);
          free(pf);
        }
        found = 1;
      }
      if (builtin_find(name, 0)) {
        fd_printf(out, terse ? "builtin\n" : "%s is a shell builtin\n", name);
        found = 1;
      }
      {
        Vec dirs;
        size_t d;
        const char *pv = var_get("PATH");
        vec_init(&dirs);
        if (pv) path_list_split(pv, &dirs);
        for (d = 0; d < dirs.n; d++) {
          char *nd = path_to_native(dirs.v[d]);
          char *cand = path_join(nd, name);
          char *hit = NULL;
          free(nd);
          {
            Vec one;
            vec_init(&one);
            free(path);
            path = NULL;
            (void)one;
          }
          {
            OsStat st;
#ifdef _WIN32
            static const char *const ex[] = {"", ".exe", ".com", ".cmd", ".bat", NULL};
            int e;
            for (e = 0; ex[e] && hit == NULL; e++) {
              char *c2 = xstrcat3(cand, ex[e], "");
              if (os_stat(c2, &st) == 0 && !st.is_dir && (e > 0 || os_is_exec(c2))) hit = c2;
              else free(c2);
            }
#else
            if (os_is_exec(cand)) hit = xstrdup(cand);
            (void)st;
#endif
          }
          if (hit) {
            char *disp = path_to_display(hit);
            fd_printf(out, terse ? "file\n" : (path_only ? "%s\n" : "%s is %s\n"),
                      path_only ? disp : name, disp);
            free(disp);
            free(hit);
            found = 1;
          }
          free(cand);
        }
        vec_free(&dirs);
      }
      if (!found) {
        if (!terse && !path_only) sh_error("type: %s: not found", name);
        status = 1;
      }
      free(path);
      continue;
    }
    switch (k) {
      case K_NONE:
        if (!terse && !path_only) sh_error("type: %s: not found", name);
        status = 1;
        break;
      case K_ALIAS:
        if (!path_only) fd_printf(out, terse ? "alias\n" : "%s is aliased to `%s'\n", name, alias_get(name));
        break;
      case K_KEYWORD:
        if (!path_only) fd_printf(out, terse ? "keyword\n" : "%s is a shell keyword\n", name);
        break;
      case K_FUNCTION:
        if (path_only) break;
        if (terse) fd_puts(out, "function\n");
        else {
          char *pf = func_pretty(func_find(name));
          fd_printf(out, "%s is a function\n%s\n", name, pf);
          free(pf);
        }
        break;
      case K_BUILTIN:
        if (!path_only) fd_printf(out, terse ? "builtin\n" : "%s is a shell builtin\n", name);
        break;
      case K_FILE: {
        char *d = path_to_display(path);
        if (terse) fd_puts(out, "file\n");
        else if (path_only) fd_printf(out, "%s\n", d);
        else fd_printf(out, "%s is %s\n", name, d);
        free(d);
        break;
      }
    }
    free(path);
  }
  return status;
}


static int b_hash (int argc, char **argv, int in, int out, int err) {
  int i = 1, status = 0;
  (void)in; (void)err;
  if (argc == 1) {
    sh_hash_list(out);
    return 0;
  }
  for (; i < argc && argv[i][0] == '-'; i++) {
    if (strcmp(argv[i], "-r") == 0) sh_hash_clear();
    else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "-l") == 0 ||
             strcmp(argv[i], "-d") == 0) {
      for (i++; i < argc; i++) {
        char *p = sh_find_command(argv[i]);
        if (p == NULL) {
          sh_error("hash: %s: not found", argv[i]);
          status = 1;
        }
        else {
          char *d = path_to_display(p);
          fd_printf(out, "%s\n", d);
          free(d);
          free(p);
        }
      }
      return status;
    }
    else if (strcmp(argv[i], "-p") == 0) i++;
  }
  for (; i < argc; i++) {
    if (sh_hash_add(argv[i]) != 0) {
      sh_error("hash: %s: not found", argv[i]);
      status = 1;
    }
  }
  return status;
}

/* }================================================================== */


/*
** {==================================================================
** history, fc, enable, caller, alias, completion stubs
** ===================================================================
*/

static int b_history (int argc, char **argv, int in, int out, int err) {
  const Vec *h = line_hist();
  size_t i, from = 0;
  (void)in; (void)err;
  if (argc > 1 && strcmp(argv[1], "-c") == 0) {
    line_hist_clear();
    return 0;
  }
  if (argc > 2 && strcmp(argv[1], "-d") == 0) {
    line_hist_delete(atoi(argv[2]) - 1);
    return 0;
  }
  if (argc > 1 && (strcmp(argv[1], "-w") == 0 || strcmp(argv[1], "-a") == 0)) {
    if (argc > 2) {
      char *native = path_to_native(argv[2]);
      line_hist_write(native);
      free(native);
    }
    return 0;
  }
  if (argc > 1 && (strcmp(argv[1], "-r") == 0 || strcmp(argv[1], "-n") == 0)) return 0;
  if (argc > 1 && isdigit((unsigned char)argv[1][0])) {
    size_t n = (size_t)atol(argv[1]);
    if (n < h->n) from = h->n - n;
  }
  for (i = from; i < h->n; i++)
    fd_printf(out, "%5lu  %s\n", (unsigned long)(i + 1), h->v[i]);
  return 0;
}


static int b_fc (int argc, char **argv, int in, int out, int err) {
  int i;
  for (i = 1; i < argc; i++)
    if (strcmp(argv[i], "-l") == 0) {
      char *a[3];
      a[0] = "history";
      a[1] = NULL;
      return b_history(1, a, in, out, err);
    }
  sh_error("fc: only fc -l is supported (use the Up key to edit history)");
  return 1;
}


static Vec disabled;


int builtin_enabled (const char *name) {
  size_t i;
  for (i = 0; i < disabled.n; i++)
    if (strcmp(disabled.v[i], name) == 0) return 0;
  return 1;
}


static int b_enable (int argc, char **argv, int in, int out, int err) {
  int i = 1, off = 0, status = 0;
  (void)in; (void)err;
  for (; i < argc && argv[i][0] == '-'; i++)
    if (strchr(argv[i], 'n')) off = 1;
  if (i >= argc) {
    Vec names;
    size_t k;
    vec_init(&names);
    builtin_names(&names);
    for (k = 0; k < names.n; k++)
      if (builtin_enabled(names.v[k]) != off) fd_printf(out, "enable %s%s\n", off ? "-n " : "", names.v[k]);
    vec_free(&names);
    return 0;
  }
  for (; i < argc; i++) {
    size_t k;
    if (builtin_find(argv[i], 0) == NULL) {
      sh_error("enable: %s: not a shell builtin", argv[i]);
      status = 1;
      continue;
    }
    for (k = 0; k < disabled.n; k++)
      if (strcmp(disabled.v[k], argv[i]) == 0) {
        free(disabled.v[k]);
        memmove(disabled.v + k, disabled.v + k + 1, (disabled.n - k) * sizeof(char *));
        disabled.n--;
        break;
      }
    if (off) vec_push(&disabled, xstrdup(argv[i]));
  }
  return status;
}


static int b_caller (int argc, char **argv, int in, int out, int err) {
  const char *fn;
  (void)argc; (void)argv; (void)in; (void)err;
  fn = var_aget("FUNCNAME", 1);
  if (var_count("FUNCNAME") == 0) return 1;
  fd_printf(out, "%d %s %s\n", sh_lineno, fn ? fn : "main",
            sh_source_name ? sh_source_name : "main");
  return 0;
}


static int b_alias (int argc, char **argv, int in, int out, int err) {
  int i, status = 0;
  Buf b;
  (void)in; (void)err;
  buf_init(&b);
  if (argc < 2 || (argc == 2 && strcmp(argv[1], "-p") == 0)) alias_dump(&b);
  for (i = 1; i < argc; i++) {
    char *eq = strchr(argv[i], '=');
    if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--") == 0) continue;
    if (eq != NULL && eq != argv[i]) {
      char *name = xstrndup(argv[i], (size_t)(eq - argv[i]));
      alias_set(name, eq + 1);
      free(name);
    }
    else if (alias_get(argv[i]) != NULL) {
      buf_puts(&b, "alias ");
      buf_puts(&b, argv[i]);
      buf_putc(&b, '=');
      put_quoted(&b, alias_get(argv[i]));
      buf_putc(&b, '\n');
    }
    else {
      sh_error("alias: %s: not found", argv[i]);
      status = 1;
    }
  }
  if (b.len) os_write(out, b.s, b.len);
  buf_free(&b);
  return status;
}


static int b_unalias (int argc, char **argv, int in, int out, int err) {
  int i, status = 0;
  (void)in; (void)out; (void)err;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-a") == 0) {
      vec_free(&alias_keys);
      vec_free(&alias_vals);
    }
    else if (alias_unset(argv[i]) != 0) {
      sh_error("unalias: %s: not found", argv[i]);
      status = 1;
    }
  }
  return status;
}


/* complete, compopt and bind: bashrc files call them; nothing to do */
static int b_accept (int argc, char **argv, int in, int out, int err) {
  (void)argc; (void)argv; (void)in; (void)out; (void)err;
  return 0;
}


static int b_compgen (int argc, char **argv, int in, int out, int err) {
  Vec cands, words;
  const char *prefix = "";
  int i;
  size_t k;
  (void)in; (void)err;
  vec_init(&cands);
  vec_init(&words);
  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (a[0] != '-' || a[1] == '\0') {
      prefix = a;
      continue;
    }
    if (strcmp(a, "--") == 0) {
      if (i + 1 < argc) prefix = argv[++i];
      continue;
    }
    if (strcmp(a, "-W") == 0 && i + 1 < argc) {	/* the list, split on blanks */
      const char *s = argv[++i];
      while (*s) {
        size_t n;
        while (*s == ' ' || *s == '\t' || *s == '\n') s++;
        n = strcspn(s, " \t\n");
        if (n > 0) vec_push(&words, xstrndup(s, n));
        s += n;
      }
      continue;
    }
    if (strcmp(a, "-A") == 0 && i + 1 < argc) {
      const char *act = argv[++i];
      if (strcmp(act, "function") == 0) func_names(&cands);
      else if (strcmp(act, "variable") == 0) var_names(&cands, "", 0);
      else if (strcmp(act, "alias") == 0) alias_names(&cands);
      else if (strcmp(act, "builtin") == 0) builtin_names(&cands);
      continue;
    }
    {
      const char *p;
      for (p = a + 1; *p; p++) {
        if (*p == 'a') alias_names(&cands);
        else if (*p == 'b') builtin_names(&cands);
        else if (*p == 'v') var_names(&cands, "", 0);
        else if (*p == 'k') {
          static const char *const kws[] = {"if", "then", "else", "elif", "fi", "case",
            "esac", "for", "select", "while", "until", "do", "done", "in", "function",
            "time", "{", "}", "!", "[[", "]]", "coproc", NULL};
          int j;
          for (j = 0; kws[j]; j++) vec_push(&cands, xstrdup(kws[j]));
        }
        else if (*p == 'f' || *p == 'd') {
          Vec files;
          char *dir = path_to_native(".");
          size_t j;
          vec_init(&files);
          os_listdir(dir, &files);
          for (j = 0; j < files.n; j++) {
            OsStat st;
            char *full = path_join(dir, files.v[j]);
            if (*p == 'f' || (os_stat(full, &st) == 0 && st.is_dir)) vec_push(&cands, xstrdup(files.v[j]));
            free(full);
          }
          vec_free(&files);
          free(dir);
        }
        else if (*p == 'c') {
          builtin_names(&cands);
          alias_names(&cands);
          func_names(&cands);
        }
      }
    }
  }
  vec_sort(&cands);
  {
    int any = 0;
    for (k = 0; k < words.n; k++)	/* -W words keep their order */
      if (strncmp(words.v[k], prefix, strlen(prefix)) == 0) {
        fd_printf(out, "%s\n", words.v[k]);
        any = 1;
      }
    for (k = 0; k < cands.n; k++) {
      if (strncmp(cands.v[k], prefix, strlen(prefix)) != 0) continue;
      if (k > 0 && strcmp(cands.v[k], cands.v[k - 1]) == 0) continue;
      fd_printf(out, "%s\n", cands.v[k]);
      any = 1;
    }
    vec_free(&cands);
    vec_free(&words);
    return any ? 0 : 1;
  }
}

/* }================================================================== */


/*
** {==================================================================
** Fallbacks: small versions of everyday tools, used only when no real
** program with that name is found in PATH
** ===================================================================
*/

static int b_clear (int argc, char **argv, int in, int out, int err) {
  (void)argc; (void)argv; (void)in; (void)err;
  fd_puts(out, "\033[H\033[2J\033[3J");
  return 0;
}


static int b_cat (int argc, char **argv, int in, int out, int err) {
  char chunk[16384];
  int i, status = 0;
  (void)err;
  for (i = 1; i < argc || i == 1; i++) {
    int fd = in, opened = 0;
    long n;
    if (i < argc && strcmp(argv[i], "-") != 0) {
      char *native = path_to_native(argv[i]);
      fd = os_open(native, OS_READ);
      free(native);
      if (fd < 0) {
        sh_error("cat: %s: No such file or directory", argv[i]);
        status = 1;
        continue;
      }
      opened = 1;
    }
    while ((n = os_read(fd, chunk, sizeof(chunk))) > 0)
      if (os_write(out, chunk, (size_t)n) < 0) break;
    if (opened) os_close(fd);
  }
  return status;
}


static int b_mkdir (int argc, char **argv, int in, int out, int err) {
  int i, parents = 0, status = 0;
  (void)in; (void)out; (void)err;
  for (i = 1; i < argc; i++) {
    char *native;
    int r;
    if (strcmp(argv[i], "-p") == 0) {
      parents = 1;
      continue;
    }
    native = path_to_native(argv[i]);
    r = parents ? mkdir_p(native) : os_mkdir(native);
    if (r != 0) {
      sh_error("mkdir: cannot create directory '%s'", argv[i]);
      status = 1;
    }
    free(native);
  }
  return status;
}


static void ls_long (int out, const char *dir, const char *name, int color) {
  char when[32], size[24];
  OsStat st;
  struct tm *tm;
  char *full = dir ? path_join(dir, name) : xstrdup(name);
  os_stat(full, &st);
  free(full);
  tm = localtime(&st.mtime);
  if (tm == NULL || strftime(when, sizeof(when), "%Y-%m-%d %H:%M", tm) == 0)
    strcpy(when, "                ");
  ll_to_str(st.size, size);
  fd_printf(out, "%s %12s %s %s%s%s%s\n", st.is_dir ? "d" : "-", size, when,
            (color && st.is_dir) ? "\033[1;34m" : "", name,
            (color && st.is_dir) ? "\033[0m" : "", st.is_dir ? "/" : "");
}


static void ls_dir (int out, const char *native, int all, int lng) {
  Vec names, shown;
  size_t i, width = 0, cols, rows, r, c;
  int tty = os_is_tty(out);
  vec_init(&names);
  vec_init(&shown);
  os_listdir(native, &names);
  vec_sort(&names);
  for (i = 0; i < names.n; i++) {
    if (names.v[i][0] == '.' && !all) continue;
    if (lng) ls_long(out, native, names.v[i], tty);
    else {
      OsStat st;
      char *full = path_join(native, names.v[i]);
      os_stat(full, &st);
      free(full);
      vec_push(&shown, xstrcat3(names.v[i], st.is_dir ? "/" : "", ""));
    }
  }
  for (i = 0; i < shown.n; i++) {
    size_t w = utf8_count(shown.v[i], strlen(shown.v[i]));
    if (w > width) width = w;
  }
  width += 2;
  cols = tty ? (size_t)os_term_cols() / width : 1;
  if (cols < 1) cols = 1;
  rows = (shown.n + cols - 1) / cols;
  for (r = 0; r < rows; r++) {	/* column-major, like ls */
    Buf line;
    buf_init(&line);
    for (c = 0; c < cols; c++) {
      const char *s;
      size_t n, w;
      int dir;
      if ((i = c * rows + r) >= shown.n) break;
      s = shown.v[i];
      n = strlen(s);
      w = utf8_count(s, n);
      dir = (n > 0 && s[n - 1] == '/');
      if (tty && dir) buf_puts(&line, "\033[1;34m");
      buf_puts(&line, s);
      if (tty && dir) buf_puts(&line, "\033[0m");
      if ((c + 1) * rows + r < shown.n)
        for (; w < width; w++) buf_putc(&line, ' ');
    }
    buf_putc(&line, '\n');
    os_write(out, line.s, line.len);
    buf_free(&line);
  }
  vec_free(&names);
  vec_free(&shown);
}


static int b_ls (int argc, char **argv, int in, int out, int err) {
  Vec paths;
  int i, all = 0, lng = 0, status = 0;
  size_t k;
  (void)in; (void)err;
  vec_init(&paths);
  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (a[0] == '-' && a[1] != '\0') {
      for (a++; *a; a++) {
        if (*a == 'a' || *a == 'A') all = 1;
        else if (*a == 'l') lng = 1;
        else if (*a != '1' && *a != 'h' && *a != 'F') {
          sh_error("ls: unknown option -%c (builtin ls knows -a -l)", *a);
          vec_free(&paths);
          return 2;
        }
      }
    }
    else vec_push(&paths, xstrdup(a));
  }
  if (paths.n == 0) vec_push(&paths, xstrdup("."));
  for (k = 0; k < paths.n; k++) {
    OsStat st;
    char *native = path_to_native(paths.v[k]);
    os_stat(native, &st);
    if (!st.exists) {
      sh_error("ls: %s: No such file or directory", paths.v[k]);
      status = 1;
    }
    else if (!st.is_dir) {
      if (lng) ls_long(out, NULL, native, 0);
      else fd_printf(out, "%s\n", paths.v[k]);
    }
    else {
      if (paths.n > 1) fd_printf(out, "%s%s:\n", k ? "\n" : "", paths.v[k]);
      ls_dir(out, native, all, lng);
    }
    free(native);
  }
  vec_free(&paths);
  return status;
}


/* env [NAME=value ...] [command args]: the fallback when there is no env */
static int b_env (int argc, char **argv, int in, int out, int err) {
  int i = 1;
  (void)in; (void)err;
  while (i < argc && (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "-") == 0 ||
                      strcmp(argv[i], "--") == 0))
    i++;
  if (i < argc && word_assign_pos(argv[i]) > 0) {
    Vec saved_names, cmd;
    int status;
    vec_init(&saved_names);
    for (; i < argc && word_assign_pos(argv[i]) > 0; i++) {
      size_t eq = word_assign_pos(argv[i]);
      char *name = xstrndup(argv[i], eq);
      var_set(name, argv[i] + eq + 1);
      var_set_flags(name, V_EXPORT, 0);
      vec_push(&saved_names, name);
    }
    if (i >= argc) {
      Vec env;
      size_t k;
      vec_init(&env);
      var_env(&env);
      vec_sort(&env);
      for (k = 0; k < env.n; k++) fd_printf(out, "%s\n", env.v[k]);
      vec_free(&env);
      vec_free(&saved_names);
      return 0;
    }
    vec_init(&cmd);
    vec_copy(&cmd, argv + i, (size_t)(argc - i));
    status = sh_eval_argv((int)cmd.n, cmd.v, 0, 1, 2, EX_NOFUNC);
    vec_free(&cmd);
    vec_free(&saved_names);
    return status;
  }
  if (i < argc) return sh_eval_argv(argc - i, argv + i, 0, 1, 2, EX_NOFUNC);
  {
    Vec env;
    size_t k;
    vec_init(&env);
    var_env(&env);
    vec_sort(&env);
    for (k = 0; k < env.n; k++) fd_printf(out, "%s\n", env.v[k]);
    vec_free(&env);
  }
  return 0;
}


/* which: git-bash has /usr/bin/which; this one is for PCs without it */
static int b_which (int argc, char **argv, int in, int out, int err) {
  int i, status = 0;
  (void)in; (void)err;
  for (i = 1; i < argc; i++) {
    char *exe;
    if (argv[i][0] == '-') continue;
    if ((exe = sh_find_command(argv[i])) != NULL) {
      char *shown = path_to_display(exe);
      fd_printf(out, "%s\n", shown);
      free(shown);
      free(exe);
    }
    else {
      sh_error("which: no %s in (%s)", argv[i], var_get("PATH") ? var_get("PATH") : "");
      status = 1;
    }
  }
  return status;
}

/* }================================================================== */


static int b_help (int argc, char **argv, int in, int out, int err);

#define N(name, fn, flags, help)	{name, fn, flags, help}

static const Builtin builtins[] = {
  N(":", b_true, B_SPECIAL, ":                    do nothing, successfully"),
  N(".", b_source, B_SPECIAL, NULL),
  N("[", b_bracket, 0, "[ expr ]             same as test"),
  N("alias", b_alias, 0, "alias [name=value]   define or list aliases"),
  N("bg", b_bg, 0, "bg [job]             (jobs are always running)"),
  N("bind", b_accept, 0, NULL),
  N("break", b_break, B_SPECIAL, "break [n]            leave n loops"),
  N("builtin", b_builtin, 0, "builtin name [args]  run the builtin, not a function"),
  N("caller", b_caller, 0, NULL),
  N("cd", b_cd, 0, "cd [dir|-]           change directory (no dir: home)"),
  N("command", b_command, 0, "command [-vV] name   run name, not a function; -v: what it is"),
  N("compgen", b_compgen, 0, NULL),
  N("complete", b_accept, 0, NULL),
  N("compopt", b_accept, 0, NULL),
  N("continue", b_continue, B_SPECIAL, "continue [n]         next round of the loop"),
  N("declare", b_declare, B_DECL, "declare [-aAilnrux] name[=value]  variables and attributes"),
  N("dirs", b_dirs, 0, "dirs [-clpv]         the directory stack"),
  N("disown", b_disown, 0, NULL),
  N("echo", b_echo, 0, "echo [-neE] ...      print arguments"),
  N("enable", b_enable, 0, NULL),
  N("eval", b_eval, B_SPECIAL, "eval args            run the arguments as a command"),
  N("exec", b_exec_stub, B_SPECIAL, "exec [cmd]           replace the shell; alone: keep redirections"),
  N("exit", b_exit, B_SPECIAL, "exit [n]             leave the shell"),
  N("export", b_export, B_SPECIAL | B_DECL, "export name[=value]  give a variable to programs"),
  N("false", b_false, 0, NULL),
  N("fc", b_fc, 0, NULL),
  N("fg", b_fg, 0, "fg [job]             wait for a background job"),
  N("getopts", b_getopts, 0, "getopts spec name    parse options in a script"),
  N("hash", b_hash, 0, "hash [-r] [name]     remembered program locations"),
  N("help", b_help, 0, "help [name]          this text"),
  N("history", b_history, 0, "history [-c] [n]     show or clear history"),
  N("jobs", b_jobs, 0, "jobs [-lp]           background jobs"),
  N("kill", b_kill, 0, "kill [-sig] pid|%job send a signal"),
  N("let", b_let, 0, "let expr...          arithmetic"),
  N("local", b_local, B_DECL, "local name[=value]   a variable of this function"),
  N("logout", b_logout, 0, NULL),
  N("mapfile", b_mapfile, 0, "mapfile [-t] array   read lines into an array"),
  N("popd", b_popd, 0, "popd                 back to the previous directory"),
  N("printf", b_printf, 0, "printf fmt args      formatted output"),
  N("pushd", b_pushd, 0, "pushd dir            change directory, remember this one"),
  N("pwd", b_pwd, 0, "pwd                  print current directory"),
  N("read", b_read, 0, "read [-r] [-p x] var read a line"),
  N("readarray", b_mapfile, 0, NULL),
  N("readonly", b_readonly, B_SPECIAL | B_DECL, "readonly name[=value]"),
  N("return", b_return, B_SPECIAL, "return [n]           leave a function or sourced file"),
  N("select", b_accept, 0, NULL),
  N("set", b_set, B_SPECIAL, "set [-euxo ...] [--] [args]  options and $1 $2 ..."),
  N("shift", b_shift, B_SPECIAL, "shift [n]            drop $1 .. $n"),
  N("shopt", b_shopt, 0, "shopt [-su] name     shell options"),
  N("source", b_source, 0, "source file [args]   run commands from a file"),
  N("suspend", b_suspend, 0, NULL),
  N("test", b_test, 0, "test expr            file and string tests"),
  N("times", b_times, B_SPECIAL, NULL),
  N("trap", b_trap, B_SPECIAL, "trap 'cmd' SIG...    run cmd on a signal or EXIT"),
  N("true", b_true, 0, NULL),
  N("type", b_type, 0, "type [-atp] name     what a name is"),
  N("typeset", b_declare, B_DECL, NULL),
  N("ulimit", b_ulimit, 0, NULL),
  N("umask", b_umask, 0, "umask [mode]         file creation mask"),
  N("unalias", b_unalias, 0, "unalias name|-a      remove aliases"),
  N("unset", b_unset, B_SPECIAL, "unset [-fv] name     remove a variable or function"),
  N("wait", b_wait, 0, "wait [pid|%job]      wait for background jobs"),
  /* fallbacks */
  N("ls", b_ls, B_FALLBACK, "ls [-a] [-l] [path]  list files"),
  N("cat", b_cat, B_FALLBACK, "cat [file...]        print files"),
  N("clear", b_clear, B_FALLBACK, "clear                clear the screen"),
  N("mkdir", b_mkdir, B_FALLBACK, "mkdir [-p] dir       create directories"),
  N("env", b_env, B_FALLBACK, "env [a=b] [cmd]      print the environment / run cmd"),
  N("which", b_which, B_FALLBACK, "which name           where a program is"),
  {NULL, NULL, 0, NULL}
};


static int b_help (int argc, char **argv, int in, int out, int err) {
  const Builtin *b;
  (void)in; (void)err;
  if (argc > 1) {
    int i, status = 0;
    for (i = 1; i < argc; i++) {
      int found = 0;
      for (b = builtins; b->name; b++)
        if (strcmp(b->name, argv[i]) == 0 && b->help) {
          fd_printf(out, "%s\n", b->help);
          found = 1;
        }
      if (!found) {
        sh_error("help: no help topics match `%s'", argv[i]);
        status = 1;
      }
    }
    return status;
  }
  fd_printf(out, "%s %s - portable shell by %s\n\nBuiltin commands:\n",
            MMC_NAME, MMC_VERSION, MMC_AUTHOR);
  for (b = builtins; b->name; b++)
    if (b->help && !(b->flags & B_FALLBACK)) fd_printf(out, "  %s\n", b->help);
  fd_puts(out, "\nFallbacks (used when no program with that name is in PATH):\n");
  for (b = builtins; b->name; b++)
    if (b->help && (b->flags & B_FALLBACK)) fd_printf(out, "  %s\n", b->help);
  fd_puts(out,
    "\nSyntax (bash): a | b   a |& b   a && b   a || b   a ; b   a &   ( a )   { a; }\n"
    "  if/elif/else/fi  while/until/do/done  for x in ...  for ((i=0;i<3;i++))\n"
    "  case/esac  select  f() { ...; }  [[ ... ]]  (( ... ))  time\n"
    "  > >> < 2> 2>&1 &> <> <<EOF <<< n>&m n<&- {fd}>file\n"
    "  'text' \"text $VAR\" $'\\n' $VAR ${VAR:-x} ${#x} ${x#p} ${x/a/b} ${a[@]}\n"
    "  $(cmd) `cmd` $((1+2)) <(cmd) {a,b} {1..9} ~ * ? [a-z] **  a=(1 2) a[k]=v\n"
    "Paths:   /  is the mmc folder, /d/dir is D:\\dir (Windows), ~ is home\n"
    "Config:  /etc/profile (everyone)   ~/.mmcrc (you)\n"
    "Check:   mmc --check script.sh   (syntax and commands, without running)\n"
    "Keys:    Tab completes, Up/Down history, Ctrl-A/E/K/U/W/L, Ctrl-D exits\n");
  return 0;
}


const Builtin *builtin_find (const char *name, int fallback) {
  const Builtin *b;
  for (b = builtins; b->name; b++)
    if (((b->flags & B_FALLBACK) != 0) == (fallback != 0) && strcmp(b->name, name) == 0)
      return b;
  return NULL;
}


void builtin_names (Vec *out) {
  const Builtin *b;
  for (b = builtins; b->name; b++)
    if (strcmp(b->name, "select") != 0) vec_push(out, xstrdup(b->name));
}
