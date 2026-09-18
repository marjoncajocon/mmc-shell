/*
** mbuiltin.c - builtin commands and aliases
**
** Builtins write to the file descriptors they are given, so they work
** in pipelines and with redirections like any other command.
*/

#include "mmc.h"

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


/* prints a value inside single quotes, shell style */
static void put_quoted (Buf *b, const char *s) {
  buf_putc(b, '\'');
  for (; *s; s++) {
    if (*s == '\'') buf_puts(b, "'\\''");
    else buf_putc(b, *s);
  }
  buf_putc(b, '\'');
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
  (void)in; (void)out; (void)err;
  sh_exit = 1;
  return (argc > 1) ? (atoi(argv[1]) & 0xFF) : sh_status;
}


static int b_cd (int argc, char **argv, int in, int out, int err) {
  static char *oldpwd = NULL;
  char *target, *native, *cwd;
  (void)in;
  if (argc > 2) {
    fd_puts(err, "mmc: cd: too many arguments\n");
    return 1;
  }
  if (argc < 2) {
    target = os_getenv("HOME");
    if (target == NULL) {
      fd_puts(err, "mmc: cd: HOME not set\n");
      return 1;
    }
  }
  else if (strcmp(argv[1], "-") == 0) {
    if (oldpwd == NULL) {
      fd_puts(err, "mmc: cd: no previous directory\n");
      return 1;
    }
    target = path_to_display(oldpwd);
    fd_printf(out, "%s\n", target);
  }
  else target = xstrdup(argv[1]);
  native = path_to_native(target);
  cwd = os_getcwd();
  if (os_chdir(native) != 0) {
    fd_printf(err, "mmc: cd: %s: No such directory\n", target);
    free(cwd);
    free(native);
    free(target);
    return 1;
  }
  free(oldpwd);
  oldpwd = cwd;
  free(native);
  free(target);
  return 0;
}


static int b_pwd (int argc, char **argv, int in, int out, int err) {
  char *cwd = os_getcwd();
  char *shown = path_to_display(cwd);
  (void)argc; (void)argv; (void)in; (void)err;
  fd_printf(out, "%s\n", shown);
  free(cwd);
  free(shown);
  return 0;
}


static int b_echo (int argc, char **argv, int in, int out, int err) {
  Buf b;
  int i = 1, newline = 1, escapes = 0;
  (void)in; (void)err;
  for (; i < argc; i++) {
    if (strcmp(argv[i], "-n") == 0) newline = 0;
    else if (strcmp(argv[i], "-e") == 0) escapes = 1;
    else if (strcmp(argv[i], "-ne") == 0 || strcmp(argv[i], "-en") == 0)
      newline = 0, escapes = 1;
    else break;
  }
  buf_init(&b);
  for (; i < argc; i++) {
    const char *s = argv[i];
    for (; *s; s++) {
      if (escapes && *s == '\\' && s[1] != '\0') {
        s++;
        switch (*s) {
          case 'n': buf_putc(&b, '\n'); break;
          case 't': buf_putc(&b, '\t'); break;
          case 'r': buf_putc(&b, '\r'); break;
          case 'a': buf_putc(&b, '\a'); break;
          case 'e': buf_putc(&b, '\033'); break;
          case '\\': buf_putc(&b, '\\'); break;
          default: buf_putc(&b, '\\'); buf_putc(&b, *s); break;
        }
      }
      else buf_putc(&b, *s);
    }
    if (i + 1 < argc) buf_putc(&b, ' ');
  }
  if (newline) buf_putc(&b, '\n');
  os_write(out, b.s ? b.s : "", b.len);
  buf_free(&b);
  return 0;
}


static void list_env (int out, const char *prefix) {
  Vec env;
  size_t i;
  vec_init(&env);
  os_env_list(&env);
  vec_sort(&env);
  for (i = 0; i < env.n; i++) {
    char *eq = strchr(env.v[i], '=');
    if (prefix == NULL || eq == NULL) fd_printf(out, "%s\n", env.v[i]);
    else {
      *eq = '\0';
      fd_printf(out, "%s%s=\"%s\"\n", prefix, env.v[i], eq + 1);
    }
  }
  vec_free(&env);
}


