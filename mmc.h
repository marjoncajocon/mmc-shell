/*
** mmc.h - MMC shell (Marjon Mangindo Cajocon)
** Shared declarations. Include this first in every source file.
*/

#ifndef mmc_h
#define mmc_h

/* feature macros must come before any system header */
#if !defined(_WIN32)
#if !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif
#if !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#endif

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define MMC_NAME	"mmc"
#define MMC_VERSION	"0.2.0"	/* also in mmc.rc and mterm.rc */
#define MMC_AUTHOR	"Marjon Mangindo Cajocon"
/* what $BASH_VERSION says: scripts check it before using bash features */
#define MMC_BASH_COMPAT	"5.2.0(1)-release"

#define MMC_HISTORY_MAX	1000
#define MMC_SOURCE_DEPTH	64
#define MMC_ALIAS_DEPTH	16
#define MMC_FUNC_DEPTH	1000
#define MMC_FDS		64	/* shell file descriptors 0..63; {var}> takes 10 and up */

/* byte that protects the next character from globbing (see mexpand.c) */
#define QMARK	'\001'


/*
** {==================================================================
** mutil.c - memory, strings, buffers
** ===================================================================
*/

typedef struct Buf {
  char *s;
  size_t len, cap;
} Buf;

/* vector of owned strings; v[n] is always NULL once something is pushed */
typedef struct Vec {
  char **v;
  size_t n, cap;
} Vec;

void *xmalloc (size_t n);
void *xrealloc (void *p, size_t n);
char *xstrdup (const char *s);
char *xstrndup (const char *s, size_t n);
char *xstrcat3 (const char *a, const char *b, const char *c);

void buf_init (Buf *b);
void buf_putc (Buf *b, char c);
void buf_putn (Buf *b, const char *s, size_t n);
void buf_puts (Buf *b, const char *s);
void buf_printf (Buf *b, const char *fmt, ...);
char *buf_take (Buf *b);
void buf_free (Buf *b);

void vec_init (Vec *v);
void vec_push (Vec *v, char *s);
void vec_insert (Vec *v, size_t at, char *s);
void vec_sort (Vec *v);
void vec_free (Vec *v);
void vec_copy (Vec *dst, char *const *src, size_t n);

int fd_puts (int fd, const char *s);
int fd_printf (int fd, const char *fmt, ...);

int m_stricmp (const char *a, const char *b);
int m_strnicmp (const char *a, const char *b, size_t n);
int m_fncmp (const char *a, const char *b);	/* file-name compare */
int m_fnncmp (const char *a, const char *b, size_t n);
int m_envcmp (const char *a, const char *b);	/* env-name compare */
size_t utf8_count (const char *s, size_t nbytes);
int utf8_len (const char *s);	/* bytes of the character at s */
int uc_width (unsigned long cp);	/* terminal columns: 0, 1 or 2 */
char *ll_to_str (long long v, char *out);	/* out: >= 24 bytes */
int is_name (const char *s);	/* [A-Za-z_][A-Za-z0-9_]* */
int is_name_n (const char *s, size_t n);
int str_to_ll (const char *s, long long *out);	/* whole string, base 10 */

char *read_file (const char *native, size_t *len);
void crlf_to_lf (char *s, size_t *len);
int mkdir_p (const char *native);

/* }================================================================== */


/*
** {==================================================================
** mos.c - operating system layer
** ===================================================================
*/

typedef intptr_t OsProc;

typedef struct OsStat {
  int exists, is_dir, is_link, is_fifo, is_sock, is_chr, is_blk;
  unsigned mode;	/* permission bits, 0777 style */
  long long size;
  long long blocks;	/* 512 byte units on the disk */
  unsigned long nlink;
  time_t mtime, atime, ctime;
  unsigned long long dev, ino;
  long uid, gid;
} OsStat;

void os_init (void);
void os_shutdown (void);
void os_args (int *argc, char ***argv);

char *os_getenv (const char *name);	/* malloc'd, or NULL */
void os_setenv (const char *name, const char *value);	/* NULL unsets */
void os_env_list (Vec *out);	/* "NAME=value" */

char *os_getcwd (void);
int os_chdir (const char *native);
int os_stat (const char *native, OsStat *st);
int os_lstat (const char *native, OsStat *st);
int os_is_exec (const char *native);
int os_access (const char *native, int what);	/* 'r', 'w' or 'x' */
int os_listdir (const char *native, Vec *out);
int os_mkdir (const char *native);
int os_unlink (const char *native);
char *os_realpath (const char *native);	/* malloc'd, or NULL */

#define OS_READ		0
#define OS_WRITE	1
#define OS_APPEND	2
#define OS_RDWR		3
#define OS_EXCL		4	/* create, fail if it exists (noclobber) */
int os_open (const char *native, int mode);
int os_pipe (int fds[2]);
int os_dup (int fd);
void os_close (int fd);
long os_read (int fd, void *buf, size_t n);
long os_write (int fd, const void *buf, size_t n);
int os_wait_readable (int fd, int ms);	/* 1 ready, 0 timeout, -1 error */
int os_fd_valid (int fd);

