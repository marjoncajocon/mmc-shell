/*
** mcomp.c - programmable completion: complete, compgen, compopt
**
** What bash calls "progcomp": a command can say how its own arguments
** are completed, so that git's completion script gives branch names on
** Tab. A rule (a Comp below) is stored per command name; when Tab is
** pressed, comp_for_line() finds the rule, sets COMP_WORDS, COMP_CWORD,
** COMP_LINE and COMP_POINT, runs the function of -F (or -C, -W, -G ...)
** and reads the candidates back out of COMPREPLY.
*/

#include "mmc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* -a -b -c -d -e -f -g -j -k -s -u -v and the same as -A names */
#define AC_ALIAS	0x0001u
#define AC_BUILTIN	0x0002u
#define AC_COMMAND	0x0004u
#define AC_DIR		0x0008u
#define AC_EXPORT	0x0010u
#define AC_FILE		0x0020u
#define AC_GROUP	0x0040u
#define AC_JOB		0x0080u
#define AC_KEYWORD	0x0100u
#define AC_SERVICE	0x0200u
#define AC_USER		0x0400u
#define AC_VARIABLE	0x0800u
#define AC_FUNCTION	0x1000u
#define AC_SETOPT	0x2000u
#define AC_SHOPT	0x4000u
#define AC_SIGNAL	0x8000u

typedef struct Comp {
  char *name;	/* the command; NULL for the -D, -E and -I rules */
  int kind;	/* CK_* */
  char *func, *command, *words, *glob, *prefix, *suffix, *filter;
  unsigned actions, opts;
} Comp;

enum { CK_NAME, CK_DEFAULT, CK_EMPTY, CK_INITIAL };

static Comp *comps = NULL;
static size_t ncomps = 0;
static Comp *running = NULL;	/* the rule whose function is running (compopt) */


static void comp_clear (Comp *c) {
  free(c->name);
  free(c->func);
  free(c->command);
  free(c->words);
  free(c->glob);
  free(c->prefix);
  free(c->suffix);
  free(c->filter);
  memset(c, 0, sizeof(*c));
}


static Comp *comp_find (const char *name, int kind) {
  size_t i;
  for (i = 0; i < ncomps; i++) {
    if (comps[i].kind != kind) continue;
    if (kind != CK_NAME) return &comps[i];
    if (comps[i].name != NULL && strcmp(comps[i].name, name) == 0) return &comps[i];
  }
  return NULL;
}


static Comp *comp_add (const char *name, int kind) {
  Comp *c = comp_find(name, kind);
  if (c != NULL) {
    char *keep = c->name;
    c->name = NULL;
    comp_clear(c);
    c->name = keep;
    c->kind = kind;
    return c;
  }
  comps = (Comp *)xrealloc(comps, (ncomps + 1) * sizeof(Comp));
  c = &comps[ncomps++];
  memset(c, 0, sizeof(*c));
  c->name = name ? xstrdup(name) : NULL;
  c->kind = kind;
  return c;
}


static void comp_remove (const char *name, int kind) {
  Comp *c = comp_find(name, kind);
  size_t i;
  if (c == NULL) return;
  i = (size_t)(c - comps);
  comp_clear(c);
  memmove(comps + i, comps + i + 1, (ncomps - i - 1) * sizeof(Comp));
  ncomps--;
}


/*
** {==================================================================
** What the letters and -o words mean
** ===================================================================
*/

static unsigned action_letter (char c) {
  switch (c) {
    case 'a': return AC_ALIAS;
    case 'b': return AC_BUILTIN;
    case 'c': return AC_COMMAND;
    case 'd': return AC_DIR;
    case 'e': return AC_EXPORT;
    case 'f': return AC_FILE;
    case 'g': return AC_GROUP;
    case 'j': return AC_JOB;
    case 'k': return AC_KEYWORD;
    case 's': return AC_SERVICE;
    case 'u': return AC_USER;
    case 'v': return AC_VARIABLE;
    default: return 0;
  }
}


