/*
** mexec.c - running commands
**
** line -> tokens -> alias expansion -> lists (; && || &) -> pipelines.
** In a pipeline every external program is started first, then builtins
** run inside the shell, then the shell waits for the programs.
*/

#include "mmc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


int sh_status = 0;
int sh_exit = 0;
int sh_interactive = 0;
Vec sh_args;


void sh_setvar (const char *name, const char *value) {
  if (m_envcmp(name, "PATH") == 0) {	/* keep PATH tidy and Linux style */
    char *norm = path_list_normalize(value);
    os_setenv("PATH", norm);
    free(norm);
  }
  else os_setenv(name, value);
}


/*
** {==================================================================
** Finding programs
** ===================================================================
*/

#ifdef _WIN32

static const char *const exe_exts[] = {".exe", ".com", ".cmd", ".bat", NULL};


static int has_exe_ext (const char *path) {
  size_t n = strlen(path), i;
  for (i = 0; exe_exts[i]; i++) {
    size_t e = strlen(exe_exts[i]);
    if (n > e && m_stricmp(path + n - e, exe_exts[i]) == 0) return 1;
  }
  return 0;
}


/* name of the interpreter from a "#!" line, or NULL */
static char *script_interp (const char *native) {
  char line[256];
  char *p, *word, *name;
  long n;
  int fd = os_open(native, OS_READ);
  if (fd < 0) return NULL;
  n = os_read(fd, line, sizeof(line) - 1);
  os_close(fd);
  if (n < 3 || line[0] != '#' || line[1] != '!') return NULL;
  line[n] = '\0';
  line[strcspn(line, "\r\n")] = '\0';
  p = line + 2;
  for (;;) {	/* "#!/usr/bin/env python3 -u" -> python3 */
    while (*p == ' ' || *p == '\t') p++;
    word = p;
    while (*p != '\0' && *p != ' ' && *p != '\t') p++;
    if (*p != '\0') *p++ = '\0';
    name = word + strlen(word);
    while (name > word && name[-1] != '/' && name[-1] != '\\') name--;
    if (strcmp(name, "env") != 0 || *p == '\0') break;
  }
  return name[0] ? xstrdup(name) : NULL;
}


static char *try_candidate (const char *base) {
  size_t i;
  char *interp;
  if (has_exe_ext(base)) return os_is_exec(base) ? xstrdup(base) : NULL;
  for (i = 0; exe_exts[i]; i++) {
    char *cand = xstrcat3(base, exe_exts[i], "");
    if (os_is_exec(cand)) return cand;
    free(cand);
  }
  if (os_is_exec(base) && (interp = script_interp(base)) != NULL) {
    free(interp);
    return xstrdup(base);	/* a "#!" script */
  }
  return NULL;
}

#else

static char *try_candidate (const char *base) {
  return os_is_exec(base) ? xstrdup(base) : NULL;
}

#endif


char *sh_find_command (const char *name) {
  Vec dirs;
  size_t i;
  char *path, *found = NULL;
  int has_dir = strchr(name, '/') != NULL;
#ifdef _WIN32
  if (strchr(name, '\\') != NULL || strchr(name, ':') != NULL) has_dir = 1;
#endif
  if (name[0] == '\0') return NULL;
  if (has_dir) {
    char *native = path_to_native(name);
    found = try_candidate(native);
    free(native);
    return found;
  }
  path = os_getenv("PATH");
  vec_init(&dirs);
  if (path) path_list_split(path, &dirs);
  free(path);
  for (i = 0; i < dirs.n && found == NULL; i++) {
    char *dir = path_to_native(dirs.v[i]);
    char *cand = path_join(dir, name);
    found = try_candidate(cand);
    free(dir);
    free(cand);
  }
  vec_free(&dirs);
  return found;
}

/* }================================================================== */


/*
** {==================================================================
** Aliases are expanded on the token level
** ===================================================================
*/

static int is_list_op (int type) {
  return type == T_PIPE || type == T_AND || type == T_OR ||
         type == T_SEMI || type == T_BG;
}