int os_is_tty (int fd);
int os_tty_raw (int on);
int os_tty_getbyte (void);
void os_tty_fix (void);
int os_term_cols (void);
int os_term_rows (void);

char *os_exe_path (const char *argv0);
char *os_hostname (void);
char *os_username (void);
long os_getpid (void);
long os_getppid (void);
long os_getuid (void);
long os_getgid (void);
long os_geteuid (void);
long long os_now_us (void);	/* wall clock, microseconds */
void os_times (double t[4]);	/* user, sys, children user, children sys */
int os_umask (int mask);	/* returns the old one; -1 only asks */
const char *os_type (void);	/* $OSTYPE */
const char *os_machine (void);	/* $MACHTYPE / $HOSTTYPE */

/* child fd k is fds[k] (k < nfds; -1: closed); envp NULL: our environment */
int os_spawn (const char *exe, char **argv, char **envp, const int *fds,
              int nfds, OsProc *proc, long *pid);
int os_wait (OsProc proc);
int os_poll_proc (OsProc proc, int *status);	/* 1 finished, 0 running, 2 stopped */
/* job control (POSIX terminals; Windows: none, but kill -STOP / -CONT work) */
int os_job_control (int interactive);	/* at start: 1 when it is on */
int os_job_active (void);
void os_job_pgid (long pgid);	/* programs started from now on join group pgid (0: a new one, -1: ours) */
void os_tty_give (long pgid);	/* the terminal to group pgid, 0: back to the shell */
int os_wait_fg (OsProc proc, int *stopped);	/* os_wait; *stopped: Ctrl-Z stopped it */
int os_suspend_self (void);
void os_detach (OsProc proc);
int os_kill (long pid, int sig);
int os_exec (const char *exe, char **argv, char **envp);	/* POSIX only */
int os_can_exec_replace (void);

/* Ctrl-C: set by the signal/console handler, read by the shell */
extern volatile int os_interrupted;
void os_catch_signal (int sig, int on);	/* trap: deliver to os_pending */
extern volatile int os_pending[65];
extern int os_pipe_exit;	/* Windows: a write into a pipe nobody reads ends us (SIGPIPE) */

typedef struct OsThread OsThread;
typedef void (*OsThreadFn) (void *arg);
OsThread *os_thread_start (OsThreadFn fn, void *arg);
void os_thread_join (OsThread *t);
/* a pipe a program opens by its path (<( ) and >( )); to_reader: what is
** written into fd comes out there, else what is written there comes out of fd */
typedef struct OsNPipe OsNPipe;
OsNPipe *os_npipe_new (const char *dir, int fd, int to_reader, char **path);	/* takes fd */
void os_npipe_end (OsNPipe *np);

/* for the tools (c*.c): what errno says elsewhere, Windows included */
enum {
  OS_E_OTHER = 1, OS_E_NOENT, OS_E_ACCES, OS_E_EXIST, OS_E_NOTEMPTY,
  OS_E_NOTDIR, OS_E_ISDIR, OS_E_XDEV, OS_E_BUSY, OS_E_NOSPC, OS_E_PERM,
  OS_E_INVAL, OS_E_LOOP
};
int os_errcode (void);	/* why the last os_ call failed: OS_E_... */
const char *os_errmsg (void);	/* the same as text: "No such file or directory" */
int os_rmdir (const char *native);
int os_rename (const char *from, const char *to);	/* replaces 'to' */
int os_chmod (const char *native, unsigned mode);	/* Windows: the write bits only */
int os_utime (const char *native, time_t atime, time_t mtime);
int os_symlink (const char *target, const char *native, int is_dir);
int os_link (const char *from, const char *to);
char *os_readlink (const char *native);	/* malloc'd, or NULL */
int os_same_file (const char *a, const char *b);	/* 1: the same file */
long long os_seek (int fd, long long off, int whence);	/* 0 set, 1 cur, 2 end; -1 */
void os_sleep_ms (int ms);
int os_diskfree (const char *native, unsigned long long *total,
                 unsigned long long *avail, unsigned long long *free_);
void os_mounts (Vec *out);	/* "device\tmount point\ttype" */
typedef struct OsUname {
  char sysname[64], release[64], version[128], machine[32], os[32];
} OsUname;
void os_uname (OsUname *u);
typedef struct OsProcInfo {
  long pid, ppid, uid;
  long long rss, vsz;	/* bytes */
  double cpu;	/* seconds used */
  time_t start;
  char *name;	/* the program */
  char *cmd;	/* its command line, or the name */
  char state;	/* R S Z T ..., '?' unknown */
} OsProcInfo;
int os_proclist (OsProcInfo **out, size_t *n);
void os_proclist_free (OsProcInfo *v, size_t n);
unsigned long long os_memtotal (void);
char *os_user_name (long uid);	/* malloc'd; the number if unknown */
char *os_group_name (long gid);
int os_is_system_program (const char *native);	/* Windows: under %SystemRoot% */