static unsigned action_name (const char *s) {
  static const struct { const char *name; unsigned bit; } names[] = {
    {"alias", AC_ALIAS}, {"arrayvar", AC_VARIABLE}, {"binding", 0},
    {"builtin", AC_BUILTIN}, {"command", AC_COMMAND}, {"directory", AC_DIR},
    {"disabled", AC_BUILTIN}, {"enabled", AC_BUILTIN}, {"export", AC_EXPORT},
    {"file", AC_FILE}, {"function", AC_FUNCTION}, {"group", AC_GROUP},
    {"helptopic", AC_BUILTIN}, {"hostname", 0}, {"job", AC_JOB},
    {"keyword", AC_KEYWORD}, {"running", AC_JOB}, {"service", AC_SERVICE},
    {"setopt", AC_SETOPT}, {"shopt", AC_SHOPT}, {"signal", AC_SIGNAL},
    {"stopped", AC_JOB}, {"user", AC_USER}, {"variable", AC_VARIABLE},
    {NULL, 0}
  };
  int i;
  for (i = 0; names[i].name; i++)
    if (strcmp(names[i].name, s) == 0) return names[i].bit;
  return 0;
}


static unsigned opt_word (const char *s) {
  if (strcmp(s, "nospace") == 0) return COMP_NOSPACE;
  if (strcmp(s, "filenames") == 0) return COMP_FILENAMES;
  if (strcmp(s, "dirnames") == 0) return COMP_DIRNAMES;
  if (strcmp(s, "default") == 0) return COMP_DEFAULT;
  if (strcmp(s, "bashdefault") == 0) return COMP_BASHDEFAULT;
  if (strcmp(s, "plusdirs") == 0) return COMP_PLUSDIRS;
  if (strcmp(s, "nosort") == 0) return COMP_NOSORT;
  if (strcmp(s, "noquote") == 0) return COMP_NOQUOTE;
  return 0;
}


static const char *opt_word_of (unsigned bit) {
  switch (bit) {
    case COMP_NOSPACE: return "nospace";
    case COMP_FILENAMES: return "filenames";
    case COMP_DIRNAMES: return "dirnames";
    case COMP_DEFAULT: return "default";
    case COMP_BASHDEFAULT: return "bashdefault";
    case COMP_PLUSDIRS: return "plusdirs";
    case COMP_NOSORT: return "nosort";
    case COMP_NOQUOTE: return "noquote";
    default: return NULL;
  }
}

/* }================================================================== */


/*
** {==================================================================
** Making the candidates
** ===================================================================
*/

static void push_unique (Vec *v, char *s) {
  size_t i;
  for (i = 0; i < v->n; i++)
    if (strcmp(v->v[i], s) == 0) {
      free(s);
      return;
    }
  vec_push(v, s);
}


/* the files of one directory that start with what was typed */
static void files_matching (const char *word, int dirs_only, Vec *out) {
  const char *slash = strrchr(word, '/');
  char *dirpart = slash ? xstrndup(word, (size_t)(slash + 1 - word)) : xstrdup("");
  const char *base = slash ? slash + 1 : word;
  char *lookup, *native;
  Vec files;
  size_t i;
  if (dirpart[0] == '~' && dirpart[1] == '/') {
    const char *home = var_get("HOME");
    lookup = xstrcat3(home ? home : "", dirpart + 1, "");
  }
  else lookup = xstrdup(dirpart[0] ? dirpart : ".");
  native = path_to_native(lookup);
  vec_init(&files);
  os_listdir(native, &files);
  for (i = 0; i < files.n; i++) {
    OsStat st;
    char *full;
    Buf b;
    if (m_fnncmp(files.v[i], base, strlen(base)) != 0) continue;
    if (files.v[i][0] == '.' && base[0] != '.') continue;
    full = path_join(native, files.v[i]);
    if (os_stat(full, &st) != 0) st.is_dir = 0;
    free(full);
    if (dirs_only && !st.is_dir) continue;
    buf_init(&b);
    buf_puts(&b, dirpart);
    buf_puts(&b, files.v[i]);
    if (st.is_dir) buf_putc(&b, '/');
    push_unique(out, buf_take(&b));
  }
  vec_free(&files);
  free(dirpart);
  free(lookup);
  free(native);
}