static int is_redirect (int type) {
  return type == T_LT || type == T_GT || type == T_GTGT || type == T_ERR ||
         type == T_ERRAPP || type == T_BOTH;
}


/* is token 'i' the command name of a simple command? */
static int is_command_pos (const TokVec *t, size_t i) {
  while (i > 0) {
    const Token *prev = &t->v[i - 1];
    if (is_list_op(prev->type)) return 1;
    if (prev->type != T_WORD || word_assign_pos(prev->text) == 0) return 0;
    i--;	/* skip "NAME=value" prefixes */
  }
  return 1;
}


static void alias_expand (TokVec *t) {
  size_t i;
  for (i = 0; i < t->n; i++) {
    Token tok = t->v[i];
    const char *value;
    TokVec sub, all;
    size_t k;
    if (tok.type != T_WORD || tok.depth < 0 || tok.depth >= MMC_ALIAS_DEPTH)
      continue;
    if (!is_command_pos(t, i) || (value = alias_get(tok.text)) == NULL)
      continue;
    tok_init(&sub);
    if (lex_line(value, &sub) != 0) {
      tok_free(&sub);
      continue;
    }
    tok_init(&all);
    for (k = 0; k < i; k++)
      tok_push(&all, t->v[k].type, t->v[k].text, t->v[k].depth);
    for (k = 0; k < sub.n; k++) {
      int depth = tok.depth + 1;
      if (k == 0 && sub.v[k].type == T_WORD &&
          strcmp(sub.v[k].text, tok.text) == 0)
        depth = -1;	/* alias ls='ls -F' must not loop */
      tok_push(&all, sub.v[k].type, sub.v[k].text, depth);
    }
    for (k = i + 1; k < t->n; k++)
      tok_push(&all, t->v[k].type, t->v[k].text, t->v[k].depth);
    free(tok.text);
    free(sub.v);	/* the strings moved into 'all' */
    free(t->v);
    *t = all;
    i--;	/* look at the same position again */
  }
}

/* }================================================================== */


/*
** {==================================================================
** Pipelines
** ===================================================================
*/

typedef struct Stage {
  Vec argv;		/* expanded words */
  Vec assigns;		/* expanded NAME=value prefixes */
  int fd[3];
  int own[3];		/* must the shell close fd[k]? */
  int failed;
  const Builtin *builtin;
  char *exe;		/* native path of the program */
  OsProc proc;
  int spawned;
  int status;
} Stage;


static void stage_set_fd (Stage *s, int k, int fd) {
  if (s->own[k]) os_close(s->fd[k]);
  s->fd[k] = fd;
  s->own[k] = 1;
}


static void stage_close (Stage *s) {
  int k;
  for (k = 0; k < 3; k++) {
    if (s->own[k]) os_close(s->fd[k]);
    s->fd[k] = k;
    s->own[k] = 0;
  }
}


static void stage_redirect (Stage *s, int type, const char *raw) {
  char *target, *native;
  int fd;
  if (type == T_ERROUT || type == T_OUTERR) {
    int from = (type == T_ERROUT) ? 1 : 2;
    if ((fd = os_dup(s->fd[from])) >= 0) stage_set_fd(s, 3 - from, fd);
    return;
  }
  target = expand_str(raw);
  native = path_to_native(target);
  fd = os_open(native, type == T_LT ? OS_READ :
               (type == T_GTGT || type == T_ERRAPP) ? OS_APPEND : OS_WRITE);
  if (fd < 0) {
    fd_printf(2, "mmc: %s: cannot open\n", target);
    s->failed = 1;
  }
  else if (type == T_LT) stage_set_fd(s, 0, fd);
  else if (type == T_GT || type == T_GTGT) stage_set_fd(s, 1, fd);
  else if (type == T_ERR || type == T_ERRAPP) stage_set_fd(s, 2, fd);
  else {	/* &> file */
    int copy = os_dup(fd);
    stage_set_fd(s, 1, fd);
    if (copy >= 0) stage_set_fd(s, 2, copy);
  }
  free(target);
  free(native);
}