/* }================================================================== */


/*
** {==================================================================
** mpath.c - Linux style paths ("/" is the MMC root, "/d" is drive D:)
** ===================================================================
*/

#ifdef _WIN32
#define MMC_SEP		'\\'
#define MMC_SEPS	"\\"
#else
#define MMC_SEP		'/'
#define MMC_SEPS	"/"
#endif

void path_set_root (const char *native);
const char *path_root (void);
int path_is_sep (int c);
char *path_join (const char *a, const char *b);
char *path_dirname (const char *native);
const char *path_basename (const char *native);
char *path_to_native (const char *p);
char *path_to_display (const char *native);	/* root mapped to "/" */
char *path_to_drive (const char *native);	/* only "X:" -> "/x" */
char *path_arg_to_native (const char *arg);
char *path_env_to_native (const char *val);
void path_list_split (const char *val, Vec *out);
char *path_list_normalize (const char *val);
char *path_tmpdir (void);	/* where temporary files go, native */

/* }================================================================== */


/*
** {==================================================================
** mvar.c - shell variables
** ===================================================================
*/

#define V_EXPORT	0x0001
#define V_READONLY	0x0002
#define V_INTEGER	0x0004
#define V_ARRAY		0x0008
#define V_ASSOC		0x0010
#define V_LOWER		0x0020
#define V_UPPER		0x0040
#define V_NAMEREF	0x0080
#define V_UNSET		0x0100	/* declared ("local x"), but no value */
#define V_SPECIAL	0x0200	/* RANDOM, SECONDS ...: computed */
#define V_CAPITAL	0x0400

typedef struct Elem {	/* element of an array */
  long long idx;	/* indexed arrays */
  char *key;	/* associative arrays */
  char *val;
} Elem;

typedef struct Var {
  char *name;
  int flags;
  char *val;	/* scalars */
  Elem *el;	/* arrays: sorted by idx, or in insertion order */
  size_t n, cap;
  struct Var *next;	/* hash chain */
} Var;

void var_init (void);	/* imports the environment */
Var *var_lookup (const char *name);	/* follows namerefs; NULL if absent */
const char *var_get (const char *name);	/* NULL: unset */
int var_set (const char *name, const char *value);	/* -1: readonly */
int var_append (const char *name, const char *value);	/* += */
int var_unset (const char *name);	/* -1: readonly */
int var_unset_local (const char *name);
void var_copy_global (const char *name);	/* compat44: a local's value to the global */
int var_flags (const char *name);	/* -1: absent */
int var_set_flags (const char *name, int on, int off);	/* creates it */
int var_is_set (const char *name);
int var_local (const char *name);	/* in the current function scope */
int var_in_function (void);
void var_scope_push (void);
void var_scope_pop (void);
void var_env (Vec *out);	/* "NAME=value" of what is exported */
void var_env_funcs (Vec *out);	/* BASH_FUNC_x%%= entries we were given */
void var_names (Vec *out, const char *prefix, int all);
void var_all (Vec *out);	/* Var * of every visible variable, sorted */
void *var_save (void);	/* a copy of everything, for subshells */
void var_restore (void *saved);
void var_path_changed (void);

/* arrays */
int var_aset (const char *name, long long idx, const char *value);
int var_akset (const char *name, const char *key, const char *value);
const char *var_aget (const char *name, long long idx);
const char *var_akget (const char *name, const char *key);
int var_aunset (const char *name, long long idx);
int var_akunset (const char *name, const char *key);
void var_values (const char *name, Vec *out);	/* ${a[@]} */
void var_keys (const char *name, Vec *out);	/* ${!a[@]} */
size_t var_count (const char *name);
long long var_anext (const char *name);	/* index after the last one */
int var_make_array (const char *name, int assoc);	/* empties it */

/* }================================================================== */


/*
** {==================================================================
** mparse.c - tokens and the syntax tree
** ===================================================================
*/

/* redirection operators */
enum {
  R_IN, R_OUT, R_CLOBBER, R_APPEND, R_RW, R_DUPIN, R_DUPOUT,
  R_HEREDOC, R_HERESTR, R_BOTH, R_BOTHAPP
};

typedef struct Redir {
  int fd;	/* -1: the default of the operator */
  int op;
  char *word;	/* target, raw; for here-docs the delimiter */
  char *here;	/* here-doc body */
  int here_quoted;	/* no expansion in the body */
  char *fdvar;	/* {name}>file */
  struct Redir *next;
} Redir;

enum {
  N_SIMPLE, N_PIPE, N_AND, N_OR, N_LIST, N_SUBSHELL, N_GROUP, N_IF,
  N_WHILE, N_UNTIL, N_FOR, N_ARITHFOR, N_SELECT, N_CASE, N_FUNC,
  N_ARITH, N_COND, N_COPROC
};