/* the programs in the PATH, without their extension on Windows */
static void commands_matching (const char *word, Vec *out) {
  Vec dirs;
  char *path = var_get("PATH") ? xstrdup(var_get("PATH")) : NULL;
  size_t i, k;
  vec_init(&dirs);
  if (path) path_list_split(path, &dirs);
  for (i = 0; i < dirs.n; i++) {
    Vec files;
    char *dir = path_to_native(dirs.v[i]);
    vec_init(&files);
    os_listdir(dir, &files);
    for (k = 0; k < files.n; k++) {
      char *name = files.v[k];
#ifdef _WIN32
      char *dot = strrchr(name, '.');
      if (dot == NULL || (m_stricmp(dot, ".exe") != 0 && m_stricmp(dot, ".cmd") != 0 &&
          m_stricmp(dot, ".bat") != 0 && m_stricmp(dot, ".com") != 0))
        continue;
      *dot = '\0';
#endif
      if (m_fnncmp(name, word, strlen(word)) != 0) continue;
      push_unique(out, xstrdup(name));
    }
    free(dir);
    vec_free(&files);
  }
  free(path);
  vec_free(&dirs);
}


/* everything the -a -b -c ... letters and -A names ask for */
void comp_actions (unsigned actions, const char *word, Vec *out) {
  Vec all;
  size_t i;
  size_t wlen = strlen(word);
  vec_init(&all);
  if (actions & AC_ALIAS) alias_names(&all);
  if (actions & AC_BUILTIN) builtin_names(&all);
  if (actions & AC_FUNCTION) func_names(&all);
  if (actions & AC_VARIABLE) var_names(&all, "", 1);
  if (actions & AC_EXPORT) var_names(&all, "", 0);
  if (actions & AC_COMMAND) {
    builtin_names(&all);
    alias_names(&all);
    func_names(&all);
    commands_matching(word, out);
  }
  if (actions & AC_KEYWORD) {
    static const char *const kws[] = {"if", "then", "else", "elif", "fi", "case",
      "esac", "for", "select", "while", "until", "do", "done", "in", "function",
      "time", "{", "}", "!", "[[", "]]", "coproc", NULL};
    int j;
    for (j = 0; kws[j]; j++) vec_push(&all, xstrdup(kws[j]));
  }
  if (actions & AC_SETOPT) {
    const ShOpt *o;
    for (o = sh_opts; o->name; o++)
      if (!o->shopt) vec_push(&all, xstrdup(o->name));
  }
  if (actions & AC_SHOPT) {
    const ShOpt *o;
    for (o = sh_opts; o->name; o++)
      if (o->shopt) vec_push(&all, xstrdup(o->name));
  }
  if (actions & AC_SIGNAL) {
    int k;
    for (k = 1; k < 32; k++) {
      Buf b;
      buf_init(&b);
      buf_puts(&b, "SIG");
      buf_puts(&b, trap_signame(k));
      vec_push(&all, buf_take(&b));
    }
  }
  if (actions & AC_USER) {
    const char *u = var_get("USER");
    if (u != NULL) vec_push(&all, xstrdup(u));
  }
  if (actions & (AC_FILE | AC_DIR)) files_matching(word, (actions & AC_FILE) == 0, out);
  for (i = 0; i < all.n; i++)
    if (strncmp(all.v[i], word, wlen) == 0) push_unique(out, xstrdup(all.v[i]));
  vec_free(&all);
}


/* -X: drop what matches the pattern, keep what matches a !pattern */
static void apply_filter (Vec *v, const char *filter, const char *word) {
  Buf pat;
  size_t i, k = 0;
  int negate = 0;
  const char *p = filter;
  if (filter == NULL || filter[0] == '\0') return;
  if (*p == '!') {
    negate = 1;
    p++;
  }
  buf_init(&pat);
  for (; *p != '\0'; p++) {	/* & in the pattern means the word typed */
    if (*p == '&') buf_puts(&pat, word);
    else if (*p == '\\' && p[1] == '&') buf_putc(&pat, *++p);
    else buf_putc(&pat, *p);
  }
  for (i = 0; i < v->n; i++) {
    int hit = pat_match(pat.s ? pat.s : "", v->v[i], PM_EXTGLOB);
    if (negate) hit = !hit;
    if (hit) free(v->v[i]);	/* it goes */
    else v->v[k++] = v->v[i];
  }
  v->n = k;
  if (v->v != NULL) v->v[k] = NULL;
  buf_free(&pat);
}