static int b_export (int argc, char **argv, int in, int out, int err) {
  int i, status = 0;
  (void)in;
  if (argc < 2) {
    list_env(out, "declare -x ");
    return 0;
  }
  for (i = 1; i < argc; i++) {
    size_t eq = word_assign_pos(argv[i]);
    if (eq != 0) {
      char *name = xstrndup(argv[i], eq);
      sh_setvar(name, argv[i] + eq + 1);
      free(name);
    }
    else if (strchr(argv[i], '=') != NULL || argv[i][0] == '-') {
      fd_printf(err, "mmc: export: %s: not a valid name\n", argv[i]);
      status = 1;
    }
    /* "export NAME": every mmc variable is already exported */
  }
  return status;
}


static int b_unset (int argc, char **argv, int in, int out, int err) {
  int i;
  (void)in; (void)out; (void)err;
  for (i = 1; i < argc; i++) os_setenv(argv[i], NULL);
  return 0;
}


static int b_env (int argc, char **argv, int in, int out, int err) {
  (void)in;
  if (argc > 1) {
    fd_printf(err, "mmc: env: builtin version takes no arguments (%s)\n",
              argv[1]);
    return 1;
  }
  list_env(out, NULL);
  return 0;
}


static int b_alias (int argc, char **argv, int in, int out, int err) {
  int i, status = 0;
  Buf b;
  (void)in;
  buf_init(&b);
  if (argc < 2) {
    size_t k;
    for (k = 0; k < alias_keys.n; k++) {
      buf_puts(&b, "alias ");
      buf_puts(&b, alias_keys.v[k]);
      buf_putc(&b, '=');
      put_quoted(&b, alias_vals.v[k]);
      buf_putc(&b, '\n');
    }
  }
  for (i = 1; i < argc; i++) {
    char *eq = strchr(argv[i], '=');
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
      fd_printf(err, "mmc: alias: %s: not found\n", argv[i]);
      status = 1;
    }
  }
  if (b.len) os_write(out, b.s, b.len);
  buf_free(&b);
  return status;
}


static int b_unalias (int argc, char **argv, int in, int out, int err) {
  int i, status = 0;
  (void)in; (void)out;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-a") == 0) {
      vec_free(&alias_keys);
      vec_free(&alias_vals);
    }
    else if (alias_unset(argv[i]) != 0) {
      fd_printf(err, "mmc: unalias: %s: not found\n", argv[i]);
      status = 1;
    }
  }
  return status;
}


static int b_source (int argc, char **argv, int in, int out, int err) {
  char *native;
  int status;
  (void)in; (void)out;
  if (argc < 2) {
    fd_puts(err, "mmc: source: file name required\n");
    return 2;
  }
  native = path_to_native(argv[1]);
  status = sh_source(native, 1);
  free(native);
  return status;
}


static int b_which (int argc, char **argv, int in, int out, int err) {
  int i, status = 0;
  (void)in;
  for (i = 1; i < argc; i++) {
    const char *name = argv[i];
    char *exe;
    if (alias_get(name) != NULL)
      fd_printf(out, "%s: aliased to %s\n", name, alias_get(name));
    else if (builtin_find(name, 0) != NULL)
      fd_printf(out, "%s: shell builtin\n", name);
    else if ((exe = sh_find_command(name)) != NULL) {
      char *shown = path_to_display(exe);
      fd_printf(out, "%s\n", shown);
      free(shown);
      free(exe);
    }
    else if (builtin_find(name, 1) != NULL)
      fd_printf(out, "%s: shell builtin (fallback)\n", name);
    else {
      fd_printf(err, "mmc: %s: %s: not found\n", argv[0], name);
      status = 1;
    }
  }
  return status;
}