/* N_COND node kinds (in 'kind') */
enum { C_AND, C_OR, C_NOT, C_UNARY, C_BINARY, C_WORD };

#define NF_BG		1	/* list item ends with & */
#define NF_NEGATE	2	/* ! pipeline */
#define NF_TIME		4	/* time pipeline */
#define NF_TIMEP	8	/* time -p */
#define NF_ERRPIPE	16	/* stage is followed by |& */

typedef struct CaseItem {
  char **pats;
  int npats;
  struct Node *body;
  int term;	/* 0 ;;  1 ;&  2 ;;& */
  struct CaseItem *next;
} CaseItem;

typedef struct Node {
  int type, line, flags;
  int kind;	/* N_COND: C_AND ... */
  Redir *redir;
  struct Node *a, *b, *c;	/* condition / then / else, left / right, body */
  struct Node **kids;	/* N_LIST, N_PIPE */
  int nkids;
  char **words;	/* simple command, for ... in, cond operands */
  int nwords;
  char **assigns;	/* NAME=value prefixes */
  int nassigns;
  char *str;	/* variable, function name, arithmetic, case word, cond op */
  char *arith[3];	/* for (( ; ; )) */
  int has_in;	/* for NAME in ... */
  char *src;	/* source text: functions, pipeline stages, & */
  CaseItem *items;
  struct Prog *prog;
} Node;

typedef struct Prog {	/* owns every node of one parsed command */
  struct Arena *arena;
  int refs;
} Prog;

enum { P_OK, P_EOF, P_ERROR, P_INCOMPLETE };

typedef struct Parser Parser;
Parser *parse_new (const char *text, const char *name, int line0);
int parse_next (Parser *p, Node **out);	/* one complete command */
int parse_line (Parser *p);	/* line of the next command */
void parse_free (Parser *p);
void parse_set_check (Parser *p, int on);	/* --check: collect, do not stop */
int parse_errors (Parser *p);
int parse_is_complete (const char *text);	/* for the prompt: P_OK ... */
void prog_ref (Prog *p);
void prog_unref (Prog *p);
char *parse_word_list (const char *text, Vec *out);	/* a=( ... ) contents */
size_t parse_skip_subst (const char *s, int kind);	/* end of $( ${ $(( ` */
int parse_is_keyword (const char *w);

/* }================================================================== */


/*
** {==================================================================
** mexpand.c, mpattern.c, marith.c, mregex.c - expansion
** ===================================================================
*/

size_t word_assign_pos (const char *w);	/* '=' of NAME=, NAME+=, NAME[i]= */
int expand_words (char **raw, int n, Vec *out);	/* -1: error */
int expand_word (const char *raw, Vec *out);
char *expand_str (const char *raw);	/* no splitting, no globbing */
char *expand_assign (const char *raw);	/* tilde after = and : too */
char *expand_pattern (const char *raw);	/* marked: quoted parts literal */
char *expand_heredoc (const char *text);
char *expand_prompt (const char *ps);	/* \u \h \w ... then $VAR */
char *expand_ansi_c (const char *s, size_t n, size_t *used);	/* $'...' */
int expand_failed (void);	/* ${x?}, bad substitution, failglob */
char *shell_quote (const char *s);	/* 'it''s' style, for declare -p */
char *unmark (const char *marked);

#define PM_EXTGLOB	1
#define PM_NOCASE	2
#define PM_PATHNAME	4	/* '*' does not match '/' */
#define PM_PERIOD	8	/* leading '.' must be matched explicitly */
int pat_match (const char *pat, const char *s, int flags);	/* pat is marked */
int pat_match_len (const char *pat, const char *s, size_t n, int flags);
int pat_has_glob (const char *marked, int extglob);
void glob_expand (const char *marked, Vec *out);	/* nothing if no match */

int arith_eval (const char *expr, long long *out);	/* 0 ok, -1 error */

typedef struct Regex Regex;
Regex *regex_compile (const char *pat, int icase, char **err);	/* ERE */
int regex_exec (Regex *re, const char *s, Vec *groups);	/* 1 match */
void regex_free (Regex *re);
#define RE_EXTENDED	1	/* ERE; else BRE (grep, sed) */
#define RE_ICASE	2
#define RE_NEWLINE	4	/* ^ $ at every line, . not a newline (sed M) */
#define RE_AWK		8	/* \ escapes inside [...] */
#define RE_WORDS	16	/* grep -w: no word character right before or after */
#define RE_WHOLE	32	/* grep -x: the whole subject */
#define RE_NOTBOL	1	/* regex_match: the start is not a line start */
#define RE_NOTEOL	2
Regex *regex_new (const char *pat, int flags, char **err);
Regex *regex_new_n (const char *pat, size_t len, int flags, char **err);
/* leftmost-longest match in s[0..len) at or after start; m (may be NULL):
** 2 * (regex_nsub + 1) offsets, (size_t)-1 for a group that took no part */
int regex_match (Regex *re, const char *s, size_t len, size_t start, int eflags, size_t *m);
int regex_nsub (const Regex *re);