static void apply_fix (Vec *v, const char *prefix, const char *suffix) {
  size_t i;
  if ((prefix == NULL || prefix[0] == '\0') && (suffix == NULL || suffix[0] == '\0')) return;
  for (i = 0; i < v->n; i++) {
    Buf b;
    buf_init(&b);
    if (prefix) buf_puts(&b, prefix);
    buf_puts(&b, v->v[i]);
    if (suffix) buf_puts(&b, suffix);
    free(v->v[i]);
    v->v[i] = buf_take(&b);
  }
}


/* runs the -F function, or the -C command, and collects what comes back */
static void run_generators (const Comp *c, const char *word, const char *cmd,
                            const char *prev, Vec *out) {
  if (c->func != NULL && c->func[0] != '\0') {
    Func *f = func_find(c->func);
    if (f != NULL) {
      char *argv[4];
      size_t i, n;
      Comp *outer = running;
      argv[0] = c->func;
      argv[1] = (char *)cmd;
      argv[2] = (char *)word;
      argv[3] = (char *)prev;
      var_make_array("COMPREPLY", 0);
      running = (Comp *)c;
      func_call(f, 4, argv);
      running = outer;
      n = var_count("COMPREPLY");
      for (i = 0; i < n; i++) {
        const char *v = var_aget("COMPREPLY", (long long)i);
        if (v != NULL) vec_push(out, xstrdup(v));
      }
    }
  }
  if (c->command != NULL && c->command[0] != '\0') {
    Buf line;
    char *text;
    size_t len = 0;
    buf_init(&line);
    buf_puts(&line, c->command);
    buf_putc(&line, ' ');
    buf_puts(&line, cmd ? cmd : "");
    buf_putc(&line, ' ');
    buf_puts(&line, word);
    buf_putc(&line, ' ');
    buf_puts(&line, prev ? prev : "");
    text = sh_capture(line.s ? line.s : "", &len);
    buf_free(&line);
    if (text != NULL) {
      char *p = text;
      while (*p != '\0') {
        size_t n = strcspn(p, "\n");
        if (n > 0) vec_push(out, xstrndup(p, n));
        p += n;
        if (*p == '\n') p++;
      }
      free(text);
    }
  }
}


/*
** Every candidate of one rule, already filtered by what was typed.
** 'opts' gets the -o options that the caller has to honour.
*/
static void comp_run (const Comp *c, const char *word, const char *cmd,
                      const char *prev, Vec *out, unsigned *opts) {
  Vec raw;
  size_t i;
  vec_init(&raw);
  run_generators(c, word, cmd, prev, &raw);
  if (c->words != NULL) {	/* -W: split on IFS, each part expanded, then matched */
    const char *ifs = var_get("IFS");
    const char *p = c->words;
    if (ifs == NULL) ifs = " \t\n";
    while (*p != '\0') {
      Buf one;
      Vec parts;
      size_t k;
      while (*p != '\0' && strchr(ifs, *p) != NULL) p++;
      if (*p == '\0') break;
      buf_init(&one);
      while (*p != '\0' && strchr(ifs, *p) == NULL) {
        if (*p == '\'' || *p == '"') {	/* "a b" stays one candidate */
          char q = *p;
          buf_putc(&one, *p++);
          while (*p != '\0' && *p != q) buf_putc(&one, *p++);
          if (*p != '\0') buf_putc(&one, *p++);
        }
        else if (*p == '\\' && p[1] != '\0') {
          buf_putc(&one, *p++);
          buf_putc(&one, *p++);
        }
        else buf_putc(&one, *p++);
      }
      vec_init(&parts);
      expand_word(one.s ? one.s : "", &parts);
      for (k = 0; k < parts.n; k++)
        if (strncmp(parts.v[k], word, strlen(word)) == 0) vec_push(&raw, xstrdup(parts.v[k]));
      vec_free(&parts);
      buf_free(&one);
    }
  }
  if (c->glob != NULL) {	/* -G: a pathname pattern */
    Vec files;
    vec_init(&files);
    expand_word(c->glob, &files);
    for (i = 0; i < files.n; i++)
      if (strncmp(files.v[i], word, strlen(word)) == 0) vec_push(&raw, xstrdup(files.v[i]));
    vec_free(&files);
  }
  if (c->actions != 0) comp_actions(c->actions, word, &raw);
  apply_filter(&raw, c->filter, word);
  apply_fix(&raw, c->prefix, c->suffix);
  if (c->opts & COMP_PLUSDIRS) files_matching(word, 1, &raw);
  for (i = 0; i < raw.n; i++) push_unique(out, xstrdup(raw.v[i]));
  vec_free(&raw);
  *opts = c->opts;
}

