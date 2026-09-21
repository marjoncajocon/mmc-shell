/*
** mexec.c - running commands
**
** Walks the syntax tree of mparse.c. There is no fork() on Windows, so
** one design serves every system:
**   ( ) and $( )    run inside the shell, after a copy of its state is
**                   taken; the copy is put back afterwards
**   pipelines       programs run side by side; shell code in the middle
**                   of a pipeline runs in a child mmc, builtins there run
**                   in the shell with their output kept and fed on; the
**                   last stage runs in the shell (so "... | while read"
**                   keeps its variables, like zsh and ksh)
**   cmd &           programs are started and left running; shell code
**                   runs in a child mmc
*/

#include "mmc.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


int sh_status = 0;
int sh_exit = 0;
int sh_interactive = 0;
int sh_login = 0;
int sh_lineno = 0;
int sh_subshell = 0;
long sh_pid = 0;
long sh_last_bg = 0;
Vec sh_pos;
const char *sh_source_name = NULL;
int sh_fd[MMC_FDS];
Vec sh_dirstack;

static int sh_own[MMC_FDS];	/* the shell opened sh_fd[k] and must close it */
static int brk = 0, cont_ = 0, returning = 0;	/* unwinding */
static int loop_depth = 0, func_depth = 0, return_frames = 0;
static int no_errexit = 0;	/* > 0: in a condition, set -e is off */
static int last_subst_status = 0;
static int stage_counter = 0;
static char *traps[TRAP_MAX];
static int in_trap = 0;
static int exit_trap_done = 0;

static int exec_node (Node *n);
int sh_dash_c = 0;


/*
** {==================================================================
** Options: set -o and shopt
** ===================================================================
*/

ShOpt sh_opts[] = {
  /* set -o */
  {"allexport", 'a', 0, 0}, {"braceexpand", 'B', 0, 1}, {"emacs", 0, 0, 1},
  {"errexit", 'e', 0, 0}, {"errtrace", 'E', 0, 0}, {"functrace", 'T', 0, 0},
  {"hashall", 'h', 0, 1}, {"histexpand", 'H', 0, 0}, {"history", 0, 0, 1},
  {"ignoreeof", 0, 0, 0}, {"interactive-comments", 0, 0, 1}, {"keyword", 'k', 0, 0},
  {"monitor", 'm', 0, 0}, {"noclobber", 'C', 0, 0}, {"noexec", 'n', 0, 0},
  {"noglob", 'f', 0, 0}, {"nolog", 0, 0, 0}, {"notify", 'b', 0, 0},
  {"nounset", 'u', 0, 0}, {"onecmd", 't', 0, 0}, {"physical", 'P', 0, 0},
  {"pipefail", 0, 0, 0}, {"posix", 0, 0, 0}, {"privileged", 'p', 0, 0},
  {"verbose", 'v', 0, 0}, {"vi", 0, 0, 0}, {"xtrace", 'x', 0, 0},
  /* shopt */
  {"assoc_expand_once", 0, 1, 0}, {"autocd", 0, 1, 0}, {"cdable_vars", 0, 1, 0},
  {"cdspell", 0, 1, 0}, {"checkhash", 0, 1, 0}, {"checkjobs", 0, 1, 0},
  {"checkwinsize", 0, 1, 1}, {"cmdhist", 0, 1, 1}, {"compat31", 0, 1, 0},
  {"compat32", 0, 1, 0}, {"compat40", 0, 1, 0}, {"compat41", 0, 1, 0},
  {"compat42", 0, 1, 0}, {"compat43", 0, 1, 0}, {"compat44", 0, 1, 0},
  {"complete_fullquote", 0, 1, 1}, {"direxpand", 0, 1, 0}, {"dirspell", 0, 1, 0},
  {"dotglob", 0, 1, 0}, {"execfail", 0, 1, 0}, {"expand_aliases", 0, 1, 1},
  {"extdebug", 0, 1, 0}, {"extglob", 0, 1, 0}, {"extquote", 0, 1, 1},
  {"failglob", 0, 1, 0}, {"force_fignore", 0, 1, 1}, {"globasciiranges", 0, 1, 1},
  {"globskipdots", 0, 1, 1}, {"globstar", 0, 1, 0}, {"gnu_errfmt", 0, 1, 0},
  {"histappend", 0, 1, 0}, {"histreedit", 0, 1, 0}, {"histverify", 0, 1, 0},
  {"hostcomplete", 0, 1, 1}, {"huponexit", 0, 1, 0}, {"inherit_errexit", 0, 1, 0},
  {"interactive_comments", 0, 1, 1}, {"lastpipe", 0, 1, 0}, {"lithist", 0, 1, 0},
  {"localvar_inherit", 0, 1, 0}, {"localvar_unset", 0, 1, 0}, {"login_shell", 0, 1, 0},
  {"mailwarn", 0, 1, 0}, {"no_empty_cmd_completion", 0, 1, 0}, {"nocaseglob", 0, 1, 0},
  {"nocasematch", 0, 1, 0}, {"noexpand_translation", 0, 1, 0}, {"nullglob", 0, 1, 0},
  {"patsub_replacement", 0, 1, 1}, {"progcomp", 0, 1, 1}, {"progcomp_alias", 0, 1, 0},
  {"promptvars", 0, 1, 1}, {"restricted_shell", 0, 1, 0}, {"shift_verbose", 0, 1, 0},
  {"sourcepath", 0, 1, 1}, {"varredir_close", 0, 1, 0}, {"xpg_echo", 0, 1, 0},
  {NULL, 0, 0, 0}
};


static ShOpt *opt_find (const char *name) {
  ShOpt *o;
  for (o = sh_opts; o->name; o++)
    if (strcmp(o->name, name) == 0) return o;
  return NULL;
}


int opt_get (const char *name) {
  ShOpt *o = opt_find(name);
  return o ? o->value : 0;
}


int opt_set (const char *name, int on) {
  ShOpt *o = opt_find(name);
  if (o == NULL) return -1;
  o->value = on;
  if (strcmp(name, "vi") == 0 && on) opt_find("emacs")->value = 0;
  if (strcmp(name, "emacs") == 0 && on) opt_find("vi")->value = 0;
  return 0;
}


int opt_letter (char c, int on) {
  ShOpt *o;
  for (o = sh_opts; o->name; o++) {
    if (o->letter == c && !o->shopt) {
      o->value = on;
      return 0;
    }
  }
  return -1;
}


char *opt_flags (void) {
  static const char order[] = "abefhkmnptuvxBCEHPT";
  Buf b;
  const char *p;
  buf_init(&b);
  for (p = order; *p; p++) {
    ShOpt *o;
    for (o = sh_opts; o->name; o++)
      if (o->letter == *p && !o->shopt && o->value) buf_putc(&b, *p);
  }
  if (sh_interactive) buf_putc(&b, 'i');
  if (sh_dash_c) buf_putc(&b, 'c');	/* started with -c, like bash */
  return buf_take(&b);
}


/*
** The compatibility level (bash's "shell compatibility mode"): one of
** shopt compat31 ... compat44 at most is on, or BASH_COMPAT holds 4.2,
** 42, 50 ... The default is the bash version mmc follows (5.2: 52).
*/

static int compat_default (void) {
  const char *v = MMC_BASH_COMPAT;
  return (v[0] - '0') * 10 + (v[2] - '0');
}


/* "4.2" or "42" -> 42; 0 for nothing, -1 for a level there is not */
static int compat_parse (const char *s) {
  int lvl;
  if (s == NULL || s[0] == '\0') return 0;
  if (isdigit((unsigned char)s[0]) && s[1] == '.' && isdigit((unsigned char)s[2]) && s[3] == '\0')
    lvl = (s[0] - '0') * 10 + (s[2] - '0');
  else if (isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[1]) && s[2] == '\0')
    lvl = (s[0] - '0') * 10 + (s[1] - '0');
  else return -1;
  if (lvl == 31 || lvl == 32 || (lvl >= 40 && lvl <= 44) || (lvl >= 50 && lvl <= compat_default()))
    return lvl;
  return -1;
}


int sh_compat (void) {
  const ShOpt *o;
  int lvl;
  for (o = sh_opts; o->name; o++)
    if (o->value && o->shopt && strncmp(o->name, "compat", 6) == 0) return atoi(o->name + 6);
  lvl = compat_parse(var_get("BASH_COMPAT"));
  return lvl > 0 ? lvl : compat_default();
}


/* BASH_COMPAT was set (or unset: NULL): the compatNN options follow it */
void sh_compat_var (const char *value) {
  ShOpt *o;
  int lvl = compat_parse(value);
  if (lvl < 0) sh_error("BASH_COMPAT: %s: compatibility value out of range", value);
  for (o = sh_opts; o->name; o++)
    if (o->shopt && strncmp(o->name, "compat", 6) == 0) o->value = atoi(o->name + 6) == lvl;
}


/* shopt -s compat42: the others go off; BASH_COMPAT follows */
void opt_compat (const char *name, int on) {
  ShOpt *o = opt_find(name), *p;
  char num[24];
  int lvl = compat_default();
  if (o == NULL) return;
  if (on)
    for (p = sh_opts; p->name; p++)
      if (p->shopt && strncmp(p->name, "compat", 6) == 0) p->value = 0;
  o->value = on;
  for (p = sh_opts; p->name; p++)	/* what is still on, else the default */
    if (p->value && p->shopt && strncmp(p->name, "compat", 6) == 0) lvl = atoi(p->name + 6);
  var_set("BASH_COMPAT", ll_to_str(lvl, num));
}

/* }================================================================== */


/*
** {==================================================================
** Messages
** ===================================================================
*/

void sh_error (const char *fmt, ...) {
  char msg[1024];
  va_list ap;
  int fd = sh_fd[2] >= 0 ? sh_fd[2] : 2;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  if (!sh_interactive && sh_source_name != NULL) {
    /* gnu_errfmt: "file:3: msg"; bash keeps "line 3" for "cd: ..." and
    ** the other messages of builtins */
    const char *colon = strstr(msg, ": ");
    int gnu = O("gnu_errfmt");
    if (gnu && colon != NULL) {
      char *head = xstrndup(msg, (size_t)(colon - msg));
      if (builtin_find(head, 0) != NULL) gnu = 0;
      free(head);
    }
    fd_printf(fd, gnu ? "%s:%d: %s\n" : "%s: line %d: %s\n", sh_source_name, sh_lineno, msg);
  }
  else fd_printf(fd, "mmc: %s\n", msg);
}


void sh_set_lineno (int line) {
  sh_lineno = line;
}


void sh_setvar (const char *name, const char *value) {
  var_set(name, value);
}

/* }================================================================== */