/* }================================================================== */


/*
** {==================================================================
** mexec.c - running commands
** ===================================================================
*/

extern int sh_status;		/* $? */
extern int sh_exit;		/* 'exit' was called: unwind */
extern int sh_interactive;
extern int sh_login;
extern int sh_lineno;
extern int sh_subshell;		/* nesting of ( ) and $( ) */
extern int sh_dash_c;		/* started with -c: $- says c */
extern int sh_command_number;	/* commands read so far: \# in the prompt */
void sh_local_dash (void);	/* local -: set options come back at return */
int hist_expand_word (const char *word, char **out);	/* history -p */
extern long sh_pid;		/* $$ - the same in subshells */
extern long sh_last_bg;		/* $! */
extern Vec sh_pos;		/* $0 $1 ... */
extern const char *sh_source_name;	/* script being read, for messages */
extern int sh_fd[MMC_FDS];	/* shell fd -> OS fd, -1 closed */

/* set -o / shopt -s options */
typedef struct ShOpt {
  const char *name;
  char letter;	/* for set -x style, 0 none */
  int shopt;	/* 1: shopt, 0: set -o */
  int value;
} ShOpt;
extern ShOpt sh_opts[];
int opt_get (const char *name);
int opt_set (const char *name, int on);	/* -1: unknown */
int opt_letter (char c, int on);	/* -1: unknown */
char *opt_flags (void);	/* $- */
void opt_compat (const char *name, int on);	/* shopt -s/-u compatNN */
int sh_compat (void);	/* the compatibility level: 31 ... 52 */
void sh_compat_var (const char *value);	/* BASH_COMPAT was set or unset */
#define O(name)	opt_get(name)

void sh_setvar (const char *name, const char *value);
char *sh_find_command (const char *name);	/* native path or NULL */
void sh_hash_clear (void);
void sh_hash_list (int fd);
int sh_hash_add (const char *name);
int sh_run_string (const char *text, const char *name, int line0);
int sh_source (const char *native, int must_exist, char **args, int nargs);
int sh_run_script (const char *native);	/* the main script: return does not end it */
void source_push (const char *name);	/* BASH_SOURCE */
void source_pop (void);
const char *source_top (void);
int sh_run_node (Node *n);
int sh_eval_argv (int argc, char **argv, int in, int out, int err, int flags);
#define EX_NOFUNC	1	/* 'command': skip functions */
#define EX_NOALIAS	2
#define EX_BUILTIN_ONLY	4	/* 'builtin' */
char *sh_capture (const char *src, size_t *len);	/* $( ) */
char *sh_procsubst (const char *src, int write);	/* <( ) >( ): a path */
void *sh_procsubst_mark (void);	/* cleanup_to(mark): only what came after */
void sh_procsubst_cleanup_to (void *mark);
int sh_dev_fd (const char *path);	/* /dev/stdin, /dev/fd/N ...: N, else -1 */
char *sh_fd_to_file (int k);	/* fd k read to its end into a temporary file */
void sh_procsubst_cleanup (void);
void sh_error (const char *fmt, ...);	/* "mmc: line N: ..." */
void sh_run_traps (void);
void sh_exit_now (int status);	/* EXIT trap, history, bye */
void sh_before_command (char **argv, int argc);
int sh_errexit_check (int status);
void sh_set_lineno (int line);
void sh_init_fds (void);
int sh_can_return (void);	/* inside a function or sourced file? */
void sh_do_return (int status);
void sh_do_break (int n, int is_continue);
int sh_loop_depth (void);
void sh_main_args (void);	/* BASH_ARGV and BASH_ARGC of the script */
void sh_args_touch (void);	/* first use of BASH_ARGV outside a function */
int sh_stage_main (const char *file, long pid);	/* mmc --stage */

/* functions */
typedef struct Func {
  char *name;
  Node *body;
  char *src;
  int flags;	/* V_EXPORT, V_READONLY */
  struct Func *next;
} Func;
Func *func_find (const char *name);
void func_define (const char *name, Node *body, const char *src);
int func_unset (const char *name);
void func_names (Vec *out);
char *func_pretty (Func *f);	/* the way bash prints it */
void func_env (Vec *out);	/* export -f: BASH_FUNC_name%%=() { ... }, like bash */
void func_import_env (void);	/* and the ones a parent shell exported */
int func_call (Func *f, int argc, char **argv);

/* traps */
int trap_set (const char *sig, const char *action);	/* NULL: default */
const char *trap_get (int sig);
int trap_signum (const char *name);	/* -1 unknown; 0 EXIT; 65 ERR ... */
const char *trap_signame (int sig);
#define TRAP_EXIT	0
#define TRAP_ERR	65
#define TRAP_DEBUG	66
#define TRAP_RETURN	67
#define TRAP_MAX	68