/* sets NAME=value pairs; 'saved' gets name, old value, name, ... */
static void assigns_apply (const Vec *assigns, Vec *saved) {
  size_t i;
  for (i = 0; i < assigns->n; i++) {
    size_t eq = word_assign_pos(assigns->v[i]);
    char *name = xstrndup(assigns->v[i], eq);
    if (saved) {
      vec_push(saved, xstrdup(name));
      vec_push(saved, os_getenv(name));
    }
    sh_setvar(name, assigns->v[i] + eq + 1);
    free(name);
  }
}


static void assigns_restore (Vec *saved) {
  size_t i;
  for (i = 0; i + 1 < saved->n; i += 2) os_setenv(saved->v[i], saved->v[i + 1]);
  vec_free(saved);
}


/* finds out what the stage runs; returns 0 if there is something to run */
static int stage_resolve (Stage *s) {
  const char *name;
  size_t i;
  if (s->failed) {
    s->status = 1;
    return -1;
  }
  if (s->argv.n == 0) {	/* only assignments and redirections */
    assigns_apply(&s->assigns, NULL);
    s->status = 0;
    return -1;
  }
  name = s->argv.v[0];
  if ((s->builtin = builtin_find(name, 0)) != NULL) return 0;
  if ((s->exe = sh_find_command(name)) == NULL) {
    if ((s->builtin = builtin_find(name, 1)) != NULL) return 0;
    fd_printf(s->fd[2], "mmc: %s: command not found\n", name);
    s->status = 127;
    return -1;
  }
#ifdef _WIN32
  if (!has_exe_ext(s->exe)) {	/* "#!" script: run its interpreter */
    char *interp = script_interp(s->exe);
    char *prog = interp ? sh_find_command(interp) : NULL;
    if (prog == NULL) {
      fd_printf(s->fd[2], "mmc: %s: interpreter '%s' not found in PATH\n", name,
                interp ? interp : "?");
      free(interp);
      s->status = 126;
      return -1;
    }
    free(s->argv.v[0]);
    s->argv.v[0] = s->exe;	/* the script becomes the first argument */
    vec_insert(&s->argv, 0, interp);
    s->exe = prog;
  }
#endif
  for (i = 1; i < s->argv.n; i++) {	/* /d/x -> D:\x for native programs */
    char *conv = path_arg_to_native(s->argv.v[i]);
    free(s->argv.v[i]);
    s->argv.v[i] = conv;
  }
  return 0;
}