/* }================================================================== */


/*
** {==================================================================
** What the line editor asks for
** ===================================================================
*/

/*
** Tab was pressed. 'words' are the words of the line so far, 'cword'
** which of them is being completed, 'word' what is typed of it.
** Returns 0 when no rule applies and the editor should do its own
** thing, else 1 with the candidates in 'out'.
*/
int comp_for_line (const char *line, size_t point, Vec *words, size_t cword,
                   const char *word, Vec *out, unsigned *opts) {
  const Comp *c = NULL;
  const char *cmd = words->n > 0 ? words->v[0] : "";
  const char *prev = (cword > 0 && cword - 1 < words->n) ? words->v[cword - 1] : "";
  char num[24];
  size_t i;
  *opts = 0;
  if (!opt_get("progcomp")) return 0;
  if (words->n == 0 || (words->n == 1 && cword == 0 && word[0] == '\0'))
    c = comp_find(NULL, CK_EMPTY);
  if (c == NULL && cword == 0) c = comp_find(NULL, CK_INITIAL);
  if (c == NULL && cword > 0) {
    c = comp_find(cmd, CK_NAME);
    if (c == NULL) {	/* /usr/bin/git also uses the rule of git */
      const char *slash = strrchr(cmd, '/');
      if (slash != NULL) c = comp_find(slash + 1, CK_NAME);
    }
    if (c == NULL) c = comp_find(NULL, CK_DEFAULT);
  }
  if (c == NULL) return 0;
  var_set("COMP_LINE", line);
  var_set("COMP_POINT", ll_to_str((long long)point, num));
  var_set("COMP_TYPE", ll_to_str(9, num));	/* Tab */
  var_set("COMP_KEY", ll_to_str(9, num));
  var_make_array("COMP_WORDS", 0);
  for (i = 0; i < words->n; i++)
    var_aset("COMP_WORDS", (long long)i, words->v[i]);
  var_set("COMP_CWORD", ll_to_str((long long)cword, num));
  comp_run(c, word, cmd, prev, out, opts);
  if (out->n == 0 && (*opts & (COMP_DEFAULT | COMP_BASHDEFAULT)) != 0) return 0;
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** The builtins: complete, compgen, compopt
** ===================================================================
*/

/* the options complete and compgen share; returns -1 on a bad one */
static int parse_spec (Comp *c, int argc, char **argv, int *first_operand,
                       int *kinds, int *remove_it, int *print_it) {
  int i;
  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *val = NULL;
    if (a[0] != '-' || a[1] == '\0') break;
    if (strcmp(a, "--") == 0) {
      i++;
      break;
    }
    if (strchr("FCWGPSXAo", a[1]) != NULL && a[2] == '\0') {
      if (i + 1 >= argc) {
        sh_error("complete: -%c: option requires an argument", a[1]);
        return -1;
      }
      val = argv[++i];
    }
    switch (a[1]) {
      case 'F': free(c->func); c->func = xstrdup(val); break;
      case 'C': free(c->command); c->command = xstrdup(val); break;
      case 'W': free(c->words); c->words = xstrdup(val); break;
      case 'G': free(c->glob); c->glob = xstrdup(val); break;
      case 'P': free(c->prefix); c->prefix = xstrdup(val); break;
      case 'S': free(c->suffix); c->suffix = xstrdup(val); break;
      case 'X': free(c->filter); c->filter = xstrdup(val); break;
      case 'A': {
        unsigned bit = action_name(val);
        if (bit == 0 && strcmp(val, "binding") != 0 && strcmp(val, "hostname") != 0) {
          sh_error("complete: %s: invalid action name", val);
          return -1;
        }
        c->actions |= bit;
        break;
      }
      case 'o': {
        unsigned bit = opt_word(val);
        if (bit == 0) {
          sh_error("complete: %s: invalid option name", val);
          return -1;
        }
        c->opts |= bit;
        break;
      }
      case 'D': *kinds = CK_DEFAULT; break;
      case 'E': *kinds = CK_EMPTY; break;
      case 'I': *kinds = CK_INITIAL; break;
      case 'r': if (remove_it) *remove_it = 1; break;
      case 'p': if (print_it) *print_it = 1; break;
      default: {
        const char *p;
        for (p = a + 1; *p != '\0'; p++) {
          unsigned bit = action_letter(*p);
          if (bit == 0) {
            sh_error("complete: -%c: invalid option", *p);
            return -1;
          }
          c->actions |= bit;
        }
      }
    }
  }
  *first_operand = i;
  return 0;
}