/* directory stack */
extern Vec sh_dirstack;

/* }================================================================== */


/*
** {==================================================================
** mjobs.c - background jobs
** ===================================================================
*/

typedef struct Job {
  int id;
  long pid;	/* last process of the pipeline */
  OsProc *procs;
  long *pids;
  int nprocs, running;
  int status;	/* when done */
  int notified;
  int stopped;	/* Ctrl-Z, kill -STOP: fg and bg let it go on */
  long pgid;	/* its process group (job control) */
  char *cmd;
} Job;

Job *job_add (const char *cmd, OsProc *procs, long *pids, int n);
Job *job_stopped (const char *cmd, OsProc *procs, long *pids, int n, long pgid);	/* Ctrl-Z */
int job_stopped_count (void);
void job_hup_stopped (void);	/* the shell goes: stopped jobs get HUP and CONT, like bash */
Job *job_find (const char *spec);	/* %1 %+ %- %name pid */
Job *job_by_pid (long pid);
int job_wait (Job *j);	/* status of the last process */
int job_wait_any (void);
int job_wait_any_p (const char *pvar);	/* wait -n -p NAME */
void job_poll (int report);	/* collect finished ones */
void job_list (int fd, int mode);	/* 0 jobs, 1 -l, 2 -p */
void job_remove (Job *j);
int job_count (void);
void job_hup_all (void);	/* shopt -s huponexit */
int job_status_of_pid (long pid, int *status);	/* for wait PID */

/* }================================================================== */


/*
** {==================================================================
** mbuiltin.c and friends - builtin commands and aliases
** ===================================================================
*/

typedef int (*BuiltinFn) (int argc, char **argv, int in, int out, int err);

#define B_FALLBACK	1	/* only used when no external command is found */
#define B_SPECIAL	2	/* POSIX special builtin */
#define B_DECL		4	/* declaration builtin: a=(..) arguments */
#define B_WINSYS	8	/* fallback that beats Windows' own program (find, sort) */

typedef struct Builtin {
  const char *name;
  BuiltinFn fn;
  int flags;
  const char *help;
} Builtin;

const Builtin *builtin_find (const char *name, int fallback);
const Builtin *builtin_fallback (const char *name);	/* "rm", "/bin/rm" ... */
void builtin_names (Vec *out);
int builtin_enabled (const char *name);
const char *alias_get (const char *name);
void alias_names (Vec *out);
void *alias_save (void);
void alias_restore (void *saved);
void alias_dump (Buf *b);
char *path_spell (const char *path);	/* cdspell, dirspell: NULL if nothing to fix */

/* each file registers its builtins in mbuiltin.c's table */
int b_test (int argc, char **argv, int in, int out, int err);
int b_bracket (int argc, char **argv, int in, int out, int err);
int cond_eval (Node *n);	/* [[ ]] */
int test_unary (const char *op, const char *arg);	/* -1: not an operator */
int test_binary (const char *l, const char *op, const char *r, int *err);

int b_echo (int argc, char **argv, int in, int out, int err);
int b_printf (int argc, char **argv, int in, int out, int err);
int b_read (int argc, char **argv, int in, int out, int err);
int b_mapfile (int argc, char **argv, int in, int out, int err);

int b_declare (int argc, char **argv, int in, int out, int err);
int b_local (int argc, char **argv, int in, int out, int err);
int b_export (int argc, char **argv, int in, int out, int err);
int b_readonly (int argc, char **argv, int in, int out, int err);
int b_unset (int argc, char **argv, int in, int out, int err);
int b_set (int argc, char **argv, int in, int out, int err);
int b_shopt (int argc, char **argv, int in, int out, int err);
int b_shift (int argc, char **argv, int in, int out, int err);
int b_getopts (int argc, char **argv, int in, int out, int err);
int b_let (int argc, char **argv, int in, int out, int err);
void var_format (Buf *b, Var *v);	/* a declare -p line */
int assign_word (const char *word, int flags_on, int local);	/* NAME=v, a=(..) */

int b_jobs (int argc, char **argv, int in, int out, int err);
int b_wait (int argc, char **argv, int in, int out, int err);
int b_kill (int argc, char **argv, int in, int out, int err);
int b_fg (int argc, char **argv, int in, int out, int err);
int b_bg (int argc, char **argv, int in, int out, int err);
int b_disown (int argc, char **argv, int in, int out, int err);
int b_trap (int argc, char **argv, int in, int out, int err);
int b_times (int argc, char **argv, int in, int out, int err);
int b_umask (int argc, char **argv, int in, int out, int err);
int b_ulimit (int argc, char **argv, int in, int out, int err);
int b_suspend (int argc, char **argv, int in, int out, int err);

/* }================================================================== */


/*
** {==================================================================
** c*.c - the tools: everyday programs (ls cp rm grep sed sort tar awk
** ...) as fallbacks, for the PCs that do not have them (Windows)
** ===================================================================
*/