/*
** {==================================================================
** Finding programs, with a hash table like bash's
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


/* name of the interpreter from a "#!" line, or NULL; *arg: its option */
static char *script_interp (const char *native, char **arg) {
  char line[256];
  char *p, *word, *name;
  long n;
  int fd = os_open(native, OS_READ);
  if (arg) *arg = NULL;
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
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '-') {	/* env -S ... */
      while (*p != '\0' && *p != ' ') p++;
    }
  }
  while (*p == ' ' || *p == '\t') p++;
  if (arg && *p) *arg = xstrdup(p);
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
  if (os_is_exec(base) && (interp = script_interp(base, NULL)) != NULL) {
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


typedef struct HashEnt {
  char *name, *path;
  int hits;
} HashEnt;

static HashEnt *hashtab = NULL;
static size_t nhash = 0;


void sh_hash_clear (void) {
  size_t i;
  for (i = 0; i < nhash; i++) {
    free(hashtab[i].name);
    free(hashtab[i].path);
  }
  free(hashtab);
  hashtab = NULL;
  nhash = 0;
}


static HashEnt *hash_get (const char *name) {
  size_t i;
  for (i = 0; i < nhash; i++)
    if (strcmp(hashtab[i].name, name) == 0) return &hashtab[i];
  return NULL;
}


static void hash_put (const char *name, const char *path) {
  HashEnt *h = hash_get(name);
  if (h == NULL) {
    hashtab = (HashEnt *)xrealloc(hashtab, (nhash + 1) * sizeof(HashEnt));
    h = &hashtab[nhash++];
    h->name = xstrdup(name);
    h->path = NULL;
    h->hits = 0;
  }
  free(h->path);
  h->path = xstrdup(path);
}


void sh_hash_list (int fd) {
  size_t i;
  if (nhash == 0) {
    fd_puts(fd, "hash: hash table empty\n");
    return;
  }
  fd_puts(fd, "hits\tcommand\n");
  for (i = 0; i < nhash; i++) {
    char *shown = path_to_display(hashtab[i].path);
    fd_printf(fd, "%4d\t%s\n", hashtab[i].hits, shown);
    free(shown);
  }
}


static char *search_path (const char *name) {
  Vec dirs;
  size_t i;
  const char *path = var_get("PATH");
  char *found = NULL;
  vec_init(&dirs);
  if (path) path_list_split(path, &dirs);
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


int sh_hash_add (const char *name) {
  char *p = search_path(name);
  if (p == NULL) return -1;
  hash_put(name, p);
  free(p);
  return 0;
}


char *sh_find_command (const char *name) {
  HashEnt *h;
  char *found;
  int has_dir = strchr(name, '/') != NULL;
#ifdef _WIN32
  if (strchr(name, '\\') != NULL || strchr(name, ':') != NULL) has_dir = 1;
#endif
  if (name[0] == '\0') return NULL;
  if (has_dir) {
    char *native = path_to_native(name);
    static const char *const std_dirs[] = {
      "/bin/", "/usr/bin/", "/usr/local/bin/", "/sbin/", "/usr/sbin/", NULL
    };
    int k;
    found = try_candidate(native);
    free(native);
    /* /bin/rm, /usr/bin/env ... that this system does not have there:
    ** the program of that name from PATH (git-bash's usr/bin on Windows) */
    for (k = 0; found == NULL && std_dirs[k] != NULL; k++) {
      size_t n = strlen(std_dirs[k]);
      if (strncmp(name, std_dirs[k], n) == 0 && strchr(name + n, '/') == NULL && name[n])
        found = search_path(name + n);
    }
    return found;
  }
  if ((h = hash_get(name)) != NULL) {
    OsStat st;
    if (os_stat(h->path, &st) == 0 && !st.is_dir) {
      h->hits++;
      return xstrdup(h->path);
    }
  }
  found = search_path(name);
  if (found != NULL && O("hashall")) {
    hash_put(name, found);
    hash_get(name)->hits = 1;
  }
  return found;
}

/* }================================================================== */


/*
** {==================================================================
** File descriptors and redirections
** ===================================================================
*/

typedef struct FdSave {
  int k, fd, own;
} FdSave;

typedef struct Saves {
  FdSave v[32];
  int n;
  OsThread *feeders[8];
  int nfeeders;
} Saves;


static void close_if_unused (int fd) {
  int j;
  for (j = 0; j < MMC_FDS; j++)
    if (sh_fd[j] == fd) return;
  os_close(fd);
}


/* shell fd k becomes OS fd 'fd'; with sv the old one is kept to restore */
static void fd_set_k (int k, int fd, int own, Saves *sv) {
  int old = sh_fd[k], old_own = sh_own[k];
  if (sv != NULL && sv->n < (int)(sizeof(sv->v) / sizeof(sv->v[0]))) {
    sv->v[sv->n].k = k;
    sv->v[sv->n].fd = old;
    sv->v[sv->n].own = old_own;
    sv->n++;
    sh_fd[k] = fd;
    sh_own[k] = own;
    return;
  }
  sh_fd[k] = fd;
  sh_own[k] = own;
  if (old_own && old >= 0 && old != fd) close_if_unused(old);
}


static void fd_restore (Saves *sv) {
  int i;
  for (i = sv->n - 1; i >= 0; i--) {
    int k = sv->v[i].k, cur = sh_fd[k], cur_own = sh_own[k];
    sh_fd[k] = sv->v[i].fd;
    sh_own[k] = sv->v[i].own;
    if (cur_own && cur >= 0) close_if_unused(cur);
  }
  sv->n = 0;
  for (i = 0; i < sv->nfeeders; i++) os_thread_join(sv->feeders[i]);
  sv->nfeeders = 0;
}


/* a thread that writes a buffer into a pipe, for here-documents */
typedef struct Feed {
  int fd;
  char *data;
  size_t len;
} Feed;


static void feed_main (void *arg) {
  Feed *f = (Feed *)arg;
  if (f->len > 0) os_write(f->fd, f->data, f->len);
  os_close(f->fd);
  free(f->data);
  free(f);
}


/* a read end that delivers 'data' (takes ownership of data) */
static int feed_pipe (char *data, size_t len, OsThread **thread) {
  int p[2];
  Feed *f;
  *thread = NULL;
  if (os_pipe(p) != 0) {
    free(data);
    return -1;
  }
  if (len <= 4096) {	/* fits in the pipe: no thread needed */
    if (len > 0) os_write(p[1], data, len);
    os_close(p[1]);
    free(data);
    return p[0];
  }
  f = (Feed *)xmalloc(sizeof(Feed));
  f->fd = p[1];
  f->data = data;
  f->len = len;
  *thread = os_thread_start(feed_main, f);
  if (*thread == NULL) feed_main(f);
  return p[0];
}


/* one word for a redirection target */
static char *redir_target (const char *raw) {
  Vec w;
  char *r;
  vec_init(&w);
  if (expand_words((char **)&raw, 1, &w) != 0) {
    vec_free(&w);
    return NULL;
  }
  if (w.n != 1) {
    char *shown = expand_str(raw);
    sh_error("%s: ambiguous redirect", shown);
    free(shown);
    vec_free(&w);
    return NULL;
  }
  r = w.v[0];
  w.v[0] = NULL;
  w.n = 0;
  vec_free(&w);
  return r;
}


static int is_digits_n (const char *s, size_t n) {
  size_t i;
  if (n == 0) return 0;
  for (i = 0; i < n; i++)
    if (!isdigit((unsigned char)s[i])) return 0;
  return 1;
}


/* /dev/stdin, /dev/stdout, /dev/stderr, /dev/fd/N: our fd N; else -1 */
int sh_dev_fd (const char *path) {
  if (strcmp(path, "/dev/stdin") == 0) return 0;
  if (strcmp(path, "/dev/stdout") == 0) return 1;
  if (strcmp(path, "/dev/stderr") == 0) return 2;
  if (strncmp(path, "/dev/fd/", 8) == 0 && path[8] != '\0' &&
      is_digits_n(path + 8, strlen(path + 8)) && strlen(path + 8) < 6)
    return atoi(path + 8);
  return -1;
}


/* what fd N holds, read to its end into a temporary file (source <(cmd)) */
char *sh_fd_to_file (int k) {
  char *dir = path_tmpdir(), name[64], *file, buf[16384];
  int fd;
  long n;
  static int counter = 0;
  sprintf(name, "mmc-fd-%ld-%d", os_getpid(), ++counter);
  file = path_join(dir, name);
  free(dir);
  fd = os_open(file, OS_WRITE);
  if (fd < 0) {
    free(file);
    return NULL;
  }
  if (k >= 0 && k < MMC_FDS && sh_fd[k] >= 0)
    while ((n = os_read(sh_fd[k], buf, sizeof(buf))) > 0) os_write(fd, buf, (size_t)n);
  os_close(fd);
  return file;
}


static int open_target (const char *target, int op) {
  char *native;
  int fd, mode, k = sh_dev_fd(target);
  OsStat st;
  if (k >= 0) {	/* one of our own fds: the same file (the real /dev/fd would miss our redirections) */
    if (k >= MMC_FDS || sh_fd[k] < 0 || (fd = os_dup(sh_fd[k])) < 0) {
      sh_error("%s: Bad file descriptor", target);
      return -1;
    }
    return fd;
  }
  native = path_to_native(target);
  if (op == R_IN) mode = OS_READ;
  else if (op == R_RW) mode = OS_RDWR;
  else if (op == R_APPEND || op == R_BOTHAPP) mode = OS_APPEND;
  else {
    mode = OS_WRITE;
    if (op != R_CLOBBER && O("noclobber") && os_stat(native, &st) == 0 &&
        !st.is_dir && !st.is_chr && m_stricmp(native, "NUL") != 0) {
      sh_error("%s: cannot overwrite existing file", target);
      free(native);
      return -1;
    }
  }
  fd = os_open(native, mode);
  if (fd < 0) {
    OsStat s2;
    if (os_stat(native, &s2) == 0 && s2.is_dir) sh_error("%s: Is a directory", target);
    else if (mode == OS_READ) sh_error("%s: No such file or directory", target);
    else sh_error("%s: cannot open for writing", target);
  }
  free(native);
  return fd;
}


/* the value of the variable of {var}> or {arr[i]}> */
static char *fdvar_get (const char *name) {
  char *raw = xstrcat3("${", name, "}"), *v = expand_str(raw);
  free(raw);
  return v;
}


/* applies the redirections; sv NULL: for good (exec) */
static int apply_redirs (Redir *r, Saves *sv) {
  for (; r != NULL; r = r->next) {
    int k = r->fd, fd;
    char *target = NULL;
    OsThread *th = NULL;
    if (k < 0) k = (r->op == R_IN || r->op == R_RW || r->op == R_DUPIN ||
                    r->op == R_HEREDOC || r->op == R_HERESTR) ? 0 : 1;
    if (r->fdvar != NULL && !((r->op == R_DUPOUT || r->op == R_DUPIN) &&
                              strcmp(r->word, "-") == 0)) {	/* {name}>file: a free fd from 10 */
      for (k = 10; k < MMC_FDS && sh_fd[k] >= 0; k++)
        ;
      if (k >= MMC_FDS) {
        sh_error("%s: too many open files", r->fdvar);
        return -1;
      }
    }
    if (k >= MMC_FDS) {
      sh_error("%d: bad file descriptor", k);
      return -1;
    }
    switch (r->op) {
      case R_HEREDOC: {
        char *body = r->here_quoted ? xstrdup(r->here ? r->here : "")
                                    : expand_heredoc(r->here ? r->here : "");
        if (expand_failed()) {
          free(body);
          return -1;
        }
        fd = feed_pipe(body, strlen(body), &th);
        break;
      }
      case R_HERESTR: {
        char *s = expand_str(r->word);
        char *body;
        if (expand_failed()) {
          free(s);
          return -1;
        }
        body = xstrcat3(s, "\n", "");
        free(s);
        fd = feed_pipe(body, strlen(body), &th);
        break;
      }
      case R_DUPIN:
      case R_DUPOUT: {
        size_t tl;
        if ((target = redir_target(r->word)) == NULL) return -1;
        tl = strlen(target);
        if (strcmp(target, "-") == 0) {	/* n>&- closes */
          free(target);
          if (r->fdvar != NULL) {	/* {var}>&-: the one in $var */
            char *v = fdvar_get(r->fdvar);
            k = v[0] ? atoi(v) : -1;
            free(v);
            if (k < 0 || k >= MMC_FDS) continue;
            fd_set_k(k, -1, 0, NULL);
            continue;
          }
          fd_set_k(k, -1, 0, sv);
          continue;
        }
        if (is_digits_n(target, tl) || (tl > 1 && target[tl - 1] == '-' &&
                                         is_digits_n(target, tl - 1))) {
          int from = atoi(target), move = target[tl - 1] == '-';
          free(target);
          if (from >= MMC_FDS || sh_fd[from] < 0) {
            sh_error("%d: bad file descriptor", from);
            return -1;
          }
          if (from == k) continue;
          fd = os_dup(sh_fd[from]);
          if (fd < 0) {
            sh_error("%d: bad file descriptor", from);
            return -1;
          }
          fd_set_k(k, fd, 1, sv);
          if (move) fd_set_k(from, -1, 0, sv);	/* n>&m- moves */
          continue;
        }
        if (r->op == R_DUPOUT && r->fd < 0) {	/* >&file is &>file */
          fd = open_target(target, R_OUT);
          free(target);
          if (fd < 0) return -1;
          fd_set_k(1, fd, 1, sv);
          fd_set_k(2, os_dup(fd), 1, sv);
          continue;
        }
        sh_error("%s: ambiguous redirect", target);
        free(target);
        return -1;
      }
      case R_BOTH:
      case R_BOTHAPP:
        if ((target = redir_target(r->word)) == NULL) return -1;
        fd = open_target(target, r->op);
        free(target);
        if (fd < 0) return -1;
        fd_set_k(1, fd, 1, sv);
        fd_set_k(2, os_dup(fd), 1, sv);
        continue;
      default:
        if ((target = redir_target(r->word)) == NULL) return -1;
        fd = open_target(target, r->op);
        free(target);
        break;
    }
    if (fd < 0) return -1;
    if (th != NULL && sv != NULL &&
        sv->nfeeders < (int)(sizeof(sv->feeders) / sizeof(sv->feeders[0])))
      sv->feeders[sv->nfeeders++] = th;
    else if (th != NULL) os_thread_join(th);
    if (r->fdvar != NULL) {	/* {var}> stays open, like bash */
      char num[24], *w;
      fd_set_k(k, fd, 1, NULL);
      w = xstrcat3(r->fdvar, "=", ll_to_str(k, num));	/* var or arr[i] */
      assign_word(w, 0, 0);
      free(w);
    }
    else fd_set_k(k, fd, 1, sv);
  }
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** Functions
** ===================================================================
*/

static Func *funcs = NULL;
static Vec funcname_stack;


Func *func_find (const char *name) {
  Func *f;
  for (f = funcs; f != NULL; f = f->next)
    if (strcmp(f->name, name) == 0) return f;
  return NULL;
}


/* export -f: every exported function, the way bash passes them on */
void func_env (Vec *out) {
  Func *f;
  for (f = funcs; f != NULL; f = f->next) {
    char *text, *eq;
    if (!(f->flags & V_EXPORT)) continue;
    text = func_pretty(f);	/* "name () \n{ ... }": bash wants "() { ..." */
    eq = xstrcat3("BASH_FUNC_", f->name, "%%=() ");
    vec_push(out, xstrcat3(eq, text + strlen(f->name) + 5, ""));
    free(eq);
    free(text);
  }
}


/*
** Functions a parent shell exported. The value must be exactly one
** function definition: anything after it would run (bash's Shellshock),
** so such a value is left alone.
*/
void func_import_env (void) {
  Vec env;
  size_t i;
  vec_init(&env);
  var_env_funcs(&env);
  for (i = 0; i < env.n; i++) {
    char *e = env.v[i] + 10, *pct = strstr(e, "%%="), *name, *text;
    Parser *p;
    Node *n = NULL, *rest = NULL;
    if (pct == NULL || strncmp(pct + 3, "() ", 3) != 0) continue;
    name = xstrndup(e, (size_t)(pct - e));
    if (!is_name(name) || parse_is_keyword(name)) {
      free(name);
      continue;
    }
    text = xstrcat3(name, " ", pct + 3);
    p = parse_new(text, "environment", 1);
    if (parse_next(p, &n) == P_OK && n != NULL) {
      Node *fn = (n->type == N_LIST && n->nkids == 1) ? n->kids[0] : n;
      if (fn->type == N_FUNC && fn->str != NULL && strcmp(fn->str, name) == 0 &&
          !(fn->flags & NF_BG) && fn->redir == NULL) {
        Prog *keep = fn->a->prog;
        prog_ref(keep);	/* the next parse_next lets go of this one */
        if (parse_next(p, &rest) == P_EOF) {	/* nothing after it */
          Func *f;
          func_define(name, fn->a, fn->src);
          if ((f = func_find(name)) != NULL) f->flags |= V_EXPORT;
        }
        prog_unref(keep);
      }
    }
    parse_free(p);
    free(text);
    free(name);
  }
  vec_free(&env);
}


void func_define (const char *name, Node *body, const char *src) {
  Func *f = func_find(name);
  if (f != NULL && (f->flags & V_READONLY)) {
    sh_error("%s: readonly function", name);
    return;
  }
  prog_ref(body->prog);
  if (f == NULL) {
    f = (Func *)xmalloc(sizeof(Func));
    memset(f, 0, sizeof(*f));
    f->name = xstrdup(name);
    f->next = funcs;
    funcs = f;
  }
  else {
    prog_unref(f->body->prog);
    free(f->src);
  }
  f->body = body;
  f->src = xstrdup(src ? src : "");
}


int func_unset (const char *name) {
  Func **pp;
  for (pp = &funcs; *pp != NULL; pp = &(*pp)->next) {
    if (strcmp((*pp)->name, name) == 0) {
      Func *f = *pp;
      if (f->flags & V_READONLY) {
        sh_error("%s: cannot unset: readonly function", name);
        return -1;
      }
      *pp = f->next;
      prog_unref(f->body->prog);
      free(f->name);
      free(f->src);
      free(f);
      return 0;
    }
  }
  return 0;
}


void func_names (Vec *out) {
  Func *f;
  for (f = funcs; f != NULL; f = f->next) vec_push(out, xstrdup(f->name));
  vec_sort(out);
}


/* the line each call was made on; grows and shrinks with funcname_stack */
static Vec callline_stack;

/* every call's arguments in order, and how many each call had: BASH_ARGV
** is the first read backwards, BASH_ARGC the second (bash fills them for
** calls only with shopt -s extdebug; the script's own are always there) */
static Vec argv_stack, argc_stack;
static size_t main_argv_n = 0, main_argc_n = 0;	/* those of the script */

/* local -: the set options to put back when the function returns */
static int *dash_values[MMC_FUNC_DEPTH + 1];


void sh_local_dash (void) {
  int n = 0, i;
  if (func_depth == 0 || dash_values[func_depth] != NULL) return;
  while (sh_opts[n].name) n++;
  dash_values[func_depth] = (int *)xmalloc((size_t)n * sizeof(int));
  for (i = 0; i < n; i++) dash_values[func_depth][i] = sh_opts[i].value;
}


static void set_args (void) {
  size_t i;
  var_make_array("BASH_ARGV", 0);
  for (i = 0; i < argv_stack.n; i++)
    var_aset("BASH_ARGV", (long long)i, argv_stack.v[argv_stack.n - 1 - i]);
  var_make_array("BASH_ARGC", 0);
  for (i = 0; i < argc_stack.n; i++)
    var_aset("BASH_ARGC", (long long)i, argc_stack.v[argc_stack.n - 1 - i]);
}


static void update_args (void) {
  if (O("extdebug")) set_args();
}


static int args_ready = 0;	/* BASH_ARGV and BASH_ARGC have been filled */


/*
** At start: the script's (or -c's) arguments go at the bottom. Like bash
** the arrays get them when first used outside a function (sh_args_touch),
** or at once with extdebug.
*/
void sh_main_args (void) {
  size_t k;
  char num[24];
  if (argc_stack.n > 0) return;
  for (k = 1; k < sh_pos.n; k++) vec_push(&argv_stack, xstrdup(sh_pos.v[k]));
  vec_push(&argc_stack, xstrdup(ll_to_str((long long)(sh_pos.n ? sh_pos.n - 1 : 0), num)));
  main_argv_n = argv_stack.n;
  main_argc_n = argc_stack.n;
  if (O("extdebug")) sh_args_touch();
}


/* BASH_ARGV or BASH_ARGC is looked at outside a function */
void sh_args_touch (void) {
  if (args_ready || main_argc_n == 0) return;
  args_ready = 1;
  set_args();
}


/* FUNCNAME follows the call stack; [0] is the running function */
static void update_funcname (void) {
  size_t i;
  if (funcname_stack.n == 0) {
    var_unset("FUNCNAME");
    var_make_array("BASH_LINENO", 0);	/* bash has the 0 of "main" even here */
    var_aset("BASH_LINENO", 0, "0");
    return;
  }
  var_make_array("FUNCNAME", 0);
  for (i = 0; i < funcname_stack.n; i++)
    var_aset("FUNCNAME", (long long)i, funcname_stack.v[funcname_stack.n - 1 - i]);
  var_aset("FUNCNAME", (long long)funcname_stack.n, "main");
  /* BASH_LINENO[i]: the line FUNCNAME[i] was called from */
  var_make_array("BASH_LINENO", 0);
  for (i = 0; i < callline_stack.n; i++)
    var_aset("BASH_LINENO", (long long)i, callline_stack.v[callline_stack.n - 1 - i]);
  var_aset("BASH_LINENO", (long long)callline_stack.n, "0");	/* main */
}


int func_call (Func *f, int argc, char **argv) {
  Vec saved_pos = sh_pos;
  int status, i, saved_loops;
  Node *body = f->body;
  Prog *prog = body->prog;
  if (func_depth >= MMC_FUNC_DEPTH) {
    sh_error("%s: maximum function nesting level exceeded (%d)", f->name, MMC_FUNC_DEPTH);
    return 1;
  }
  prog_ref(prog);	/* the function may redefine itself */
  vec_init(&sh_pos);
  vec_push(&sh_pos, xstrdup(saved_pos.n > 0 ? saved_pos.v[0] : MMC_NAME));
  for (i = 1; i < argc; i++) vec_push(&sh_pos, xstrdup(argv[i]));
  if (sh_compat() <= 44) sh_args_touch();	/* compat44: filled even in a function */
  var_scope_push();
  vec_push(&funcname_stack, xstrdup(f->name));
  {	/* the call site, for BASH_LINENO and caller */
    char num[24];
    sprintf(num, "%d", sh_lineno);
    vec_push(&callline_stack, xstrdup(num));
    if (argc_stack.n == 0) {	/* the script's own arguments at the bottom */
      size_t k;
      for (k = 1; k < saved_pos.n; k++) vec_push(&argv_stack, xstrdup(saved_pos.v[k]));
      vec_push(&argc_stack, xstrdup(ll_to_str((long long)(saved_pos.n ? saved_pos.n - 1 : 0), num)));
    }
    for (i = 1; i < argc; i++) vec_push(&argv_stack, xstrdup(argv[i]));
    vec_push(&argc_stack, xstrdup(ll_to_str(argc - 1, num)));
  }
  update_funcname();
  update_args();
  source_push(source_top());
  func_depth++;
  return_frames++;
  saved_loops = loop_depth;
  if (sh_compat() > 43) loop_depth = 0;	/* compat43: break in f ends the caller's loop */
  status = exec_node(body);
  if (returning) {
    returning = 0;
    status = sh_status;
  }
  loop_depth = saved_loops;
  return_frames--;
  func_depth--;
  if (traps[TRAP_RETURN] != NULL && traps[TRAP_RETURN][0] != '\0' && !in_trap &&
      (func_depth == 0 || O("functrace"))) {
    in_trap++;
    sh_run_string(traps[TRAP_RETURN], "trap", 1);
    in_trap--;
  }
  if (dash_values[func_depth + 1] != NULL) {	/* local -: the options come back */
    int k;
    for (k = 0; sh_opts[k].name; k++) sh_opts[k].value = dash_values[func_depth + 1][k];
    free(dash_values[func_depth + 1]);
    dash_values[func_depth + 1] = NULL;
  }
  source_pop();
  free(funcname_stack.v[--funcname_stack.n]);
  funcname_stack.v[funcname_stack.n] = NULL;
  for (i = 1; i < argc && argv_stack.n > 0; i++) free(argv_stack.v[--argv_stack.n]);
  if (argc_stack.n > 0) free(argc_stack.v[--argc_stack.n]);
  if (funcname_stack.n == 0) {	/* back in main: only what sh_main_args put there */
    while (argv_stack.n > main_argv_n) free(argv_stack.v[--argv_stack.n]);
    while (argc_stack.n > main_argc_n) free(argc_stack.v[--argc_stack.n]);
  }
  update_args();
  if (callline_stack.n > 0) {
    free(callline_stack.v[--callline_stack.n]);
    callline_stack.v[callline_stack.n] = NULL;
  }
  update_funcname();
  var_scope_pop();
  vec_free(&sh_pos);
  sh_pos = saved_pos;
  prog_unref(prog);
  return status;
}

/* }================================================================== */


/*
** {==================================================================
** Traps
** ===================================================================
*/

static const char *const signames[] = {
  "EXIT", "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS", "FPE", "KILL",
  "USR1", "SEGV", "USR2", "PIPE", "ALRM", "TERM", "STKFLT", "CHLD", "CONT",
  "STOP", "TSTP", "TTIN", "TTOU", "URG", "XCPU", "XFSZ", "VTALRM", "PROF",
  "WINCH", "IO", "PWR", "SYS"
};


int trap_signum (const char *name) {
  size_t i;
  const char *n = name;
  if (is_digits_n(name, strlen(name))) {
    int k = atoi(name);
    return (k >= 0 && k <= 64) ? k : -1;
  }
  if (m_strnicmp(n, "SIG", 3) == 0) n += 3;
  if (m_stricmp(n, "ERR") == 0) return TRAP_ERR;
  if (m_stricmp(n, "DEBUG") == 0) return TRAP_DEBUG;
  if (m_stricmp(n, "RETURN") == 0) return TRAP_RETURN;
  for (i = 0; i < sizeof(signames) / sizeof(signames[0]); i++)
    if (m_stricmp(n, signames[i]) == 0) return (int)i;
  if (m_stricmp(n, "IOT") == 0) return 6;
  if (m_stricmp(n, "POLL") == 0) return 29;
  return -1;
}


const char *trap_signame (int sig) {
  static char buf[16];
  if (sig == TRAP_ERR) return "ERR";
  if (sig == TRAP_DEBUG) return "DEBUG";
  if (sig == TRAP_RETURN) return "RETURN";
  if (sig >= 0 && (size_t)sig < sizeof(signames) / sizeof(signames[0])) return signames[sig];
  sprintf(buf, "%d", sig);
  return buf;
}


int trap_set (const char *sig, const char *action) {
  int k = trap_signum(sig);
  if (k < 0 || k >= TRAP_MAX) {
    sh_error("trap: %s: invalid signal specification", sig);
    return -1;
  }
  free(traps[k]);
  traps[k] = action ? xstrdup(action) : NULL;
  if (k > 0 && k < 65)
    os_catch_signal(k, action == NULL ? 0 : (action[0] == '\0' ? 2 : 1));
  return 0;
}


const char *trap_get (int sig) {
  return (sig >= 0 && sig < TRAP_MAX) ? traps[sig] : NULL;
}


void sh_run_traps (void) {
  int k;
  if (in_trap) return;
  for (k = 1; k < 65; k++) {
    if (!os_pending[k]) continue;
    os_pending[k] = 0;
    if (k == 2) os_interrupted = 0;	/* a trapped Ctrl-C does not unwind */
    if (traps[k] != NULL && traps[k][0] != '\0') {
      int keep = sh_status;
      char *action = xstrdup(traps[k]);
      in_trap++;
      sh_run_string(action, "trap", 1);
      in_trap--;
      free(action);
      if (!sh_exit) sh_status = keep;
    }
  }
}


static void run_exit_trap (void) {
  int keep;
  char *action;
  if (exit_trap_done || traps[TRAP_EXIT] == NULL || traps[TRAP_EXIT][0] == '\0') return;
  exit_trap_done = 1;
  keep = sh_status;
  action = xstrdup(traps[TRAP_EXIT]);
  sh_exit = 0;
  brk = cont_ = returning = 0;
  in_trap++;
  sh_run_string(action, "trap", 1);
  in_trap--;
  free(action);
  if (!sh_exit) sh_status = keep;
  sh_exit = 1;
}


void sh_exit_now (int status) {
  sh_status = status;
  run_exit_trap();
  sh_procsubst_cleanup();
}


/*
** Like bash: inside a function the ERR trap only fires with set -E
** (errtrace), and DEBUG and RETURN only with set -T (functrace).
*/
static int trap_reaches_here (const char *option) {
  return func_depth == 0 || O(option);
}


/* $BASH_COMMAND and the DEBUG trap: before every simple command */
void sh_before_command (char **argv, int argc) {
  Buf b;
  int i;
  if (argc <= 0 || in_trap) return;	/* a trap does not overwrite it */
  buf_init(&b);
  for (i = 0; i < argc; i++) {
    if (i > 0) buf_putc(&b, ' ');
    buf_puts(&b, argv[i]);
  }
  var_set("BASH_COMMAND", b.s ? b.s : "");
  if (traps[TRAP_DEBUG] != NULL && traps[TRAP_DEBUG][0] != '\0' && !in_trap &&
      trap_reaches_here("functrace")) {
    int saved = sh_status;
    in_trap++;
    sh_run_string(traps[TRAP_DEBUG], "trap", 1);
    in_trap--;
    sh_status = saved;
  }
  buf_free(&b);
}


static void err_trap (int status) {
  if (!trap_reaches_here("errtrace")) return;
  if (traps[TRAP_ERR] != NULL && traps[TRAP_ERR][0] != '\0' && !in_trap) {
    in_trap++;
    sh_run_string(traps[TRAP_ERR], "trap", 1);
    in_trap--;
    sh_status = status;
  }
}


/* set -e: a failing command ends the shell (outside conditions) */
int sh_errexit_check (int status) {
  if (status == 0 || no_errexit > 0) return 0;
  err_trap(status);
  if (O("errexit") && !sh_exit) {
    sh_status = status;
    sh_exit = 1;
    return 1;
  }
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** Subshells: a copy of the shell state, put back afterwards
** ===================================================================
*/

#define MAX_OPTS	160

typedef struct State {
  void *vars, *aliases;
  Func *funcs;
  char *cwd;
  Vec pos, dirs, funcnames;
  int opts[MAX_OPTS];
  char *traps[TRAP_MAX];
  int fd[MMC_FDS], own[MMC_FDS];
  int loop_depth, func_depth, return_frames, brk, cont, returning, exit_trap_done;
  long last_bg;
} State;


static Func *funcs_copy (Func *f) {
  Func *head = NULL, **tail = &head;
  for (; f != NULL; f = f->next) {
    Func *c = (Func *)xmalloc(sizeof(Func));
    *c = *f;
    c->name = xstrdup(f->name);
    c->src = xstrdup(f->src);
    prog_ref(f->body->prog);
    c->next = NULL;
    *tail = c;
    tail = &c->next;
  }
  return head;
}


static void funcs_free (Func *f) {
  while (f != NULL) {
    Func *next = f->next;
    prog_unref(f->body->prog);
    free(f->name);
    free(f->src);
    free(f);
    f = next;
  }
}


static void state_save (State *s) {
  int i;
  s->vars = var_save();
  s->aliases = alias_save();
  s->funcs = funcs_copy(funcs);
  s->cwd = os_getcwd();
  vec_init(&s->pos);
  vec_copy(&s->pos, sh_pos.v, sh_pos.n);
  vec_init(&s->dirs);
  vec_copy(&s->dirs, sh_dirstack.v, sh_dirstack.n);
  vec_init(&s->funcnames);
  vec_copy(&s->funcnames, funcname_stack.v, funcname_stack.n);
  for (i = 0; sh_opts[i].name && i < MAX_OPTS; i++) s->opts[i] = sh_opts[i].value;
  for (i = 0; i < TRAP_MAX; i++) {
    s->traps[i] = traps[i];
    /* a subshell forgets the traps, but keeps what is ignored */
    traps[i] = (traps[i] != NULL && traps[i][0] == '\0') ? xstrdup("") : NULL;
  }
  for (i = 0; i < MMC_FDS; i++) {
    s->fd[i] = sh_fd[i];
    s->own[i] = sh_own[i];
    sh_own[i] = 0;	/* the subshell must not close the parent's files */
  }
  s->loop_depth = loop_depth;
  s->func_depth = func_depth;
  s->return_frames = return_frames;
  s->brk = brk;
  s->cont = cont_;
  s->returning = returning;
  s->exit_trap_done = exit_trap_done;
  s->last_bg = sh_last_bg;
  if (sh_compat() > 44) loop_depth = 0;	/* compat44: break leaves the subshell */
  return_frames = 0;
  exit_trap_done = 0;
  sh_subshell++;
  {
    char num[24];
    var_set("BASH_SUBSHELL", ll_to_str(sh_subshell, num));
  }
}


static void state_restore (State *s) {
  int i;
  run_exit_trap();	/* the subshell's own EXIT trap */
  var_restore(s->vars);
  alias_restore(s->aliases);
  funcs_free(funcs);
  funcs = s->funcs;
  os_chdir(s->cwd);
  free(s->cwd);
  vec_free(&sh_pos);
  sh_pos = s->pos;
  vec_free(&sh_dirstack);
  sh_dirstack = s->dirs;
  vec_free(&funcname_stack);
  funcname_stack = s->funcnames;
  for (i = 0; sh_opts[i].name && i < MAX_OPTS; i++) sh_opts[i].value = s->opts[i];
  for (i = 0; i < TRAP_MAX; i++) {
    free(traps[i]);
    traps[i] = s->traps[i];
  }
  for (i = 0; i < MMC_FDS; i++) {	/* what the subshell opened goes */
    int cur = sh_fd[i], own = sh_own[i];
    sh_fd[i] = s->fd[i];
    sh_own[i] = s->own[i];
    if (own && cur >= 0) close_if_unused(cur);
  }
  loop_depth = s->loop_depth;
  func_depth = s->func_depth;
  return_frames = s->return_frames;
  brk = s->brk;
  cont_ = s->cont;
  returning = s->returning;
  exit_trap_done = s->exit_trap_done;
  sh_last_bg = s->last_bg;
  sh_exit = 0;
  sh_subshell--;
}


static int run_subshell (Node *body) {
  State s;
  int status;
  state_save(&s);
  status = exec_node(body);
  if (sh_exit) status = sh_status;
  returning = 0;	/* return in ( ) leaves the subshell */
  sh_status = status;
  state_restore(&s);
  sh_status = status;
  return status;
}


/* the reader side of $( ) */
typedef struct Reader {
  int fd;
  Buf out;
} Reader;


static void reader_main (void *arg) {
  Reader *r = (Reader *)arg;
  char chunk[16384];
  long n;
  while ((n = os_read(r->fd, chunk, sizeof(chunk))) > 0) buf_putn(&r->out, chunk, (size_t)n);
  os_close(r->fd);
}


static Reader *reader_start (int fd, OsThread **th) {
  Reader *r = (Reader *)xmalloc(sizeof(Reader));
  r->fd = fd;
  buf_init(&r->out);
  *th = os_thread_start(reader_main, r);
  return r;
}


/* ends the reader; returns what it read */
static char *reader_finish (Reader *r, OsThread *th, size_t *len) {
  char *out;
  if (th != NULL) os_thread_join(th);
  else reader_main(r);
  if (len) *len = r->out.len;
  out = buf_take(&r->out);
  free(r);
  return out;
}


char *sh_capture (const char *src, size_t *len) {
  State s;
  Reader *r;
  OsThread *th;
  int p[2], status;
  char *out;
  {	/* $(< file): the file, without running anything */
    const char *s = src;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    if (s[0] == '<' && s[1] != '<' && s[1] != '(' && s[1] != '&') {
      size_t n;
      s++;
      while (*s == ' ' || *s == '\t') s++;
      n = strlen(s);
      while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n')) n--;
      if (n > 0 && strcspn(s, ";|&<>\n") >= n) {
        char *word = xstrndup(s, n), *target = expand_str(word), *native;
        size_t flen = 0;
        char *data;
        free(word);
        native = path_to_native(target);
        data = read_file(native, &flen);
        if (data == NULL) {
          sh_error("%s: No such file or directory", target);
          last_subst_status = sh_status = 1;
          data = xstrdup("");
          flen = 0;
        }
        else last_subst_status = sh_status = 0;
        free(native);
        free(target);
        if (len) *len = flen;
        return data;
      }
    }
  }
  if (os_pipe(p) != 0) {
    sh_error("cannot make a pipe");
    if (len) *len = 0;
    return xstrdup("");
  }
  r = reader_start(p[0], &th);
  state_save(&s);
  loop_depth = s.loop_depth;	/* bash: break in $( ) ends it, at every level */
  sh_fd[1] = p[1];
  sh_own[1] = 1;
  if (!O("inherit_errexit") && !O("posix")) opt_set("errexit", 0);	/* like bash */
  status = sh_run_string(src, sh_source_name, sh_lineno);
  if (sh_exit) status = sh_status;
  returning = 0;
  state_restore(&s);	/* closes our end of the pipe */
  out = reader_finish(r, th, len);
  last_subst_status = status;
  sh_status = status;
  return out;
}

/* }================================================================== */


/*
** {==================================================================
** Child mmc for shell code that must run side by side (pipelines, &)
** ===================================================================
*/

static void dump_state (Buf *b) {
  Vec vars;
  size_t i;
  ShOpt *o;
  Func *f;
  vec_init(&vars);
  var_all(&vars);
  for (i = 0; i < vars.n; i++) {
    Var *v = (Var *)(void *)vars.v[i];
    if (v->flags & (V_SPECIAL | V_UNSET)) continue;
    if (strcmp(v->name, "FUNCNAME") == 0 || strcmp(v->name, "PIPESTATUS") == 0 ||
        strcmp(v->name, "BASH_REMATCH") == 0 || strcmp(v->name, "SHELLOPTS") == 0 ||
        strcmp(v->name, "BASHOPTS") == 0 || strcmp(v->name, "PPID") == 0 ||
        strcmp(v->name, "BASH_VERSINFO") == 0 || strcmp(v->name, "UID") == 0 ||
        strcmp(v->name, "EUID") == 0)
      continue;
    var_format(b, v);
  }
  free(vars.v);
  for (o = sh_opts; o->name; o++) {
    if (o->shopt) buf_printf(b, "shopt -%c %s\n", o->value ? 's' : 'u', o->name);
    else if (strcmp(o->name, "emacs") && strcmp(o->name, "vi") && strcmp(o->name, "history"))
      buf_printf(b, "set %co %s\n", o->value ? '-' : '+', o->name);
  }
  for (f = funcs; f != NULL; f = f->next) {
    buf_puts(b, f->src);
    buf_putc(b, '\n');
  }
  alias_dump(b);
  buf_puts(b, "set --");
  for (i = 1; i < sh_pos.n; i++) {
    char *q = shell_quote(sh_pos.v[i]);
    buf_putc(b, ' ');
    buf_puts(b, sh_pos.v[i][0] ? q : "''");
    free(q);
  }
  buf_putc(b, '\n');
}


/* starts "mmc --stage file pid" for src with the given descriptors */
static int spawn_stage (const char *src, int in, int out, int err, OsProc *proc,
                        long *pid) {
  Buf b;
  char *dir = path_tmpdir(), name[64], *file;
  char *argv[6], pidbuf[24];
  int fds[3], fd, r;
  Vec env;
  const char *d = var_get("MMC_STAGE_DEPTH");
  int depth = d ? atoi(d) : 0;
  char depthvar[48];
  if (depth >= 32) {	/* never a chain of children starting children without end */
    sh_error("too many nested mmc stages (%d): stopped", depth);
    free(dir);
    return -1;
  }
  sprintf(depthvar, "MMC_STAGE_DEPTH=%d", depth + 1);
  if (var_get("MMC_TRACE_STAGE") != NULL) fd_printf(2, "mmc: stage[%d]: %s\n", depth, src);
  sprintf(name, "mmc-stage-%ld-%d.sh", os_getpid(), ++stage_counter);
  file = path_join(dir, name);
  free(dir);
  buf_init(&b);
  dump_state(&b);
  buf_puts(&b, src);
  buf_putc(&b, '\n');
  if ((fd = os_open(file, OS_WRITE)) < 0) {
    sh_error("cannot write %s", file);
    buf_free(&b);
    free(file);
    return -1;
  }
  os_write(fd, b.s, b.len);
  os_close(fd);
  buf_free(&b);
  argv[0] = (char *)mmc_exe();
  argv[1] = "--stage";
  argv[2] = file;
  argv[3] = ll_to_str(sh_pid, pidbuf);
  argv[4] = NULL;
  fds[0] = in;
  fds[1] = out;
  fds[2] = err;
  vec_init(&env);
  var_env(&env);
  {	/* the child's depth, in its environment only */
    size_t k;
    for (k = 0; k < env.n; k++)
      if (strncmp(env.v[k], "MMC_STAGE_DEPTH=", 16) == 0) {
        free(env.v[k]);
        env.v[k] = xstrdup(depthvar);
        break;
      }
    if (k == env.n) vec_push(&env, xstrdup(depthvar));
  }
  r = os_spawn(mmc_exe(), argv, env.v, fds, 3, proc, pid);
  vec_free(&env);
  if (r != 0) os_unlink(file);
  free(file);
  return r;
}


/* the child side: runs the stage file, then removes it */
int sh_stage_main (const char *file, long pid) {
  size_t len = 0;
  char *text = read_file(file, &len);
  int status;
  os_unlink(file);
  if (text == NULL) {
    sh_error("--stage: %s: cannot read", file);
    return 127;
  }
  sh_pid = pid;
  sh_subshell = 1;
  status = sh_run_string(text, NULL, 1);
  free(text);
  if (sh_exit) status = sh_status;
  sh_exit_now(status);
  return sh_status;
}

/* }================================================================== */


/*
** {==================================================================
** Simple commands
** ===================================================================
*/

typedef struct TempVar {
  char *name, *old;
  int flags, existed, keep;
} TempVar;


static void temp_restore (TempVar *saved, int n);


/* NAME=value before a command: set now, undone afterwards */
static int temp_assign (char **assigns, int n, TempVar *saved, int export) {
  int i;
  for (i = 0; i < n; i++) {
    size_t eq = word_assign_pos(assigns[i]);
    char *name = xstrndup(assigns[i], eq);
    char *value;
    int append = 0;
    if (eq > 0 && assigns[i][eq - 1] == '+') {
      name[eq - 1] = '\0';
      append = 1;
    }
    value = expand_assign(assigns[i] + eq + 1);
    if (expand_failed()) {
      free(name);
      free(value);
      temp_restore(saved, i);	/* undo the ones already made */
      return -1;
    }
    saved[i].name = xstrdup(name);
    saved[i].keep = 0;
    saved[i].existed = var_flags(name) >= 0;
    saved[i].flags = saved[i].existed ? var_flags(name) : 0;
    saved[i].old = var_get(name) ? xstrdup(var_get(name)) : NULL;
    if (append) var_append(name, value);
    else var_set(name, value);
    if (export) var_set_flags(name, V_EXPORT, 0);
    free(name);
    free(value);
  }
  return 0;
}


static void temp_restore (TempVar *saved, int n) {
  int i;
  for (i = n - 1; i >= 0; i--) {
    if (!(saved[i].flags & V_READONLY) && !saved[i].keep) {
      if (!saved[i].existed) var_unset(saved[i].name);
      else {
        if (saved[i].old) var_set(saved[i].name, saved[i].old);
        else var_unset(saved[i].name);
        if (!(saved[i].flags & V_EXPORT)) var_set_flags(saved[i].name, 0, V_EXPORT);
      }
    }
    free(saved[i].name);
    free(saved[i].old);
  }
}


/* does this export / readonly / declare -x -r give 'name' an attribute? */
static int gives_attr (char **argv, const char *name) {
  size_t nl = strlen(name);
  int i, decl = strcmp(argv[0], "declare") == 0 || strcmp(argv[0], "typeset") == 0;
  int attr = !decl;
  if (!decl && strcmp(argv[0], "export") != 0 && strcmp(argv[0], "readonly") != 0) return 0;
  if (decl && var_in_function()) return 0;	/* there it makes a new local */
  for (i = 1; argv[i] != NULL; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--") == 0) {
      i++;
      break;
    }
    if ((a[0] != '-' && a[0] != '+') || a[1] == '\0') break;
    if (strpbrk(a + 1, "fgnp") != NULL) return 0;	/* functions, -g, names, listing */
    if (strpbrk(a + 1, "xr") != NULL) {
      if (a[0] == '+') return 0;
      attr = 1;
    }
  }
  if (!attr) return 0;
  for (; argv[i] != NULL; i++)
    if (strncmp(argv[i], name, nl) == 0 &&
        (argv[i][nl] == '\0' || argv[i][nl] == '=' || (argv[i][nl] == '+' && argv[i][nl + 1] == '=')))
      return 1;
  return 0;
}


/*
** "v=1 export v", "v=1 readonly v": the value stays, exported (in bash it
** came from the temporary environment). compat44: when v is a function's
** local the global v gets it too.
*/
static void temp_keep (TempVar *saved, int n, char **argv) {
  int i;
  for (i = 0; i < n; i++) {
    if (!gives_attr(argv, saved[i].name)) continue;
    saved[i].keep = 1;
    var_set_flags(saved[i].name, V_EXPORT, 0);
    if (sh_compat() <= 44 && !O("posix")) var_copy_global(saved[i].name);
  }
}


static void xtrace (char **argv, int argc, char **assigns, int nassigns) {
  Buf b;
  int i, lvl;
  const char *ps4 = var_get("PS4");
  char *pre = expand_str(ps4 ? ps4 : "+ ");
  buf_init(&b);
  /* the first character of PS4 repeats with the nesting level */
  for (lvl = 0; lvl < sh_subshell && pre[0]; lvl++) buf_putc(&b, pre[0]);
  buf_puts(&b, pre);
  free(pre);
  for (i = 0; i < nassigns; i++) {
    char *v = expand_assign(assigns[i]);
    if (i > 0) buf_putc(&b, ' ');
    buf_puts(&b, v);
    free(v);
  }
  for (i = 0; i < argc; i++) {
    char *q = shell_quote(argv[i]);
    if (i > 0 || nassigns > 0) buf_putc(&b, ' ');
    buf_puts(&b, argv[i][0] == '\0' ? "''" : q);
    free(q);
  }
  buf_putc(&b, '\n');
  {	/* BASH_XTRACEFD: the trace goes to that fd */
    const char *t = var_get("BASH_XTRACEFD");
    long long k = -1;
    int fd = sh_fd[2] >= 0 ? sh_fd[2] : 2;
    if (t != NULL && *t && str_to_ll(t, &k) == 0 && k >= 0 && k < MMC_FDS && sh_fd[k] >= 0)
      fd = sh_fd[k];
    os_write(fd, b.s, b.len);
  }
  buf_free(&b);
}


static int is_decl_builtin (const char *name) {
  return strcmp(name, "declare") == 0 || strcmp(name, "typeset") == 0 ||
         strcmp(name, "local") == 0 || strcmp(name, "export") == 0 ||
         strcmp(name, "readonly") == 0;
}


/* expands the words; declaration builtins get their NAME=value whole */
static int expand_command (Node *n, Vec *argv) {
  int i;
  if (n->nwords == 0) return 0;
  if (expand_words(n->words, 1, argv) != 0) return -1;
  if (argv->n == 1 && is_decl_builtin(argv->v[0]) && func_find(argv->v[0]) == NULL) {
    for (i = 1; i < n->nwords; i++) {
      const char *w = n->words[i];
      size_t eq = word_assign_pos(w);
      if (eq > 0) {
        size_t wl = strlen(w);
        if (w[eq + 1] == '(' && w[wl - 1] == ')') vec_push(argv, xstrdup(w));	/* a=(..) raw */
        else {
          char *name = xstrndup(w, eq + 1);
          char *val = expand_assign(w + eq + 1);
          vec_push(argv, xstrcat3(name, val, ""));
          free(name);
          free(val);
        }
        if (expand_failed()) return -1;
      }
      else if (expand_words((char **)&n->words[i], 1, argv) != 0) return -1;
    }
    return 0;
  }
  return expand_words(n->words + 1, n->nwords - 1, argv);
}


/* the status of a Windows program killed by Ctrl-C is 0xC000013A */
static int normalize_status (int st) {
  if ((unsigned)st == 0xC000013Au) return 130;
  if (st < 0 || st > 255) return (st & 0xFF) ? (st & 0xFF) : 1;
  return st;
}


#ifdef _WIN32

/* a program of git-bash/MSYS2 (next to msys-2.0.dll) reads /x/y paths itself */
static int is_msys_program (const char *prog) {
  static char *last_dir = NULL;
  static int last = 0;
  char *dir = path_dirname(prog);
  if (last_dir == NULL || m_fncmp(dir, last_dir) != 0) {
    char *dll = path_join(dir, "msys-2.0.dll");
    OsStat st;
    last = os_stat(dll, &st) == 0;
    free(dll);
    free(last_dir);
    last_dir = xstrdup(dir);
  }
  free(dir);
  return last;
}


/* for MSYS programs: /c/x, /tmp, / ... mean the same to them; a path of
** the mmc folder is converted only when it names something that exists */
static char *msys_arg (const char *arg) {
  char *conv;
  OsStat st;
  if (arg[0] == '/' && arg[1] == '/' && arg[2] != '/' && arg[2] != '\0' &&
      strchr(arg + 2, '/') == NULL)
    return msys_arg(arg + 1);	/* "$root/x" with root=/ : //x is /x */
  conv = path_arg_to_native(arg);
  if (strcmp(conv, arg) == 0 || arg[0] != '/') return conv;
  if (strcmp(arg, "/") == 0 || strcmp(arg, "/dev/null") == 0 ||
      (isalpha((unsigned char)arg[1]) && (arg[2] == '/' || arg[2] == '\0')) ||
      (strncmp(arg, "/tmp", 4) == 0 && (arg[4] == '\0' || arg[4] == '/'))) {
    free(conv);
    return xstrdup(arg);
  }
  if (os_stat(conv, &st) == 0) return conv;
  {
    char *parent = path_dirname(conv);
    int ok = m_fncmp(parent, path_root()) != 0 && os_stat(parent, &st) == 0 && st.is_dir;
    free(parent);
    if (ok) return conv;
  }
  free(conv);
  return xstrdup(arg);
}

#endif


/* runs a program with the shell's descriptors; 'async': does not wait */
#ifdef _WIN32
static void psub_files (Vec *args);
static int psub_fd (int k);
#endif


/* job control applies: an interactive shell on a terminal, not in ( ) or $( ) */
static int job_control_here (void) {
  return sh_interactive && sh_subshell == 0 && os_job_active();
}


static char *join_words (const Vec *v) {
  Buf b;
  size_t i;
  buf_init(&b);
  for (i = 0; i < v->n; i++) {
    if (i > 0) buf_putc(&b, ' ');
    buf_puts(&b, v->v[i]);
  }
  return b.s ? buf_take(&b) : xstrdup("");
}


static int run_external (const char *exe, Vec *argv, int async, OsProc *proc_out,
                         long *pid_out) {
  Vec env, args;
  OsProc proc;
  long pid = 0;
  int fds[MMC_FDS], nfds, i, r, nul_fd = -1, jc, stopped = 0;
  char *prog = xstrdup(exe);
  vec_init(&args);
  vec_copy(&args, argv->v, argv->n);
#ifdef _WIN32
  psub_files(&args);	/* <( ) and >( ): a file for a program */
  if (!has_exe_ext(prog)) {	/* a "#!" script: run its interpreter */
    char *iarg = NULL;
    char *interp = script_interp(prog, &iarg);
    char *found = NULL;
    if (interp != NULL) {
      found = sh_find_command(interp);
      if (found == NULL && (strcmp(interp, "sh") == 0 || strcmp(interp, "bash") == 0 ||
                            strcmp(interp, "mmc") == 0))
        found = xstrdup(mmc_exe());	/* no sh in PATH: we are a sh */
    }
    if (found == NULL) {
      sh_error("%s: bad interpreter '%s'", args.v[0], interp ? interp : "?");
      free(interp);
      free(iarg);
      free(prog);
      vec_free(&args);
      return 126;
    }
    free(args.v[0]);
    args.v[0] = path_to_display(prog);	/* the script becomes the first argument */
    if (iarg != NULL) vec_insert(&args, 0, iarg);
    vec_insert(&args, 0, interp);
    free(prog);
    prog = found;
  }
#endif
  {
#ifdef _WIN32
    int msys = is_msys_program(prog);
#endif
    for (i = 1; i < (int)args.n; i++) {	/* /d/x -> D:\x for native programs */
#ifdef _WIN32
      char *conv = msys ? msys_arg(args.v[i]) : path_arg_to_native(args.v[i]);
#else
      char *conv = path_arg_to_native(args.v[i]);
#endif
      free(args.v[i]);
      args.v[i] = conv;
    }
  }
  nfds = 3;
  for (i = 0; i < MMC_FDS; i++) {
    fds[i] = sh_fd[i];
#ifdef _WIN32
    if (i >= 3 && psub_fd(i)) fds[i] = -1;	/* a >( ) reader would never see the end */
#endif
    if (fds[i] >= 0 && i >= nfds) nfds = i + 1;
  }
  if (async && !sh_interactive && sh_fd[0] == 0) {	/* cmd & reads /dev/null */
    char *nul = path_to_native("/dev/null");
    nul_fd = os_open(nul, OS_READ);
    free(nul);
    fds[0] = nul_fd;
  }
  vec_init(&env);
  var_env(&env);
  jc = !async && job_control_here();
  if (jc) os_job_pgid(0);	/* a process group of its own */
  r = os_spawn(prog, args.v, env.v, fds, nfds, &proc, &pid);
  if (jc) os_job_pgid(-1);
  if (nul_fd >= 0) os_close(nul_fd);
  vec_free(&env);
  free(prog);
  if (r != 0) {
    vec_free(&args);
    return 126;
  }
  if (async) {
    vec_free(&args);
    if (proc_out) *proc_out = proc;
    if (pid_out) *pid_out = pid;
    return 0;
  }
  if (jc) os_tty_give(pid);
  r = os_wait_fg(proc, &stopped);
  if (jc) os_tty_give(0);
  if (stopped) {	/* Ctrl-Z: it waits as a job, the prompt comes back */
    char *cmd = join_words(argv);
    job_stopped(cmd, &proc, &pid, 1, pid);
    free(cmd);
    vec_free(&args);
    return r;
  }
  vec_free(&args);
  return normalize_status(r);
}


/* argv is expanded; runs a function, a builtin or a program */
static int run_argv (Vec *argv, char **assigns, int nassigns, int flags) {
  const char *name = argv->v[0];
  Func *f;
  const Builtin *b;
  char *exe;
  TempVar saved[64];
  int n = nassigns > 64 ? 64 : nassigns, status;
  if (!(flags & (EX_NOFUNC | EX_BUILTIN_ONLY)) && (f = func_find(name)) != NULL) {
    if (temp_assign(assigns, n, saved, 1) != 0) return 1;
    status = func_call(f, (int)argv->n, argv->v);
    temp_restore(saved, n);
    return status;
  }
  if ((b = builtin_find(name, 0)) != NULL && builtin_enabled(name)) {
    if (temp_assign(assigns, n, saved, 0) != 0) return 1;
    status = b->fn((int)argv->n, argv->v, sh_fd[0], sh_fd[1], sh_fd[2]);
    temp_keep(saved, n, argv->v);
    temp_restore(saved, n);
    return status;
  }
  if (flags & EX_BUILTIN_ONLY) {
    sh_error("builtin: %s: not a shell builtin", name);
    return 1;
  }
  exe = sh_find_command(name);
  if (exe == NULL) {
    OsStat st;
    char *native;
    if ((b = builtin_find(name, 1)) != NULL) {	/* fallbacks: ls, cat ... */
      if (temp_assign(assigns, n, saved, 0) != 0) return 1;
      status = b->fn((int)argv->n, argv->v, sh_fd[0], sh_fd[1], sh_fd[2]);
      temp_restore(saved, n);
      return status;
    }
    /* shopt -s autocd: a folder name alone means cd into it */
    if (O("autocd") && argv->n == 1 && sh_interactive) {
      char *dir = path_to_native(name);
      int is_dir = (os_stat(dir, &st) == 0 && st.is_dir);
      free(dir);
      if (is_dir) {
        char *a[3];
        a[0] = "cd";
        a[1] = (char *)name;
        a[2] = NULL;
        return sh_eval_argv(2, a, sh_fd[0], sh_fd[1], sh_fd[2], EX_NOFUNC);
      }
    }
    if (!(flags & EX_NOFUNC) && (f = func_find("command_not_found_handle")) != NULL) {
      Vec a;
      vec_init(&a);
      vec_push(&a, xstrdup("command_not_found_handle"));
      vec_copy(&a, argv->v, argv->n);
      status = func_call(f, (int)a.n, a.v);
      vec_free(&a);
      return status;
    }
    native = path_to_native(name);
    if (strchr(name, '/') != NULL && os_stat(native, &st) == 0) {
      sh_error(st.is_dir ? "%s: Is a directory" : "%s: Permission denied", name);
      free(native);
      return 126;
    }
    free(native);
    if (strchr(name, '/') != NULL) sh_error("%s: No such file or directory", name);
    else sh_error("%s: command not found", name);
    return 127;
  }
  if (temp_assign(assigns, n, saved, 1) != 0) {
    free(exe);
    return 1;
  }
  status = run_external(exe, argv, 0, NULL, NULL);
  temp_restore(saved, n);
  free(exe);
  return status;
}


int sh_eval_argv (int argc, char **argv, int in, int out, int err, int flags) {
  Vec v;
  int status;
  (void)in; (void)out; (void)err;
  if (argc <= 0) return 0;
  vec_init(&v);
  vec_copy(&v, argv, (size_t)argc);
  status = run_argv(&v, NULL, 0, flags);
  vec_free(&v);
  return status;
}


/* assignments alone: a=1 b=(x y) - they stay */
static int assign_only (Node *n) {
  int i;
  last_subst_status = 0;
  for (i = 0; i < n->nassigns; i++) {
    if (O("xtrace")) xtrace(NULL, 0, &n->assigns[i], 1);
    if (assign_word(n->assigns[i], 0, 0) != 0) return 1;
  }
  return last_subst_status;
}


/* exec [-cl] [-a name] [cmd args]: without a command, redirections stay */
static int do_exec (Vec *argv, Node *n) {
  size_t i = 1;
  Vec rest;
  char *exe;
  int status;
  Saves sv;
  while (i < argv->n && argv->v[i][0] == '-' && argv->v[i][1] != '\0') {
    if (strcmp(argv->v[i], "--") == 0) {
      i++;
      break;
    }
    if (strcmp(argv->v[i], "-a") == 0) i++;
    i++;
  }
  if (i >= argv->n) return apply_redirs(n->redir, NULL) != 0 ? 1 : 0;
  memset(&sv, 0, sizeof(sv));
  vec_init(&rest);
  vec_copy(&rest, argv->v + i, argv->n - i);
  if (apply_redirs(n->redir, &sv) != 0) {
    fd_restore(&sv);
    vec_free(&rest);
    return 1;
  }
  exe = sh_find_command(rest.v[0]);
  if (exe == NULL) {
    sh_error("%s: not found", rest.v[0]);
    fd_restore(&sv);
    vec_free(&rest);
    if (!sh_interactive && !O("execfail")) {
      sh_exit = 1;
      sh_status = 127;
    }
    return 127;
  }
  if (os_can_exec_replace() && sh_subshell == 0) {	/* POSIX: really replace us */
    Vec env;
    vec_init(&env);
    var_env(&env);
    run_exit_trap();
    os_exec(exe, rest.v, env.v);
    vec_free(&env);
  }
  status = run_external(exe, &rest, 0, NULL, NULL);
  free(exe);
  fd_restore(&sv);
  vec_free(&rest);
  sh_status = status;
  sh_exit = 1;	/* the command took our place: we end with it */
  return status;
}


static int exec_simple (Node *n) {
  Vec argv;
  Saves sv;
  int status;
  void *mark = sh_procsubst_mark();
  sh_set_lineno(n->line);
  memset(&sv, 0, sizeof(sv));
  vec_init(&argv);
  last_subst_status = 0;
  if (expand_command(n, &argv) != 0 || expand_failed()) {
    vec_free(&argv);
    if (!sh_interactive) sh_exit = 1;
    sh_procsubst_cleanup_to(mark);
    return 1;
  }
  if (argv.n == 0) {	/* only assignments and redirections */
    status = 0;
    sh_before_command(n->assigns, n->nassigns);
    if (apply_redirs(n->redir, &sv) != 0) status = 1;
    fd_restore(&sv);
    if (status == 0) status = assign_only(n);
    vec_free(&argv);
    sh_procsubst_cleanup_to(mark);
    return status;
  }
  if (O("xtrace")) xtrace(argv.v, (int)argv.n, n->assigns, n->nassigns);
  sh_before_command(argv.v, (int)argv.n);
  if (strcmp(argv.v[0], "exec") == 0 && func_find("exec") == NULL) {
    status = do_exec(&argv, n);
    vec_free(&argv);
    return status;
  }
  if (apply_redirs(n->redir, &sv) != 0) {
    fd_restore(&sv);
    vec_free(&argv);
    sh_procsubst_cleanup_to(mark);
    return 1;
  }
  status = run_argv(&argv, n->assigns, n->nassigns, 0);
  var_set("_", argv.v[argv.n - 1]);	/* $_: the last argument */
  fd_restore(&sv);
  vec_free(&argv);
  sh_procsubst_cleanup_to(mark);
  return status;
}

/* }================================================================== */


/*
** {==================================================================
** Pipelines
** ===================================================================
*/

typedef struct Stage {
  Node *n;
  Vec argv;	/* expanded, for simple commands */
  char *exe;	/* a program to start */
  int external, in_shell, failed;
  OsProc proc;
  long pid;
  int spawned, status;
  int in, out, err;	/* OS descriptors for the stage */
} Stage;


/* the shell's fd table as the stage sees it (for the time of a call) */
typedef struct View {
  int fd[MMC_FDS], own[MMC_FDS];
} View;


static void view_enter (View *v, const Stage *st) {
  int k;
  for (k = 0; k < MMC_FDS; k++) {
    v->fd[k] = sh_fd[k];
    v->own[k] = sh_own[k];
    sh_own[k] = 0;
  }
  sh_fd[0] = st->in;
  sh_fd[1] = st->out;
  sh_fd[2] = st->err;
}


static void view_leave (View *v) {
  int k;
  for (k = 0; k < MMC_FDS; k++) {
    sh_fd[k] = v->fd[k];
    sh_own[k] = v->own[k];
  }
}


static void spawn_program_stage (Stage *s) {
  View v;
  Saves sv;
  TempVar saved[64];
  int na = s->n->nassigns > 64 ? 64 : s->n->nassigns;
  memset(&sv, 0, sizeof(sv));
  view_enter(&v, s);
  if (apply_redirs(s->n->redir, &sv) == 0) {
    if (O("xtrace")) xtrace(s->argv.v, (int)s->argv.n, s->n->assigns, s->n->nassigns);
    if (temp_assign(s->n->assigns, na, saved, 1) == 0) {
      s->status = run_external(s->exe, &s->argv, 1, &s->proc, &s->pid);
      s->spawned = (s->status == 0);
      temp_restore(saved, na);
    }
    else s->status = 1;
  }
  else s->status = 1;
  fd_restore(&sv);
  view_leave(&v);
}


static void set_pipestatus (Stage *st, int n) {
  int i;
  char num[24];
  var_make_array("PIPESTATUS", 0);
  for (i = 0; i < n; i++) var_aset("PIPESTATUS", i, ll_to_str(st[i].status, num));
}


/* a builtin in the middle: its output is kept, then fed to the next stage */
static void run_middle_builtin (Stage *st, OsThread **feeder) {
  View v;
  Reader *r;
  OsThread *th;
  int p[2];
  size_t len;
  Feed *f;
  *feeder = NULL;
  if (os_pipe(p) != 0) {
    st->status = 1;
    return;
  }
  r = reader_start(p[0], &th);
  view_enter(&v, st);
  sh_fd[1] = p[1];
  no_errexit++;	/* set -e looks at the whole pipeline, not its stages */
  st->status = exec_node(st->n);
  no_errexit--;
  view_leave(&v);
  os_close(p[1]);
  f = (Feed *)xmalloc(sizeof(Feed));
  f->data = reader_finish(r, th, &len);
  f->len = len;
  f->fd = st->out;
  *feeder = os_thread_start(feed_main, f);
  if (*feeder == NULL) feed_main(f);
}


/*
** What "time" prints: $TIMEFORMAT like bash. %[p][l]R, U, S are the real,
** user and system seconds (p decimals, 0..3, default 3; l: 1m2.345s),
** %P the CPU share, %% a percent sign. Unset: bash's layout; empty:
** nothing. time -p always uses the POSIX layout.
*/
static void time_report (double real, double user, double sys, int posix) {
  const char *f = posix ? "real %2R\nuser %2U\nsys %2S" : var_get("TIMEFORMAT");
  Buf b;
  if (f == NULL) f = "\nreal\t%3lR\nuser\t%3lU\nsys\t%3lS";
  if (*f == '\0') return;
  buf_init(&b);
  for (; *f; f++) {
    int prec = 3, lng = 0;
    const char *start = f;
    double v;
    char num[64];
    if (*f != '%') {
      buf_putc(&b, *f);
      continue;
    }
    f++;
    if (*f == '%') {
      buf_putc(&b, '%');
      continue;
    }
    if (*f == 'P') {
      sprintf(num, "%.2f", real > 0 ? (user + sys) * 100.0 / real : 0.0);
      buf_puts(&b, num);
      continue;
    }
    if (*f >= '0' && *f <= '9') {
      prec = *f - '0';
      if (prec > 3) prec = 3;
      f++;
    }
    if (*f == 'l') {
      lng = 1;
      f++;
    }
    if (*f != 'R' && *f != 'U' && *f != 'S') {	/* not a format: as it is */
      buf_putn(&b, start, (size_t)(f - start + (*f != '\0')));
      if (*f == '\0') break;
      continue;
    }
    v = (*f == 'R') ? real : (*f == 'U') ? user : sys;
    if (lng) sprintf(num, "%dm%.*fs", (int)(v / 60), prec, v - 60 * (int)(v / 60));
    else sprintf(num, "%.*f", prec, v);
    buf_puts(&b, num);
  }
  buf_putc(&b, '\n');
  os_write(sh_fd[2], b.s, b.len);
  buf_free(&b);
}


static int exec_pipeline (Node *pn) {
  int n = pn->nkids, i, status = 0;
  long long t0 = 0;
  double tm0[4];
  if (pn->flags & NF_TIME) {
    t0 = os_now_us();
    os_times(tm0);
  }
  if (pn->flags & NF_NEGATE) no_errexit++;
  if (n == 1) {
    char num[24];
    status = exec_node(pn->kids[0]);
    var_make_array("PIPESTATUS", 0);
    var_aset("PIPESTATUS", 0, ll_to_str(status, num));
  }
  else if (n > 1) {
    void *mark = sh_procsubst_mark();
    int jc = job_control_here(), stopped_at = -1;
    long pgid = 0;
    Stage *st = (Stage *)xmalloc((size_t)n * sizeof(Stage));
    int (*pipes)[2] = (int (*)[2])xmalloc((size_t)n * sizeof(*pipes));
    OsThread **feeders = (OsThread **)xmalloc((size_t)n * sizeof(OsThread *));
    memset(st, 0, (size_t)n * sizeof(Stage));
    memset(feeders, 0, (size_t)n * sizeof(OsThread *));
    for (i = 0; i < n - 1; i++)
      if (os_pipe(pipes[i]) != 0) {
        sh_error("cannot make a pipe");
        pipes[i][0] = pipes[i][1] = -1;
      }
    for (i = 0; i < n; i++) {	/* what is each stage? */
      Stage *s = &st[i];
      s->n = pn->kids[i];
      vec_init(&s->argv);
      s->in = (i == 0) ? sh_fd[0] : pipes[i - 1][0];
      s->out = (i == n - 1) ? sh_fd[1] : pipes[i][1];
      s->err = (s->n->flags & NF_ERRPIPE) ? s->out : sh_fd[2];
      if (s->n->type == N_SIMPLE && s->n->nwords > 0) {
        if (expand_command(s->n, &s->argv) != 0 || expand_failed()) {
          s->failed = 1;
          s->status = 1;
          continue;
        }
        if (s->argv.n > 0 && func_find(s->argv.v[0]) == NULL &&
            !(builtin_find(s->argv.v[0], 0) != NULL && builtin_enabled(s->argv.v[0])) &&
            strcmp(s->argv.v[0], "exec") != 0 &&
            (s->exe = sh_find_command(s->argv.v[0])) != NULL)
          s->external = 1;
      }
      s->in_shell = !s->external;
    }
    /* 1. programs, and shell code in the middle (a child mmc) */
    if (jc) os_job_pgid(0);	/* the first one makes the group, the others join */
    for (i = 0; i < n; i++) {
      Stage *s = &st[i];
      if (s->failed) {
        s->in_shell = 0;
        continue;
      }
      if (s->external) spawn_program_stage(s);
      else if (i < n - 1) {
        int builtin_only = s->n->type == N_SIMPLE && s->argv.n > 0 &&
                           func_find(s->argv.v[0]) == NULL;
        if (!builtin_only) {
          if (spawn_stage(s->n->src ? s->n->src : "", s->in, s->out, s->err,
                          &s->proc, &s->pid) == 0)
            s->spawned = 1;
          else s->status = 126;
          s->in_shell = 0;
        }
      }
      if (jc && s->spawned && pgid == 0) {
        pgid = s->pid;
        os_job_pgid(pgid);
      }
    }
    if (jc) os_job_pgid(-1);
    if (jc && pgid != 0 && !st[n - 1].in_shell) os_tty_give(pgid);
    /* our copies of the pipe ends of running stages go, so EOF can come */
    for (i = 0; i < n - 1; i++) {
      if (!st[i].in_shell && pipes[i][1] >= 0) {
        os_close(pipes[i][1]);
        pipes[i][1] = -1;
      }
      if (!st[i + 1].in_shell && pipes[i][0] >= 0) {
        os_close(pipes[i][0]);
        pipes[i][0] = -1;
      }
    }
    /* 2. builtins in the middle, in order */
    for (i = 0; i < n - 1; i++) {
      if (!st[i].in_shell) continue;
      run_middle_builtin(&st[i], &feeders[i]);
      pipes[i][1] = -1;	/* the feeder owns it now */
      if (i > 0 && pipes[i - 1][0] >= 0) {
        os_close(pipes[i - 1][0]);
        pipes[i - 1][0] = -1;
      }
    }
    /* 3. the last stage, in the shell */
    if (st[n - 1].in_shell) {
      View v;
      view_enter(&v, &st[n - 1]);
      no_errexit++;
      /* bash runs the last stage in a subshell too, unless shopt -s lastpipe */
      st[n - 1].status = O("lastpipe") ? exec_node(st[n - 1].n) : run_subshell(st[n - 1].n);
      no_errexit--;
      view_leave(&v);
      if (pipes[n - 2][0] >= 0) {
        os_close(pipes[n - 2][0]);
        pipes[n - 2][0] = -1;
      }
    }
    /* 4. wait for everything */
    if (jc && pgid != 0) os_tty_give(pgid);
    for (i = 0; i < n; i++) {
      if (st[i].spawned && stopped_at < 0) {
        int s = 0, r = os_wait_fg(st[i].proc, &s);
        if (s) stopped_at = i;	/* Ctrl-Z: the rest is stopped too */
        st[i].status = s ? r : normalize_status(r);
        if (s) continue;
        st[i].spawned = 0;
      }
      /* a builtin feeding a stopped program would wait for ever: let it be */
      if (feeders[i] != NULL && stopped_at < 0) os_thread_join(feeders[i]);
    }
    if (jc && pgid != 0) os_tty_give(0);
    if (stopped_at >= 0) {	/* what still runs becomes a stopped job */
      OsProc *procs = (OsProc *)xmalloc((size_t)n * sizeof(OsProc));
      long *pids = (long *)xmalloc((size_t)n * sizeof(long));
      int np = 0;
      for (i = 0; i < n; i++)
        if (st[i].spawned) {
          procs[np] = st[i].proc;
          pids[np++] = st[i].pid;
        }
      if (np > 0) job_stopped(pn->src ? pn->src : "", procs, pids, np, pgid);
      free(procs);
      free(pids);
    }
    for (i = 0; i < n - 1; i++) {
      if (pipes[i][0] >= 0) os_close(pipes[i][0]);
      if (pipes[i][1] >= 0) os_close(pipes[i][1]);
    }
    set_pipestatus(st, n);
    status = st[n - 1].status;
    if (O("pipefail"))
      for (i = n - 1; i >= 0; i--)
        if (st[i].status != 0) {
          status = st[i].status;
          break;
        }
    for (i = 0; i < n; i++) {
      vec_free(&st[i].argv);
      free(st[i].exe);
    }
    free(st);
    free(pipes);
    free(feeders);
    sh_procsubst_cleanup_to(mark);
  }
  if (pn->flags & NF_NEGATE) {
    no_errexit--;
    status = !status;
  }
  if (pn->flags & NF_TIME) {
    double tm1[4], real = (double)(os_now_us() - t0) / 1e6, user, sys;
    os_times(tm1);
    user = (tm1[0] - tm0[0]) + (tm1[2] - tm0[2]);
    sys = (tm1[1] - tm0[1]) + (tm1[3] - tm0[3]);
    time_report(real, user, sys, (pn->flags & NF_TIMEP) != 0);
  }
  return status;
}

/* }================================================================== */


/*
** {==================================================================
** Background: cmd &
** ===================================================================
*/

static void report_bg (long pid) {
  sh_last_bg = pid;
  if (sh_interactive) {
    Job *j = job_by_pid(pid);
    fd_printf(2, "[%d] %ld\n", j ? j->id : 1, pid);
  }
}


/*
** coproc [NAME] command: runs in the background with a pipe to its input
** and one from its output. NAME[1] is the fd to write to it, NAME[0] the
** fd to read from it, NAME_PID its process (NAME is COPROC by default).
*/
static int exec_coproc (Node *n) {
  const char *name = n->str ? n->str : "COPROC";
  int to[2], from[2], kr, kw;
  OsProc proc;
  long pid = 0;
  char num[24], pidname[128];
  if (n->a == NULL || n->a->src == NULL) {
    sh_error("coproc: nothing to run");
    return 1;
  }
  for (kr = 10; kr < MMC_FDS && sh_fd[kr] >= 0; kr++)	/* two free fds from 10 */
    ;
  for (kw = kr + 1; kw < MMC_FDS && sh_fd[kw] >= 0; kw++)
    ;
  if (kw >= MMC_FDS) {
    sh_error("coproc: too many open files");
    return 1;
  }
  if (os_pipe(to) != 0) {
    sh_error("coproc: cannot make a pipe");
    return 1;
  }
  if (os_pipe(from) != 0) {
    os_close(to[0]);
    os_close(to[1]);
    sh_error("coproc: cannot make a pipe");
    return 1;
  }
  if (spawn_stage(n->a->src, to[0], from[1], sh_fd[2], &proc, &pid) != 0) {
    os_close(to[0]);
    os_close(to[1]);
    os_close(from[0]);
    os_close(from[1]);
    return 1;
  }
  os_close(to[0]);	/* the child's ends */
  os_close(from[1]);
  fd_set_k(kr, from[0], 1, NULL);
  fd_set_k(kw, to[1], 1, NULL);
  var_make_array(name, 0);
  var_aset(name, 0, ll_to_str(kr, num));
  var_aset(name, 1, ll_to_str(kw, num));
  snprintf(pidname, sizeof(pidname), "%s_PID", name);
  var_set(pidname, ll_to_str(pid, num));
  job_add(n->a->src, &proc, &pid, 1);
  report_bg(pid);
  return 0;
}


static int start_background (Node *n);


/* cmd &: under job control a process group of its own, so Ctrl-C and
** Ctrl-Z at the terminal leave it alone */
static int exec_background (Node *n) {
  int r, jc = job_control_here();
  if (jc) os_job_pgid(0);
  r = start_background(n);
  if (jc) os_job_pgid(-1);
  return r;
}


static int start_background (Node *n) {
  OsProc proc;
  long pid = 0;
  if (n->type == N_SIMPLE && n->nwords > 0) {	/* one program: start it, go on */
    Vec argv;
    char *exe = NULL;
    vec_init(&argv);
    if (expand_command(n, &argv) == 0 && !expand_failed() && argv.n > 0 &&
        func_find(argv.v[0]) == NULL && builtin_find(argv.v[0], 0) == NULL &&
        (exe = sh_find_command(argv.v[0])) != NULL) {
      Saves sv;
      TempVar saved[64];
      int na = n->nassigns > 64 ? 64 : n->nassigns, r = 1;
      memset(&sv, 0, sizeof(sv));
      if (apply_redirs(n->redir, &sv) == 0 && temp_assign(n->assigns, na, saved, 1) == 0) {
        r = run_external(exe, &argv, 1, &proc, &pid);
        temp_restore(saved, na);
      }
      fd_restore(&sv);
      free(exe);
      if (r == 0 && pid != 0) {
        job_add(n->src ? n->src : argv.v[0], &proc, &pid, 1);
        report_bg(pid);
      }
      vec_free(&argv);
      return r;
    }
    free(exe);
    vec_free(&argv);
  }
  {	/* anything else runs in a child mmc */
    int in = sh_fd[0], fd = -1;
    if (!sh_interactive && sh_fd[0] == 0) {
      char *nul = path_to_native("/dev/null");
      fd = os_open(nul, OS_READ);
      free(nul);
      if (fd >= 0) in = fd;
    }
    if (spawn_stage(n->src ? n->src : "", in, sh_fd[1], sh_fd[2], &proc, &pid) != 0) {
      if (fd >= 0) os_close(fd);
      return 1;
    }
    if (fd >= 0) os_close(fd);
    job_add(n->src ? n->src : "", &proc, &pid, 1);
    report_bg(pid);
  }
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** Compound commands
** ===================================================================
*/

static int unwinding (void) {
  if (os_interrupted) {
    if (!sh_interactive) {
      sh_exit = 1;
      sh_status = 130;
    }
    return 1;
  }
  return sh_exit || brk > 0 || cont_ > 0 || returning;
}


static int exec_list_items (Node **kids, int n) {
  int i, status = sh_status;
  for (i = 0; i < n; i++) {
    Node *k = kids[i];
    sh_run_traps();
    job_poll(0);
    if (unwinding()) break;
    status = (k->flags & NF_BG) ? exec_background(k) : exec_node(k);
    sh_status = status;
  }
  return status;
}


static int cond_status (Node *n) {
  int s;
  no_errexit++;
  s = exec_node(n);
  no_errexit--;
  return s;
}


/* after the body of a loop: 1 means leave the loop */
static int loop_control (void) {
  if (brk > 0) {
    brk--;
    return 1;
  }
  if (cont_ > 0 && --cont_ > 0) return 1;	/* continue 2: the outer loop goes on */
  return unwinding();
}


static int exec_for (Node *n) {
  Vec items;
  size_t i;
  int status = 0;
  vec_init(&items);
  if (n->has_in) {
    if (expand_words(n->words, n->nwords, &items) != 0) {
      vec_free(&items);
      return 1;
    }
  }
  else if (sh_pos.n > 1) vec_copy(&items, sh_pos.v + 1, sh_pos.n - 1);
  loop_depth++;
  for (i = 0; i < items.n; i++) {
    if (var_set(n->str, items.v[i]) != 0) {
      status = 1;
      break;
    }
    status = n->b ? exec_node(n->b) : 0;
    sh_status = status;
    if (loop_control()) break;
  }
  loop_depth--;
  vec_free(&items);
  return status;
}


static int arith_status (const char *raw, long long *v) {
  char *e = expand_str(raw);
  int r;
  if (expand_failed()) {
    free(e);
    return -1;
  }
  r = arith_eval(e, v);
  free(e);
  return r;
}


static int exec_arithfor (Node *n) {
  long long v = 0;
  int status = 0;
  if (n->arith[0] && arith_status(n->arith[0], &v) != 0) return 1;
  loop_depth++;
  for (;;) {
    const char *c = n->arith[1] ? n->arith[1] : "";
    while (*c == ' ' || *c == '\t') c++;
    if (*c != '\0') {
      if (arith_status(n->arith[1], &v) != 0) {
        status = 1;
        break;
      }
      if (v == 0) break;
    }
    status = n->b ? exec_node(n->b) : 0;
    sh_status = status;
    if (loop_control()) break;
    if (n->arith[2] && arith_status(n->arith[2], &v) != 0) {
      status = 1;
      break;
    }
  }
  loop_depth--;
  return status;
}


static int exec_while (Node *n, int until) {
  int status = 0;
  loop_depth++;
  for (;;) {
    int c = cond_status(n->a);
    if (unwinding()) {
      if (loop_control()) break;
      continue;
    }
    if (until ? c == 0 : c != 0) break;
    status = n->b ? exec_node(n->b) : 0;
    sh_status = status;
    if (loop_control()) break;
  }
  loop_depth--;
  return status;
}


static int exec_case (Node *n) {
  char *word = expand_str(n->str);
  CaseItem *it;
  int status = 0, fall = 0, flags = O("extglob") ? PM_EXTGLOB : 0;
  if (expand_failed()) {
    free(word);
    return 1;
  }
  for (it = n->items; it != NULL; it = it->next) {
    int hit = fall, k;
    for (k = 0; k < it->npats && !hit; k++) {
      char *pat = expand_pattern(it->pats[k]);
      hit = pat_match(pat, word, flags);
      free(pat);
    }
    if (!hit) continue;
    status = it->body ? exec_node(it->body) : 0;
    if (unwinding()) break;
    fall = (it->term == 1);	/* ;& runs the next body too */
    if (it->term == 0) break;	/* ;;& tests the next patterns */
  }
  free(word);
  return status;
}


static int exec_select (Node *n) {
  Vec items;
  int status = 0;
  vec_init(&items);
  if (n->has_in) {
    if (expand_words(n->words, n->nwords, &items) != 0) {
      vec_free(&items);
      return 1;
    }
  }
  else if (sh_pos.n > 1) vec_copy(&items, sh_pos.v + 1, sh_pos.n - 1);
  if (items.n == 0) {
    vec_free(&items);
    return 0;
  }
  loop_depth++;
  for (;;) {
    size_t i;
    const char *ps3 = var_get("PS3");
    char *line;
    int timed_out = 0;
    long long k = 0;
    for (i = 0; i < items.n; i++)
      fd_printf(sh_fd[2], "%lu) %s\n", (unsigned long)i + 1, items.v[i]);
    fd_puts(sh_fd[2], ps3 ? ps3 : "#? ");
    line = line_read_raw(sh_fd[0], '\n', -1, 0, -1, &timed_out);
    if (line == NULL) {
      fd_puts(sh_fd[2], "\n");
      status = 1;
      break;
    }
    var_set("REPLY", line);
    if (line[0] == '\0') {
      free(line);
      continue;
    }
    if (str_to_ll(line, &k) == 0 && k >= 1 && (size_t)k <= items.n) var_set(n->str, items.v[k - 1]);
    else var_set(n->str, "");
    free(line);
    status = n->b ? exec_node(n->b) : 0;
    if (loop_control()) break;
  }
  loop_depth--;
  vec_free(&items);
  return status;
}


static int exec_node (Node *n) {
  int status = 0;
  Saves sv;
  if (n == NULL) return 0;
  if (unwinding()) return sh_status;
  if (n->type != N_SIMPLE && n->type != N_LIST && n->type != N_AND && n->type != N_OR)
    sh_set_lineno(n->line);
  memset(&sv, 0, sizeof(sv));
  /* compound commands carry their own redirections */
  if (n->type != N_SIMPLE && n->type != N_FUNC && n->redir != NULL &&
      apply_redirs(n->redir, &sv) != 0) {
    fd_restore(&sv);
    return sh_status = 1;
  }
  switch (n->type) {
    case N_SIMPLE:
      status = sh_status = exec_simple(n);
      if (n->nwords > 0) {	/* a plain command is a pipeline of one */
        char num[24];
        var_make_array("PIPESTATUS", 0);
        var_aset("PIPESTATUS", 0, ll_to_str(status, num));
      }
      sh_errexit_check(status);
      break;
    case N_PIPE:
      status = sh_status = exec_pipeline(n);
      if (!(n->flags & NF_NEGATE) && n->nkids > 1) sh_errexit_check(status);
      break;
    case N_AND:
    case N_OR:
      status = sh_status = cond_status(n->a);
      if (unwinding()) break;
      if ((n->type == N_AND) == (status == 0)) status = sh_status = exec_node(n->b);
      break;
    case N_LIST:
      status = exec_list_items(n->kids, n->nkids);
      break;
    case N_SUBSHELL:
      status = sh_status = run_subshell(n->a);
      sh_errexit_check(status);
      break;
    case N_GROUP:
      status = exec_node(n->a);
      break;
    case N_IF: {
      int c = cond_status(n->a);
      if (unwinding()) {
        status = sh_status;
        break;
      }
      if (c == 0) status = exec_node(n->b);
      else status = n->c ? exec_node(n->c) : 0;
      break;
    }
    case N_WHILE: status = exec_while(n, 0); break;
    case N_UNTIL: status = exec_while(n, 1); break;
    case N_FOR: status = exec_for(n); break;
    case N_ARITHFOR: status = exec_arithfor(n); break;
    case N_SELECT: status = exec_select(n); break;
    case N_CASE: status = exec_case(n); break;
    case N_FUNC:
      func_define(n->str, n->a, n->src);
      status = 0;
      break;
    case N_ARITH: {
      long long v = 0;
      if (O("xtrace")) {
        char *e = expand_str(n->str);
        fd_printf(sh_fd[2], "+ (( %s ))\n", e);
        free(e);
      }
      status = sh_status = (arith_status(n->str, &v) != 0) ? 1 : (v == 0);
      sh_errexit_check(status);
      break;
    }
    case N_COND:
      status = sh_status = cond_eval(n);
      sh_errexit_check(status);
      break;
    case N_COPROC:
      status = sh_status = exec_coproc(n);
      break;
  }
  if (n->type != N_SIMPLE && n->type != N_FUNC && n->redir != NULL) fd_restore(&sv);
  sh_status = status;
  return status;
}


int sh_run_node (Node *n) {
  return exec_node(n);
}

/* }================================================================== */


/*
** {==================================================================
** Running text: scripts, -c, eval, source
** ===================================================================
*/

int sh_run_string (const char *text, const char *name, int line0) {
  Parser *p = parse_new(text, name, line0);
  int status = sh_status;
  const char *saved_name = sh_source_name;
  int is_eval = name != NULL && (strcmp(name, "eval") == 0 || strcmp(name, "trap") == 0);
  if (name != NULL && !is_eval) sh_source_name = name;
  for (;;) {
    Node *n;
    int r;
    Prog *prog;
    sh_run_traps();
    if (sh_exit || returning || os_interrupted) break;
    r = parse_next(p, &n);
    if (r == P_EOF) break;
    if (r != P_OK) {
      status = sh_status = 2;
      if (!sh_interactive && !is_eval) sh_exit = 1;
      break;
    }
    if (O("verbose")) fd_printf(sh_fd[2], "%s\n", n->src ? n->src : "");
    if (O("noexec")) continue;
    prog = n->prog;
    prog_ref(prog);
    status = sh_status = exec_node(n);
    prog_unref(prog);
    if ((brk > 0 || cont_ > 0) && loop_depth == 0) brk = cont_ = 0;	/* no loop: ignored */
  }
  parse_free(p);
  sh_source_name = saved_name;
  return status;
}


/* BASH_SOURCE: the file of each frame, the running one first */
static Vec source_stack;


static void source_sync (void) {
  size_t i;
  var_make_array("BASH_SOURCE", 0);
  for (i = 0; i < source_stack.n; i++)
    var_aset("BASH_SOURCE", (long long)i, source_stack.v[source_stack.n - 1 - i]);
}


void source_push (const char *name) {
  vec_push(&source_stack, xstrdup(name));
  source_sync();
}


void source_pop (void) {
  if (source_stack.n == 0) return;
  free(source_stack.v[--source_stack.n]);
  source_stack.v[source_stack.n] = NULL;
  source_sync();
}


const char *source_top (void) {
  return source_stack.n > 0 ? source_stack.v[source_stack.n - 1] : "main";
}


static int script_frame = 0;	/* the next sh_source is the main script */


int sh_run_script (const char *native) {
  script_frame = 1;
  return sh_source(native, 1, NULL, 0);
}


int sh_source (const char *native, int must_exist, char **args, int nargs) {
  static int depth = 0;
  int is_script = script_frame;
  size_t len = 0;
  char *text, *shown;
  int status, i;
  Vec saved_pos;
  if (depth >= MMC_SOURCE_DEPTH) {
    sh_error("source: nested too deeply");
    return 1;
  }
  text = read_file(native, &len);
  if (text == NULL) {
    if (must_exist) {
      char *d = path_to_display(native);
      sh_error("%s: No such file or directory", d);
      free(d);
    }
    return must_exist ? 1 : 0;
  }
  if (strlen(text) != len) {	/* NUL bytes: a program, not a script */
    char *d = path_to_display(native);
    sh_error("%s: cannot execute binary file", d);
    free(d);
    free(text);
    return 126;
  }
  crlf_to_lf(text, &len);	/* a script saved with Windows line ends */
  shown = path_to_display(native);
  saved_pos = sh_pos;
  if (nargs > 0) {
    vec_init(&sh_pos);
    vec_push(&sh_pos, xstrdup(saved_pos.n > 0 ? saved_pos.v[0] : MMC_NAME));
    for (i = 0; i < nargs; i++) vec_push(&sh_pos, xstrdup(args[i]));
  }
  script_frame = 0;
  depth++;
  if (!is_script) return_frames++;	/* return leaves a sourced file, not the script */
  source_push(shown);
  status = sh_run_string(strncmp(text, "\xEF\xBB\xBF", 3) == 0 ? text + 3 : text, shown, 1);
  source_pop();
  if (returning && !is_script) {
    returning = 0;
    status = sh_status;
  }
  if (!is_script) return_frames--;
  depth--;
  if (nargs > 0) {
    vec_free(&sh_pos);
    sh_pos = saved_pos;
  }
  free(shown);
  free(text);
  return status;
}


int sh_can_return (void) {
  return return_frames > 0;
}


void sh_do_return (int status) {
  sh_status = status;
  returning = 1;
}


void sh_do_break (int n, int is_continue) {
  if (loop_depth == 0) return;
  if (n > loop_depth) n = loop_depth;
  if (is_continue) cont_ = n;
  else brk = n;
}


int sh_loop_depth (void) {
  return loop_depth;
}

/* }================================================================== */


/*
** {==================================================================
** Process substitution: <( ) and >( ). The command runs in a child mmc
** at once, joined to us by a pipe, so its data streams: <(tail -f log)
** works. Elsewhere the path is a FIFO every program can open. On Windows
** it is /dev/fd/N, one of our own fds: our redirections, source, read and
** mapfile use the pipe as it is; a program gets a temporary file instead
** (Windows programs cannot open a pipe by a name), filled before it starts.
** ===================================================================
*/

typedef struct PSub {
  int write;	/* >( ): waited for, so its output is there when we go on */
  OsProc proc;
#ifdef _WIN32
  int k;	/* our fd number: /dev/fd/k */
  char *file;	/* a program got this file instead */
#else
  OsNPipe *np;
#endif
  struct PSub *next;
} PSub;

static PSub *psubs = NULL;


char *sh_procsubst (const char *src, int write) {
  int p[2];
  OsProc proc;
  long pid = 0;
  PSub *ps;
#ifdef _WIN32
  int k;
  char name[32];
  for (k = MMC_FDS - 1; k >= 10 && sh_fd[k] >= 0; k--)	/* 63 down, like bash */
    ;
  if (k < 10) {
    sh_error("too many open files");
    return NULL;
  }
#else
  OsNPipe *np;
  char *path = NULL, *shown;
#endif
  if (os_pipe(p) != 0) {
    sh_error("cannot make a pipe");
    return NULL;
  }
  if (spawn_stage(src, write ? p[0] : sh_fd[0], write ? sh_fd[1] : p[1], sh_fd[2],
                  &proc, &pid) != 0) {
    os_close(p[0]);
    os_close(p[1]);
    return NULL;
  }
  os_close(write ? p[0] : p[1]);	/* the child's end */
  ps = (PSub *)xmalloc(sizeof(PSub));
  memset(ps, 0, sizeof(*ps));
  ps->write = write;
  ps->proc = proc;
#ifdef _WIN32
  fd_set_k(k, write ? p[1] : p[0], 1, NULL);
  ps->k = k;
  ps->next = psubs;
  psubs = ps;
  sprintf(name, "/dev/fd/%d", k);
  return xstrdup(name);
#else
  {
    char *dir = path_tmpdir();
    np = os_npipe_new(dir, write ? p[1] : p[0], !write, &path);
    free(dir);
  }
  if (np == NULL) {
    sh_error("cannot make a named pipe");
    os_close(write ? p[1] : p[0]);
    os_detach(proc);
    free(ps);
    return NULL;
  }
  ps->np = np;
  ps->next = psubs;
  psubs = ps;
  shown = path_to_display(path);
  free(path);
  return shown;
#endif
}


#ifdef _WIN32
/* /dev/fd/k of a process substitution, or NULL */
static PSub *psub_of (const char *arg) {
  PSub *ps;
  if (strncmp(arg, "/dev/fd/", 8) != 0 || !is_digits_n(arg + 8, strlen(arg + 8))) return NULL;
  for (ps = psubs; ps != NULL; ps = ps->next)
    if (ps->k == atoi(arg + 8) && ps->file == NULL) return ps;
  return NULL;
}


/* is fd k the pipe of a process substitution? */
static int psub_fd (int k) {
  PSub *ps;
  for (ps = psubs; ps != NULL; ps = ps->next)
    if (ps->k == k) return 1;
  return 0;
}


/*
** A program is about to get these arguments: every /dev/fd/k of ours
** becomes a temporary file. <( ): what the command wrote, all of it
** (so it has to end); >( ): an empty file, the command reads it after.
*/
static void psub_files (Vec *args) {
  size_t i;
  for (i = 1; i < args->n; i++) {
    PSub *ps = psub_of(args->v[i]);
    char *dir, name[64];
    int fd;
    if (ps == NULL) continue;
    dir = path_tmpdir();
    sprintf(name, "mmc-ps-%ld-%d", os_getpid(), ps->k);
    ps->file = path_join(dir, name);
    free(dir);
    fd = os_open(ps->file, OS_WRITE);
    if (fd < 0) continue;
    if (!ps->write) {
      char buf[16384];
      long n;
      while ((n = os_read(sh_fd[ps->k], buf, sizeof(buf))) > 0) os_write(fd, buf, (size_t)n);
      fd_set_k(ps->k, -1, 0, NULL);
    }
    os_close(fd);
    free(args->v[i]);
    args->v[i] = path_to_display(ps->file);
  }
}
#endif


/* what the list is now: a command cleans up only what it made itself */
void *sh_procsubst_mark (void) {
  return psubs;
}


void sh_procsubst_cleanup (void) {
  sh_procsubst_cleanup_to(NULL);
}


void sh_procsubst_cleanup_to (void *mark) {
  while (psubs != NULL && psubs != (PSub *)mark) {
    PSub *ps = psubs;
    psubs = ps->next;
#ifdef _WIN32
    if (ps->write && ps->file != NULL) {	/* >( cmd ) now reads what the program wrote */
      size_t len = 0;
      char *data = read_file(ps->file, &len);
      if (data != NULL && sh_fd[ps->k] >= 0) os_write(sh_fd[ps->k], data, len);
      free(data);
    }
    if (sh_fd[ps->k] >= 0) fd_set_k(ps->k, -1, 0, NULL);	/* its end of input */
    if (ps->file != NULL) {
      os_unlink(ps->file);
      free(ps->file);
    }
#else
    os_npipe_end(ps->np);
#endif
    if (ps->write) os_wait(ps->proc);	/* >( cmd ): it has read it all */
    else os_detach(ps->proc);	/* <( cmd ) may go on; nobody reads it any more */
    free(ps);
  }
}

/* }================================================================== */


void sh_init_fds (void) {
  int i;
  for (i = 0; i < MMC_FDS; i++) {
    sh_fd[i] = (i < 3) ? i : -1;
    sh_own[i] = 0;
  }
  vec_init(&funcname_stack);
  vec_init(&sh_dirstack);
}