static void print_spec (int out, const Comp *c) {
  Buf b;
  buf_init(&b);
  static const unsigned order[] = {	/* bash prints them in this order */
    COMP_BASHDEFAULT, COMP_DEFAULT, COMP_DIRNAMES, COMP_FILENAMES,
    COMP_NOQUOTE, COMP_NOSORT, COMP_NOSPACE, COMP_PLUSDIRS
  };
  int oi;
  buf_puts(&b, "complete");
  for (oi = 0; oi < (int)(sizeof(order) / sizeof(order[0])); oi++) {
    const char *w = opt_word_of(c->opts & order[oi]);
    if (w != NULL) {
      buf_puts(&b, " -o ");
      buf_puts(&b, w);
    }
  }
  {
    static const char letters[] = "abcdefgjksuv";
    int i;
    for (i = 0; letters[i] != '\0'; i++)
      if (c->actions & action_letter(letters[i])) {
        buf_puts(&b, " -");
        buf_putc(&b, letters[i]);
      }
  }
  if (c->func) {
    buf_puts(&b, " -F ");
    buf_puts(&b, c->func);
  }
  if (c->command) {
    buf_puts(&b, " -C '");
    buf_puts(&b, c->command);
    buf_putc(&b, '\'');
  }
  if (c->words) {
    buf_puts(&b, " -W '");
    buf_puts(&b, c->words);
    buf_putc(&b, '\'');
  }
  if (c->glob) {
    buf_puts(&b, " -G '");
    buf_puts(&b, c->glob);
    buf_putc(&b, '\'');
  }
  if (c->prefix) {
    buf_puts(&b, " -P '");
    buf_puts(&b, c->prefix);
    buf_putc(&b, '\'');
  }
  if (c->suffix) {
    buf_puts(&b, " -S '");
    buf_puts(&b, c->suffix);
    buf_putc(&b, '\'');
  }
  if (c->filter) {
    buf_puts(&b, " -X '");
    buf_puts(&b, c->filter);
    buf_putc(&b, '\'');
  }
  if (c->kind == CK_DEFAULT) buf_puts(&b, " -D");
  else if (c->kind == CK_EMPTY) buf_puts(&b, " -E");
  else if (c->kind == CK_INITIAL) buf_puts(&b, " -I");
  else if (c->name != NULL) {
    buf_putc(&b, ' ');
    buf_puts(&b, c->name);
  }
  buf_putc(&b, '\n');
  fd_puts(out, b.s ? b.s : "");
  buf_free(&b);
}