/* ctool.c: what every tool uses */
typedef struct Out {	/* buffered output; 'failed' once a write fails */
  int fd, failed;
  int line;	/* a terminal: each line goes out at once */
  size_t n;
  char buf[32768];
} Out;
void out_init (Out *o, int fd);
void out_putn (Out *o, const char *s, size_t n);
void out_puts (Out *o, const char *s);
void out_putc (Out *o, int c);
void out_printf (Out *o, const char *fmt, ...);
int out_flush (Out *o);	/* -1: a write failed */

typedef struct In {	/* buffered input, line by line */
  int fd, own, eof;
  char *buf;
  size_t cap, start, end;
} In;
void in_init (In *r, int fd, int own);
int in_open (In *r, const char *arg, int stdin_fd);	/* "-": stdin; -1: failed */
void in_close (In *r);
int in_line (In *r, char **line, size_t *len, int delim, int *had_delim);	/* 0: end */
long in_read (In *r, char *dst, size_t n);	/* raw bytes; 0: end */

/* GNU style options: -abc, -n5, -n 5, --long, --long=x, options after
** the operands; the operands end up in 'ops' */
typedef struct LongOpt {
  const char *name;
  int key;
  int arg;	/* 0 none, 1 required, 2 optional (--x=v only) */
} LongOpt;
typedef struct Opts {
  const char *tool;
  int argc, i, err, no_permute;
  char **argv;
  const char *cluster;	/* the rest of -abc */
  const char *arg;	/* the option's argument */
  Vec ops;	/* operands, in order */
} Opts;
#define OPT_HELP	(-2)
#define OPT_BAD		'?'
void opts_init (Opts *g, const char *tool, int argc, char **argv, int err);
int opts_next (Opts *g, const char *spec, const LongOpt *lo);	/* 0: done */
void opts_free (Opts *g);

void tool_err (int err, const char *tool, const char *fmt, ...);
int tool_help (int out, const char *tool);	/* the help line of the table */
int tool_stop (void);	/* Ctrl-C pressed */
int tool_utf8 (void);	/* characters are UTF-8 (not LC_ALL=C) */
int tool_ask (int in, int err, const char *fmt, ...);	/* "rm: remove 'x'? " y/n */
char *tool_join (const char *dir, const char *name);	/* "a" + "b" -> "a/b" */
void human_size (char *out, unsigned long long v, int si);	/* 1.5K 23M */
int parse_size (const char *s, long long *out);	/* 10 10k 5M 1G, b = 512 */
int is_dot_or_dotdot (const char *name);
char *glob_mark (const char *pat);	/* typed pattern (\x literal) -> marked, for pat_match */
void mode_string (char out[11], const OsStat *st);	/* drwxr-xr-x */
int parse_mode (const char *spec, unsigned old, int is_dir, unsigned *out);	/* 755, u+x ... */
void ls_long_line (Out *o, const char *name, const char *native, const OsStat *st,
                   int human);	/* for find -ls */
const char *tool_base (const char *path);	/* last part, after '/' or '\\' */
int tool_utf8_cols (const char *s, size_t n);	/* terminal columns */

/* carch.c: deflate / inflate (RFC 1951), gzip streams, CRC-32 */
typedef int (*SinkFn) (void *ctx, const unsigned char *p, size_t n);	/* 0 ok, -1 failed */
typedef long (*SourceFn) (void *ctx, unsigned char *p, size_t n);	/* 0 at the end */
typedef struct ISrc {	/* buffered input for inflate; what it reads too far goes back */
  SourceFn fn;
  void *ctx;
  unsigned char buf[65536];
  size_t pos, len;
  int eof;
  unsigned long long total;
} ISrc;
typedef struct Deflate Deflate;
typedef struct GzOut GzOut;
unsigned long crc32_update (unsigned long crc, const void *data, size_t n);
Deflate *deflate_new (int level, SinkFn sink, void *ctx);
int deflate_write (Deflate *d, const void *data, size_t n);
int deflate_end (Deflate *d);	/* finishes and frees */
void isrc_init (ISrc *s, SourceFn fn, void *ctx);
long isrc_fill (ISrc *s);
int isrc_byte (ISrc *s);	/* -1 at the end */
int inflate_stream (ISrc *src, SinkFn sink, void *ctx);	/* 0 ok, -1 bad data, -2 sink */
long fd_source (void *ctx, unsigned char *p, size_t n);	/* ctx: int *fd */
int fd_sink (void *ctx, const unsigned char *p, size_t n);
GzOut *gz_open (SinkFn sink, void *ctx, int level, const char *name, time_t mtime);
int gz_write (void *gz, const unsigned char *p, size_t n);
int gz_close (GzOut *g);
int gunzip_stream (ISrc *src, SinkFn sink, void *ctx);	/* 0 ok, -1 not gzip, -2 bad, -3 write */