static int b_history (int argc, char **argv, int in, int out, int err) {
  const Vec *h = line_hist();
  size_t i;
  (void)in; (void)err;
  if (argc > 1 && strcmp(argv[1], "-c") == 0) {
    line_hist_clear();
    return 0;
  }
  for (i = 0; i < h->n; i++)
    fd_printf(out, "%5lu  %s\n", (unsigned long)(i + 1), h->v[i]);
  return 0;
}


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
  for (i = 1; i < argc || i == 1; i++) {
    int fd = in, opened = 0;
    long n;
    if (i < argc && strcmp(argv[i], "-") != 0) {
      char *native = path_to_native(argv[i]);
      fd = os_open(native, OS_READ);
      free(native);
      if (fd < 0) {
        fd_printf(err, "mmc: cat: %s: cannot open\n", argv[i]);
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
  (void)in; (void)out;
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
      fd_printf(err, "mmc: mkdir: cannot create '%s'\n", argv[i]);
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
  (void)in;
  vec_init(&paths);
  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (a[0] == '-' && a[1] != '\0') {
      for (a++; *a; a++) {
        if (*a == 'a' || *a == 'A') all = 1;
        else if (*a == 'l') lng = 1;
        else if (*a != '1' && *a != 'h' && *a != 'F') {
          fd_printf(err, "mmc: ls: unknown option -%c (builtin ls knows"
                         " -a -l)\n", *a);
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
      fd_printf(err, "mmc: ls: %s: No such file or directory\n", paths.v[k]);
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

/* }================================================================== */


static int b_help (int argc, char **argv, int in, int out, int err);

static const Builtin builtins[] = {
  {"cd", b_cd, 0, "cd [dir|-]          change directory (no dir: home)"},
  {"pwd", b_pwd, 0, "pwd                 print current directory"},
  {"echo", b_echo, 0, "echo [-n] [-e] ...  print arguments"},
  {"export", b_export, 0, "export NAME=value   set an environment variable"},
  {"unset", b_unset, 0, "unset NAME          remove a variable"},
  {"alias", b_alias, 0, "alias [name=value]  define or list aliases"},
  {"unalias", b_unalias, 0, "unalias name|-a     remove aliases"},
  {"source", b_source, 0, "source file         run commands from a file"},
  {".", b_source, 0, NULL},
  {"which", b_which, 0, "which name          show what a command is"},
  {"type", b_which, 0, NULL},
  {"history", b_history, 0, "history [-c]        show or clear history"},
  {"help", b_help, 0, "help                this text"},
  {"exit", b_exit, 0, "exit [n]            leave the shell"},
  {"true", b_true, 0, NULL},
  {":", b_true, 0, NULL},
  {"false", b_false, 0, NULL},
  {"ls", b_ls, 1, "ls [-a] [-l] [path] list files"},
  {"cat", b_cat, 1, "cat [file...]       print files"},
  {"clear", b_clear, 1, "clear               clear the screen"},
  {"mkdir", b_mkdir, 1, "mkdir [-p] dir      create directories"},
  {"env", b_env, 1, "env                 print the environment"},
  {NULL, NULL, 0, NULL}
};


static int b_help (int argc, char **argv, int in, int out, int err) {
  const Builtin *b;
  (void)argc; (void)argv; (void)in; (void)err;
  fd_printf(out, "%s %s - portable shell by %s\n\nBuiltin commands:\n",
            MMC_NAME, MMC_VERSION, MMC_AUTHOR);
  for (b = builtins; b->name; b++)
    if (b->help && !b->fallback) fd_printf(out, "  %s\n", b->help);
  fd_puts(out, "\nFallbacks (used when no program with that name is in PATH):\n");
  for (b = builtins; b->name; b++)
    if (b->help && b->fallback) fd_printf(out, "  %s\n", b->help);
  fd_puts(out,
    "\nSyntax:  cmd | cmd   a && b   a || b   a ; b   cmd &\n"
    "         > file   >> file   < file   2> file   2>&1   &> file\n"
    "         'text'  \"text $VAR\"  $VAR  ${VAR:-default}  $?  ~  * ? [a-z]\n"
    "         NAME=value   NAME=value cmd\n"
    "Paths:   /  is the mmc folder, /d/dir is D:\\dir (Windows), ~ is home\n"
    "Config:  /etc/profile (everyone)   ~/.mmcrc (you)\n"
    "Keys:    Tab completes, Up/Down history, Ctrl-A/E/K/U/W/L, Ctrl-D exits\n");
  return 0;
}


const Builtin *builtin_find (const char *name, int fallback) {
  const Builtin *b;
  for (b = builtins; b->name; b++)
    if (b->fallback == fallback && strcmp(b->name, name) == 0) return b;
  return NULL;
}


void builtin_names (Vec *out) {
  const Builtin *b;
  for (b = builtins; b->name; b++) vec_push(out, xstrdup(b->name));
}