static int run_pipeline (const Token *t, size_t n, int background) {
  Stage *st;
  size_t nstages = 1, i, k;
  int status = 0;
  long pid = 0;
  for (i = 0; i < n; i++)
    if (t[i].type == T_PIPE) nstages++;
  st = (Stage *)xmalloc(nstages * sizeof(Stage));
  memset(st, 0, nstages * sizeof(Stage));
  for (k = 0; k < nstages; k++) {
    vec_init(&st[k].argv);
    vec_init(&st[k].assigns);
    st[k].fd[0] = 0;
    st[k].fd[1] = 1;
    st[k].fd[2] = 2;
  }
  for (k = 0; k + 1 < nstages; k++) {
    int p[2];
    if (os_pipe(p) != 0) {
      fd_puts(2, "mmc: cannot create pipe\n");
      st[k].failed = 1;
      continue;
    }
    stage_set_fd(&st[k], 1, p[1]);
    stage_set_fd(&st[k + 1], 0, p[0]);
  }
  for (i = 0, k = 0; i < n; i++) {	/* words and redirections */
    Stage *s = &st[k];
    if (t[i].type == T_PIPE) {
      if (s->argv.n == 0 && s->assigns.n == 0 && !s->failed) {
        fd_puts(2, "mmc: syntax error near '|'\n");
        s->failed = 1;
      }
      k++;
    }
    else if (t[i].type == T_WORD) {
      if (s->argv.n == 0 && word_assign_pos(t[i].text) != 0)
        vec_push(&s->assigns, expand_str(t[i].text));
      else expand_word(t[i].text, &s->argv);
    }
    else if (is_redirect(t[i].type)) {
      if (i + 1 >= n || t[i + 1].type != T_WORD) {
        fd_puts(2, "mmc: syntax error: redirection needs a file name\n");
        s->failed = 1;
      }
      else {
        stage_redirect(s, t[i].type, t[i + 1].text);
        i++;
      }
    }
    else stage_redirect(s, t[i].type, NULL);	/* 2>&1, >&2 */
  }
  for (k = 0; k < nstages; k++) {	/* external programs first */
    Stage *s = &st[k];
    if (stage_resolve(s) == 0 && s->exe != NULL) {
      Vec saved;
      vec_init(&saved);
      assigns_apply(&s->assigns, &saved);
      if (os_spawn(s->exe, s->argv.v, s->fd[0], s->fd[1], s->fd[2],
                   &s->proc, &pid) == 0)
        s->spawned = 1;
      else s->status = 126;
      assigns_restore(&saved);
    }
    if (s->builtin == NULL) stage_close(s);	/* lets readers see EOF */
  }
  for (k = 0; k < nstages; k++) {	/* then builtins, inside the shell */
    Stage *s = &st[k];
    if (s->builtin != NULL) {
      Vec saved;
      vec_init(&saved);
      assigns_apply(&s->assigns, &saved);
      s->status = s->builtin->fn((int)s->argv.n, s->argv.v, s->fd[0],
                                 s->fd[1], s->fd[2]);
      assigns_restore(&saved);
      stage_close(s);
    }
  }
  for (k = 0; k < nstages; k++) {
    Stage *s = &st[k];
    if (s->spawned) {
      if (background) os_detach(s->proc);
      else s->status = os_wait(s->proc);
    }
    status = s->status;
    vec_free(&s->argv);
    vec_free(&s->assigns);
    free(s->exe);
  }
  if (background && pid != 0) {
    fd_printf(2, "[bg] %ld\n", pid);
    status = 0;
  }
  free(st);
  return status;
}

/* }================================================================== */


int sh_run_line (const char *line) {
  TokVec t;
  size_t i = 0;
  int prev = T_SEMI;	/* operator in front of the current pipeline */
  tok_init(&t);
  if (lex_line(line, &t) != 0) {
    tok_free(&t);
    return sh_status = 2;
  }
  alias_expand(&t);
  while (i < t.n && !sh_exit) {
    size_t start = i;
    int op;
    while (i < t.n && (t.v[i].type == T_PIPE || !is_list_op(t.v[i].type))) i++;
    op = (i < t.n) ? t.v[i].type : T_SEMI;
    if (i == start) {
      if (op != T_SEMI || i < t.n) {
        fd_puts(2, "mmc: syntax error: command expected\n");
        sh_status = 2;
        break;
      }
    }
    else if ((prev == T_AND && sh_status != 0) ||
             (prev == T_OR && sh_status == 0))
      ;	/* short-circuit: skip this pipeline, keep the status */
    else sh_status = run_pipeline(t.v + start, i - start, op == T_BG);
    prev = op;
    i++;
  }
  tok_free(&t);
  return sh_status;
}


/* runs a script; 'must_exist' controls the error for a missing file */
int sh_source (const char *native, int must_exist) {
  static int depth = 0;
  char *text, *line;
  if (depth >= MMC_SOURCE_DEPTH) {
    fd_puts(2, "mmc: source: nested too deeply\n");
    return 1;
  }
  text = read_file(native, NULL);
  if (text == NULL) {
    if (must_exist) fd_printf(2, "mmc: %s: cannot open\n", native);
    return must_exist ? 1 : 0;
  }
  depth++;
  line = text;
  if (strncmp(line, "\xEF\xBB\xBF", 3) == 0) line += 3;	/* UTF-8 BOM */
  while (*line != '\0' && !sh_exit) {
    char *end = line + strcspn(line, "\n");
    char save = *end;
    *end = '\0';
    sh_run_line(line);
    if (save == '\0') break;
    line = end + 1;
  }
  depth--;
  free(text);
  return sh_status;
}