/* the tools, registered in mbuiltin.c's table */
int t_ls (int argc, char **argv, int in, int out, int err);
int t_cat (int argc, char **argv, int in, int out, int err);
int t_mkdir (int argc, char **argv, int in, int out, int err);
int t_rmdir (int argc, char **argv, int in, int out, int err);
int t_rm (int argc, char **argv, int in, int out, int err);
int t_cp (int argc, char **argv, int in, int out, int err);
int t_mv (int argc, char **argv, int in, int out, int err);
int t_touch (int argc, char **argv, int in, int out, int err);
int t_ln (int argc, char **argv, int in, int out, int err);
int t_find (int argc, char **argv, int in, int out, int err);
int t_basename (int argc, char **argv, int in, int out, int err);
int t_dirname (int argc, char **argv, int in, int out, int err);
int t_readlink (int argc, char **argv, int in, int out, int err);
int t_realpath (int argc, char **argv, int in, int out, int err);
int t_head (int argc, char **argv, int in, int out, int err);
int t_tail (int argc, char **argv, int in, int out, int err);
int t_tee (int argc, char **argv, int in, int out, int err);
int t_cut (int argc, char **argv, int in, int out, int err);
int t_sort (int argc, char **argv, int in, int out, int err);
int t_uniq (int argc, char **argv, int in, int out, int err);
int t_wc (int argc, char **argv, int in, int out, int err);
int t_tr (int argc, char **argv, int in, int out, int err);
int t_seq (int argc, char **argv, int in, int out, int err);
int t_sleep (int argc, char **argv, int in, int out, int err);
int t_grep (int argc, char **argv, int in, int out, int err);
int t_egrep (int argc, char **argv, int in, int out, int err);
int t_fgrep (int argc, char **argv, int in, int out, int err);
int t_sed (int argc, char **argv, int in, int out, int err);
int t_chmod (int argc, char **argv, int in, int out, int err);
int t_du (int argc, char **argv, int in, int out, int err);
int t_df (int argc, char **argv, int in, int out, int err);
int t_file (int argc, char **argv, int in, int out, int err);
int t_uname (int argc, char **argv, int in, int out, int err);
int t_hostname (int argc, char **argv, int in, int out, int err);
int t_whoami (int argc, char **argv, int in, int out, int err);
int t_id (int argc, char **argv, int in, int out, int err);
int t_ps (int argc, char **argv, int in, int out, int err);
int t_watch (int argc, char **argv, int in, int out, int err);
int t_cal (int argc, char **argv, int in, int out, int err);
int t_diff (int argc, char **argv, int in, int out, int err);
int t_gzip (int argc, char **argv, int in, int out, int err);
int t_gunzip (int argc, char **argv, int in, int out, int err);
int t_zcat (int argc, char **argv, int in, int out, int err);
int t_tar (int argc, char **argv, int in, int out, int err);
int t_zip (int argc, char **argv, int in, int out, int err);
int t_unzip (int argc, char **argv, int in, int out, int err);
int t_awk (int argc, char **argv, int in, int out, int err);

/* }================================================================== */


/*
** {==================================================================
** mcomp.c - programmable completion (complete, compgen, compopt)
** ===================================================================
*/

/* the -o options of a completion rule, for whoever shows the candidates */
#define COMP_NOSPACE	0x01u
#define COMP_FILENAMES	0x02u
#define COMP_DIRNAMES	0x04u
#define COMP_DEFAULT	0x08u
#define COMP_BASHDEFAULT 0x10u
#define COMP_PLUSDIRS	0x20u
#define COMP_NOSORT	0x40u
#define COMP_NOQUOTE	0x80u

int comp_for_line (const char *line, size_t point, Vec *words, size_t cword,
                   const char *word, Vec *out, unsigned *opts);
void comp_actions (unsigned actions, const char *word, Vec *out);
int b_complete (int argc, char **argv, int in, int out, int err);
int b_compgen (int argc, char **argv, int in, int out, int err);
int b_compopt (int argc, char **argv, int in, int out, int err);

/* }================================================================== */


/*
** {==================================================================
** mline.c - line editor
** ===================================================================
*/

char *line_read (const char *prompt);	/* malloc'd; NULL on end of input */
char *line_read_init (const char *prompt, const char *init);	/* read -e -i */
char *line_read_raw (int fd, int delim, int nchars, int silent, int timeout_ms,
                     int *timed_out);	/* for 'read' */
void line_hist_load (const char *native);
void line_hist_add (const char *s);
void line_hist_clear (void);
void line_hist_delete (int index);
void line_hist_write (const char *native);
void line_hist_append (const char *native);
const char *line_hist_file (void);
const Vec *line_hist (void);
/* the words of a line and which one the cursor is in (for completion) */
void line_words_at (const char *line, size_t upto, Vec *out, size_t *cword);

/* }================================================================== */


/*
** {==================================================================
** mmc.c
** ===================================================================
*/

const char *mmc_home (void);	/* native path of the home folder */
const char *mmc_exe (void);	/* native path of this program */

/* }================================================================== */

#endif
