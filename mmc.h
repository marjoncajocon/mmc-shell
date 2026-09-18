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
#define MMC_VERSION	"0.1.0"
#define MMC_AUTHOR	"Marjon Mangindo Cajocon"

#define MMC_HISTORY_MAX	1000
#define MMC_SOURCE_DEPTH	32
#define MMC_ALIAS_DEPTH	16

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
char *buf_take (Buf *b);
void buf_free (Buf *b);

void vec_init (Vec *v);
void vec_push (Vec *v, char *s);
void vec_insert (Vec *v, size_t at, char *s);
void vec_sort (Vec *v);
void vec_free (Vec *v);

int fd_puts (int fd, const char *s);
int fd_printf (int fd, const char *fmt, ...);

int m_stricmp (const char *a, const char *b);
int m_strnicmp (const char *a, const char *b, size_t n);
int m_fncmp (const char *a, const char *b);	/* file-name compare */
int m_fnncmp (const char *a, const char *b, size_t n);
int m_envcmp (const char *a, const char *b);	/* env-name compare */
size_t utf8_count (const char *s, size_t nbytes);
char *ll_to_str (long long v, char *out);	/* out: >= 24 bytes */

char *read_file (const char *native, size_t *len);
int mkdir_p (const char *native);

/* }================================================================== */


/*
** {==================================================================
** mos.c - operating system layer
** ===================================================================
*/

typedef intptr_t OsProc;

typedef struct OsStat {
  int exists, is_dir;
  long long size;
  time_t mtime;
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
int os_is_exec (const char *native);
int os_listdir (const char *native, Vec *out);
int os_mkdir (const char *native);

#define OS_READ		0
#define OS_WRITE	1
#define OS_APPEND	2
int os_open (const char *native, int mode);
int os_pipe (int fds[2]);
int os_dup (int fd);
void os_close (int fd);
long os_read (int fd, void *buf, size_t n);
long os_write (int fd, const void *buf, size_t n);

int os_is_tty (int fd);
int os_tty_raw (int on);
int os_tty_getbyte (void);
void os_tty_fix (void);
int os_term_cols (void);

char *os_exe_path (const char *argv0);
char *os_hostname (void);
char *os_username (void);
long os_getpid (void);

int os_spawn (const char *exe, char **argv, int in, int out, int err,
              OsProc *proc, long *pid);
int os_wait (OsProc proc);
void os_detach (OsProc proc);
void os_reap (void);

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

/* }================================================================== */


/*
** {==================================================================
** mlex.c - tokenizer
** ===================================================================
*/

enum {
  T_WORD, T_PIPE, T_AND, T_OR, T_SEMI, T_BG,
  T_LT,		/* <     */
  T_GT,		/* >     */
  T_GTGT,	/* >>    */
  T_ERR,	/* 2>    */
  T_ERRAPP,	/* 2>>   */
  T_ERROUT,	/* 2>&1  */
  T_OUTERR,	/* >&2   */
  T_BOTH	/* &>    */
};

typedef struct Token {
  int type;
  char *text;	/* raw word (quotes kept), NULL for operators */
  int depth;	/* alias expansion depth; -1: do not alias-expand */
} Token;

typedef struct TokVec {
  Token *v;
  size_t n, cap;
} TokVec;

void tok_init (TokVec *t);
void tok_push (TokVec *t, int type, char *text, int depth);
void tok_free (TokVec *t);
int lex_line (const char *s, TokVec *out);

/* }================================================================== */


/*
** {==================================================================
** mexpand.c - word expansion and globbing
** ===================================================================
*/

size_t word_assign_pos (const char *w);	/* index of '=' in NAME=..., or 0 */
int expand_word (const char *raw, Vec *out);
char *expand_str (const char *raw);

/* }================================================================== */


/*
** {==================================================================
** mexec.c - running commands
** ===================================================================
*/

extern int sh_status;		/* $? */
extern int sh_exit;		/* set by 'exit' */
extern int sh_interactive;
extern Vec sh_args;		/* $0 $1 ... */

void sh_setvar (const char *name, const char *value);
char *sh_find_command (const char *name);	/* native path or NULL */
int sh_run_line (const char *line);
int sh_source (const char *native, int must_exist);

/* }================================================================== */


/*
** {==================================================================
** mbuiltin.c - builtin commands and aliases
** ===================================================================
*/

typedef int (*BuiltinFn) (int argc, char **argv, int in, int out, int err);

typedef struct Builtin {
  const char *name;
  BuiltinFn fn;
  int fallback;	/* only used when no external command is found */
  const char *help;
} Builtin;

const Builtin *builtin_find (const char *name, int fallback);
void builtin_names (Vec *out);
const char *alias_get (const char *name);
void alias_names (Vec *out);

/* }================================================================== */


/*
** {==================================================================
** mline.c - line editor
** ===================================================================
*/

char *line_read (const char *prompt);	/* malloc'd; NULL on end of input */
void line_hist_load (const char *native);
void line_hist_add (const char *s);
void line_hist_clear (void);
const Vec *line_hist (void);

/* }================================================================== */

#endif