int b_complete (int argc, char **argv, int in, int out, int err) {
  Comp spec;
  int first = argc, kind = CK_NAME, remove_it = 0, print_it = 0, i, status = 0;
  (void)in; (void)err;
  memset(&spec, 0, sizeof(spec));
  if (parse_spec(&spec, argc, argv, &first, &kind, &remove_it, &print_it) != 0) {
    comp_clear(&spec);
    return 1;
  }
  if (remove_it) {
    if (first >= argc) {	/* complete -r: all of them */
      size_t k;
      for (k = 0; k < ncomps; k++) comp_clear(&comps[k]);
      ncomps = 0;
    }
    else
      for (i = first; i < argc; i++) comp_remove(argv[i], kind);
    comp_clear(&spec);
    return 0;
  }
  if (print_it || (first >= argc && spec.func == NULL && spec.command == NULL &&
                   spec.words == NULL && spec.glob == NULL && spec.actions == 0 &&
                   spec.opts == 0 && kind == CK_NAME)) {
    size_t k;
    if (first < argc) {
      for (i = first; i < argc; i++) {
        const Comp *c = comp_find(argv[i], CK_NAME);
        if (c == NULL) {
          sh_error("complete: %s: no completion specification", argv[i]);
          status = 1;
        }
        else print_spec(out, c);
      }
    }
    else
      for (k = 0; k < ncomps; k++) print_spec(out, &comps[k]);
    comp_clear(&spec);
    return status;
  }
  if (kind != CK_NAME) {
    Comp *c = comp_add(NULL, kind);
    char *keep = c->name;
    *c = spec;
    c->name = keep;
    c->kind = kind;
    return 0;
  }
  if (first >= argc) {
    sh_error("complete: usage: complete [-abcdefgjksuv] [-o option] "
             "[-DEI] [-A action] [-F function] [-C command] [-G glob] "
             "[-W wordlist] [-P prefix] [-S suffix] [-X filter] name ...");
    comp_clear(&spec);
    return 2;
  }
  for (i = first; i < argc; i++) {
    Comp *c = comp_add(argv[i], CK_NAME);
    char *keep = c->name;
    *c = spec;
    c->name = keep;
    c->kind = CK_NAME;
    if (i + 1 < argc) {	/* every name gets its own copy */
      spec.func = spec.func ? xstrdup(spec.func) : NULL;
      spec.command = spec.command ? xstrdup(spec.command) : NULL;
      spec.words = spec.words ? xstrdup(spec.words) : NULL;
      spec.glob = spec.glob ? xstrdup(spec.glob) : NULL;
      spec.prefix = spec.prefix ? xstrdup(spec.prefix) : NULL;
      spec.suffix = spec.suffix ? xstrdup(spec.suffix) : NULL;
      spec.filter = spec.filter ? xstrdup(spec.filter) : NULL;
    }
  }
  return 0;
}


int b_compgen (int argc, char **argv, int in, int out, int err) {
  Comp spec;
  Vec cands;
  int first = argc, kind = CK_NAME, i;
  unsigned opts = 0;
  const char *word = "";
  size_t k;
  (void)in; (void)err;
  memset(&spec, 0, sizeof(spec));
  if (parse_spec(&spec, argc, argv, &first, &kind, NULL, NULL) != 0) {
    comp_clear(&spec);
    return 1;
  }
  if (first < argc) word = argv[first];
  vec_init(&cands);
  comp_run(&spec, word, argv[0], "", &cands, &opts);	/* in the order they came */
  for (k = 0; k < cands.n; k++) fd_printf(out, "%s\n", cands.v[k]);
  i = (cands.n > 0) ? 0 : 1;
  vec_free(&cands);
  comp_clear(&spec);
  return i;
}


int b_compopt (int argc, char **argv, int in, int out, int err) {
  int i, status = 0;
  Comp *target = running;
  (void)in; (void)err;
  for (i = 1; i < argc; i++) {	/* a name after the options picks that rule */
    if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "+o") == 0) i++;
    else if (argv[i][0] != '-' && argv[i][0] != '+') {
      Comp *c = comp_find(argv[i], CK_NAME);
      if (c == NULL) {
        sh_error("compopt: %s: no completion specification", argv[i]);
        return 1;
      }
      target = c;
    }
  }
  if (target == NULL) {
    sh_error("compopt: not now: no completion is running");
    return 1;
  }
  for (i = 1; i < argc; i++) {
    int on = (argv[i][0] == '-');
    if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "+o") == 0) && i + 1 < argc) {
      unsigned bit = opt_word(argv[++i]);
      if (bit == 0) {
        sh_error("compopt: %s: invalid option name", argv[i]);
        status = 1;
        continue;
      }
      if (on) target->opts |= bit;
      else target->opts &= ~bit;
    }
    else if (strcmp(argv[i], "-D") == 0 || strcmp(argv[i], "-E") == 0 ||
             strcmp(argv[i], "-I") == 0) {
      /* which rule: already taken care of above */
    }
  }
  if (argc == 1) {	/* compopt alone: show what is set */
    unsigned bit;
    for (bit = 1; bit != 0; bit <<= 1) {
      const char *w = opt_word_of(target->opts & bit);
      if (w != NULL) fd_printf(out, "compopt -o %s\n", w);
    }
  }
  return status;
}

/* }================================================================== */
