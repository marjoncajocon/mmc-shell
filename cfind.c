/*
** cfind.c - find: walks folders and runs an expression on every entry
**
**   find [-H] [-L] [-P] [path...] [expression]
** Options: -maxdepth -mindepth -depth -follow -daystart -xdev -regextype
** Tests:   -name -iname -path -ipath -wholename -regex -iregex -type
**          -size -empty -mtime -mmin -atime -amin -ctime -cmin -newer
**          -perm -user -group -uid -gid -links -readable -writable
**          -executable -samefile -lname -ilname -true -false
** Actions: -print -print0 -printf -ls -delete -exec -execdir -ok -prune
**          -quit
** Operators: ( ) ! -not -a -and -o -or ,
*/

#include "mmc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


enum {
  F_AND, F_OR, F_NOT, F_COMMA, F_TRUE, F_FALSE,
  F_NAME, F_PATH, F_REGEX, F_LNAME, F_TYPE, F_SIZE, F_EMPTY, F_TIME, F_NEWER,
  F_PERM, F_USER, F_GROUP, F_UID, F_GID, F_LINKS, F_ACCESS, F_SAMEFILE,
  F_PRINT, F_PRINT0, F_PRINTF, F_LS, F_DELETE, F_EXEC, F_PRUNE, F_QUIT
};

typedef struct FNode {
  int kind;
  struct FNode *a, *b;
  char *str;	/* pattern (marked), name, format, file */
  Regex *re;
  int icase;
  int cmp;	/* '+' more, '-' less, '=' exactly */
  long long num, unit;
  int which;	/* F_TIME: 'm' 'a' 'c'; F_ACCESS: 'r' 'w' 'x'; F_PERM: '=' '-' '/' */
  unsigned mode;
  time_t when;
  Vec argv;	/* -exec */
  int plus, dir, ask;
  Vec batch;	/* -exec ... {} + */
  char *batch_dir;
} FNode;

typedef struct Ent {
  const char *disp;	/* the path as shown */
  const char *native;
  const char *start;	/* the starting point it came from */
  OsStat st;
  int depth;
} Ent;

typedef struct Find {
  int follow;	/* 0 -P, 1 -L, 2 -H */
  int maxdepth, mindepth, depth_first, daystart, quit, status, prune;
  int in, out, err;
  time_t now;
  Out *o;
  FNode *root;
  FNode **execs;	/* the {} + ones, run at the end */
  int nexecs;
  char **args;
  int nargs, pos;
  int regex_ere;
} Find;


static FNode *fnode (int kind) {
  FNode *n = (FNode *)xmalloc(sizeof(FNode));
  memset(n, 0, sizeof(*n));
  n->kind = kind;
  vec_init(&n->argv);
  vec_init(&n->batch);
  return n;
}


static void fnode_free (FNode *n) {
  if (n == NULL) return;
  fnode_free(n->a);
  fnode_free(n->b);
  free(n->str);
  if (n->re) regex_free(n->re);
  vec_free(&n->argv);
  vec_free(&n->batch);
  free(n->batch_dir);
  free(n);
}


/*
** {==================================================================
** Parsing the expression
** ===================================================================
*/

static const char *peek (Find *f) {
  return f->pos < f->nargs ? f->args[f->pos] : NULL;
}


static const char *need_arg (Find *f, const char *pred) {
  if (f->pos >= f->nargs) {
    tool_err(f->err, "find", "missing argument to `%s'", pred);
    return NULL;
  }
  return f->args[f->pos++];
}


/* "+5" "-5" "5" */
static int num_arg (Find *f, const char *pred, const char *s, FNode *n) {
  char *end;
  n->cmp = '=';
  if (*s == '+' || *s == '-') n->cmp = *s++;
  if (!isdigit((unsigned char)*s)) goto bad;
  n->num = strtoll(s, &end, 10);
  n->unit = 1;
  if (n->kind == F_SIZE) {
    n->unit = 512;
    switch (*end) {
      case 'c': n->unit = 1; end++; break;
      case 'w': n->unit = 2; end++; break;
      case 'b': n->unit = 512; end++; break;
      case 'k': n->unit = 1024; end++; break;
      case 'M': n->unit = 1048576; end++; break;
      case 'G': n->unit = 1073741824LL; end++; break;
    }
  }
  if (*end == '\0') return 0;
bad:
  tool_err(f->err, "find", "invalid argument `%s' to `%s'", s, pred);
  return -1;
}


static FNode *parse_or (Find *f);


static FNode *parse_primary (Find *f) {
  const char *t = peek(f), *a;
  FNode *n;
  if (t == NULL) {
    tool_err(f->err, "find", "invalid expression; you have used a binary operator with nothing before it.");
    return NULL;
  }
  f->pos++;
  if (strcmp(t, "(") == 0) {
    FNode *e = parse_or(f);
    if (e == NULL) return NULL;
    if (peek(f) == NULL || strcmp(peek(f), ")") != 0) {
      tool_err(f->err, "find", "invalid expression; I was expecting to find a ')' somewhere but did not see one.");
      fnode_free(e);
      return NULL;
    }
    f->pos++;
    return e;
  }
  if (strcmp(t, "!") == 0 || strcmp(t, "-not") == 0) {
    FNode *e = parse_primary(f);
    if (e == NULL) return NULL;
    n = fnode(F_NOT);
    n->a = e;
    return n;
  }
  /* options: always true, they set things up */
  if (strcmp(t, "-maxdepth") == 0 || strcmp(t, "-mindepth") == 0) {
    if ((a = need_arg(f, t)) == NULL) return NULL;
    if (!isdigit((unsigned char)*a)) {
      tool_err(f->err, "find", "Expected a positive decimal integer argument to %s, but got `%s'", t, a);
      return NULL;
    }
    if (t[2] == 'a') f->maxdepth = atoi(a);
    else f->mindepth = atoi(a);
    return fnode(F_TRUE);
  }
  if (strcmp(t, "-depth") == 0 || strcmp(t, "-d") == 0) {
    f->depth_first = 1;
    return fnode(F_TRUE);
  }
  if (strcmp(t, "-follow") == 0) {
    f->follow = 1;
    return fnode(F_TRUE);
  }
  if (strcmp(t, "-daystart") == 0) {
    f->daystart = 1;
    return fnode(F_TRUE);
  }
  if (strcmp(t, "-xdev") == 0 || strcmp(t, "-mount") == 0 || strcmp(t, "-noleaf") == 0 ||
      strcmp(t, "-ignore_readdir_race") == 0 || strcmp(t, "-nowarn") == 0 ||
      strcmp(t, "-warn") == 0)
    return fnode(F_TRUE);
  if (strcmp(t, "-regextype") == 0) {
    if ((a = need_arg(f, t)) == NULL) return NULL;
    f->regex_ere = strstr(a, "extended") != NULL || strcmp(a, "egrep") == 0 ||
                   strcmp(a, "awk") == 0 || strcmp(a, "posix-egrep") == 0;
    return fnode(F_TRUE);
  }
  if (strcmp(t, "-true") == 0) return fnode(F_TRUE);
  if (strcmp(t, "-false") == 0) return fnode(F_FALSE);
  /* tests */
  if (strcmp(t, "-name") == 0 || strcmp(t, "-iname") == 0 || strcmp(t, "-path") == 0 ||
      strcmp(t, "-ipath") == 0 || strcmp(t, "-wholename") == 0 || strcmp(t, "-iwholename") == 0 ||
      strcmp(t, "-lname") == 0 || strcmp(t, "-ilname") == 0) {
    if ((a = need_arg(f, t)) == NULL) return NULL;
    n = fnode(strstr(t, "name") && !strstr(t, "whole") ? (strstr(t, "lname") ? F_LNAME : F_NAME) : F_PATH);
    n->icase = t[1] == 'i';
    n->str = glob_mark(a);
    return n;
  }
  if (strcmp(t, "-regex") == 0 || strcmp(t, "-iregex") == 0) {
    char *err = NULL, *full;
    if ((a = need_arg(f, t)) == NULL) return NULL;
    n = fnode(F_REGEX);
    full = xstrcat3(f->regex_ere ? "^(" : "^\\(", a, f->regex_ere ? ")$" : "\\)$");
    n->re = regex_new(full, (f->regex_ere ? RE_EXTENDED : 0) | (t[1] == 'i' ? RE_ICASE : 0), &err);
    free(full);
    if (n->re == NULL) {
      tool_err(f->err, "find", "invalid regular expression `%s': %s", a, err ? err : "?");
      free(err);
      fnode_free(n);
      return NULL;
    }
    return n;
  }
  if (strcmp(t, "-type") == 0 || strcmp(t, "-xtype") == 0) {
    const char *p;
    if ((a = need_arg(f, t)) == NULL) return NULL;
    for (p = a; *p; p++) {
      if (*p == ',') continue;
      if (!strchr("fdlpscbD", *p)) {
        tool_err(f->err, "find", "Unknown argument to -type: %c", *p);
        return NULL;
      }
    }
    n = fnode(F_TYPE);
    n->str = xstrdup(a);
    return n;
  }
  if (strcmp(t, "-size") == 0 || strcmp(t, "-links") == 0 || strcmp(t, "-uid") == 0 ||
      strcmp(t, "-gid") == 0) {
    if ((a = need_arg(f, t)) == NULL) return NULL;
    n = fnode(t[1] == 's' ? F_SIZE : t[1] == 'l' ? F_LINKS : t[1] == 'u' ? F_UID : F_GID);
    if (num_arg(f, t, a, n) != 0) {
      fnode_free(n);
      return NULL;
    }
    return n;
  }
  if (strcmp(t, "-empty") == 0) return fnode(F_EMPTY);
  if (strcmp(t, "-mtime") == 0 || strcmp(t, "-atime") == 0 || strcmp(t, "-ctime") == 0 ||
      strcmp(t, "-mmin") == 0 || strcmp(t, "-amin") == 0 || strcmp(t, "-cmin") == 0) {
    if ((a = need_arg(f, t)) == NULL) return NULL;
    n = fnode(F_TIME);
    n->which = t[1];
    if (num_arg(f, t, a, n) != 0) {
      fnode_free(n);
      return NULL;
    }
    n->unit = strstr(t, "min") ? 60 : 86400;
    return n;
  }
  if (strcmp(t, "-newer") == 0 || strcmp(t, "-anewer") == 0 || strcmp(t, "-cnewer") == 0) {
    char *native;
    OsStat st;
    if ((a = need_arg(f, t)) == NULL) return NULL;
    native = path_to_native(a);
    if (os_stat(native, &st) != 0) {
      tool_err(f->err, "find", "'%s': %s", a, os_errmsg());
      free(native);
      return NULL;
    }
    free(native);
    n = fnode(F_NEWER);
    n->which = t[1] == 'n' ? 'm' : t[1];
    n->when = st.mtime;
    return n;
  }
  if (strcmp(t, "-perm") == 0) {
    if ((a = need_arg(f, t)) == NULL) return NULL;
    n = fnode(F_PERM);
    n->which = (*a == '-' || *a == '/') ? *a++ : '=';
    if (parse_mode(a, 0, 0, &n->mode) != 0) {
      tool_err(f->err, "find", "invalid mode `%s'", a);
      fnode_free(n);
      return NULL;
    }
    return n;
  }
  if (strcmp(t, "-user") == 0 || strcmp(t, "-group") == 0) {
    if ((a = need_arg(f, t)) == NULL) return NULL;
    n = fnode(t[1] == 'u' ? F_USER : F_GROUP);
    n->str = xstrdup(a);
    return n;
  }
  if (strcmp(t, "-readable") == 0 || strcmp(t, "-writable") == 0 || strcmp(t, "-executable") == 0) {
    n = fnode(F_ACCESS);
    n->which = t[1] == 'r' ? 'r' : t[1] == 'w' ? 'w' : 'x';
    return n;
  }
  if (strcmp(t, "-samefile") == 0) {
    if ((a = need_arg(f, t)) == NULL) return NULL;
    n = fnode(F_SAMEFILE);
    n->str = path_to_native(a);
    return n;
  }
  /* actions */
  if (strcmp(t, "-print") == 0) return fnode(F_PRINT);
  if (strcmp(t, "-print0") == 0) return fnode(F_PRINT0);
  if (strcmp(t, "-ls") == 0) return fnode(F_LS);
  if (strcmp(t, "-prune") == 0) return fnode(F_PRUNE);
  if (strcmp(t, "-quit") == 0) return fnode(F_QUIT);
  if (strcmp(t, "-delete") == 0) {
    f->depth_first = 1;
    return fnode(F_DELETE);
  }
  if (strcmp(t, "-printf") == 0) {
    if ((a = need_arg(f, t)) == NULL) return NULL;
    n = fnode(F_PRINTF);
    n->str = xstrdup(a);
    return n;
  }
  if (strcmp(t, "-exec") == 0 || strcmp(t, "-execdir") == 0 || strcmp(t, "-ok") == 0 ||
      strcmp(t, "-okdir") == 0) {
    n = fnode(F_EXEC);
    n->dir = strstr(t, "dir") != NULL;
    n->ask = t[1] == 'o';
    for (;;) {
      const char *w = peek(f);
      if (w == NULL) {
        tool_err(f->err, "find", "missing argument to `%s'", t);
        fnode_free(n);
        return NULL;
      }
      f->pos++;
      if (strcmp(w, ";") == 0) break;
      if (strcmp(w, "+") == 0 && n->argv.n > 0 && strcmp(n->argv.v[n->argv.n - 1], "{}") == 0 &&
          !n->ask) {
        n->plus = 1;
        free(n->argv.v[--n->argv.n]);	/* the {} goes; paths come in its place */
        n->argv.v[n->argv.n] = NULL;
        break;
      }
      vec_push(&n->argv, xstrdup(w));
    }
    if (n->argv.n == 0) {
      tool_err(f->err, "find", "missing argument to `%s'", t);
      fnode_free(n);
      return NULL;
    }
    if (n->plus) {
      f->execs = (FNode **)xrealloc(f->execs, (size_t)(f->nexecs + 1) * sizeof(FNode *));
      f->execs[f->nexecs++] = n;
    }
    return n;
  }
  if (t[0] == '-') tool_err(f->err, "find", "unknown predicate `%s'", t);
  else tool_err(f->err, "find", "paths must precede expression: `%s'", t);
  return NULL;
}


static int ends_and (const char *t) {
  return t == NULL || strcmp(t, ")") == 0 || strcmp(t, "-o") == 0 || strcmp(t, "-or") == 0 ||
         strcmp(t, ",") == 0;
}


static FNode *parse_and (Find *f) {
  FNode *l = parse_primary(f);
  while (l != NULL && !ends_and(peek(f))) {
    FNode *r, *n;
    if (strcmp(peek(f), "-a") == 0 || strcmp(peek(f), "-and") == 0) f->pos++;
    if ((r = parse_primary(f)) == NULL) {
      fnode_free(l);
      return NULL;
    }
    n = fnode(F_AND);
    n->a = l;
    n->b = r;
    l = n;
  }
  return l;
}


static FNode *parse_or (Find *f) {
  FNode *l = parse_and(f);
  while (l != NULL && peek(f) && (strcmp(peek(f), "-o") == 0 || strcmp(peek(f), "-or") == 0 ||
                                  strcmp(peek(f), ",") == 0)) {
    FNode *r, *n;
    int comma = strcmp(peek(f), ",") == 0;
    f->pos++;
    if ((r = parse_and(f)) == NULL) {
      fnode_free(l);
      return NULL;
    }
    n = fnode(comma ? F_COMMA : F_OR);
    n->a = l;
    n->b = r;
    l = n;
  }
  return l;
}


static int has_action (const FNode *n) {
  if (n == NULL) return 0;
  if (n->kind == F_PRINT || n->kind == F_PRINT0 || n->kind == F_PRINTF || n->kind == F_LS ||
      n->kind == F_DELETE || n->kind == F_EXEC || n->kind == F_QUIT)
    return 1;
  return has_action(n->a) || has_action(n->b);
}

/* }================================================================== */


/*
** {==================================================================
** Running it
** ===================================================================
*/

static int run_exec (Find *f, FNode *n, Vec *argv, const char *dir) {
  char *back = NULL;
  int status;
  out_flush(f->o);
  if (dir != NULL) {
    back = os_getcwd();
    if (os_chdir(dir) != 0) {
      tool_err(f->err, "find", "cannot change to '%s': %s", dir, os_errmsg());
      free(back);
      return 1;
    }
  }
  status = sh_eval_argv((int)argv->n, argv->v, f->in, f->out, f->err, EX_NOFUNC);
  if (back != NULL) {
    os_chdir(back);
    free(back);
  }
  (void)n;
  return status;
}


static void flush_batch (Find *f, FNode *n) {
  Vec argv;
  size_t i;
  int st;
  if (n->batch.n == 0) return;
  vec_init(&argv);
  for (i = 0; i < n->argv.n; i++) vec_push(&argv, xstrdup(n->argv.v[i]));
  for (i = 0; i < n->batch.n; i++) vec_push(&argv, xstrdup(n->batch.v[i]));
  st = run_exec(f, n, &argv, n->batch_dir);
  if (st != 0) f->status = 1;
  vec_free(&argv);
  vec_free(&n->batch);
  vec_init(&n->batch);
}


/* {} anywhere in a word is the path */
static char *subst_braces (const char *w, const char *path) {
  Buf b;
  const char *p;
  buf_init(&b);
  for (p = w; *p; p++) {
    if (p[0] == '{' && p[1] == '}') {
      buf_puts(&b, path);
      p++;
    }
    else buf_putc(&b, *p);
  }
  return buf_take(&b);
}


static const char *ent_name (const Ent *e) {
  return tool_base(e->disp);
}


static void put_time (Out *o, time_t t, char k) {
  struct tm *tm = localtime(&t);
  char buf[64];
  const char *fmt;
  if (k == '@') {
    out_printf(o, "%lld.0000000000", (long long)t);
    return;
  }
  if (tm == NULL) return;
  switch (k) {
    case 'Y': fmt = "%Y"; break;
    case 'm': fmt = "%m"; break;
    case 'd': fmt = "%d"; break;
    case 'H': fmt = "%H"; break;
    case 'M': fmt = "%M"; break;
    case 'S': fmt = "%S"; break;
    case 'T': fmt = "%H:%M:%S"; break;
    case 'F': fmt = "%Y-%m-%d"; break;
    case 'D': fmt = "%m/%d/%y"; break;
    case 'a': fmt = "%a"; break;
    case 'b': case 'h': fmt = "%b"; break;
    case 'e': fmt = "%e"; break;
    case 'j': fmt = "%j"; break;
    case 'y': fmt = "%y"; break;
    case '+': fmt = "%Y-%m-%d+%H:%M:%S"; break;
    case 'c': fmt = "%a %b %e %H:%M:%S %Y"; break;
    default: fmt = "%a %b %e %H:%M:%S %Y"; break;
  }
  strftime(buf, sizeof(buf), fmt, tm);
  out_puts(o, buf);
}


static void do_printf (Find *f, const char *fmt, const Ent *e) {
  Out *o = f->o;
  const char *p;
  for (p = fmt; *p; p++) {
    if (*p == '\\' && p[1]) {
      p++;
      switch (*p) {
        case 'n': out_putc(o, '\n'); break;
        case 't': out_putc(o, '\t'); break;
        case '0': out_putc(o, '\0'); break;
        case 'a': out_putc(o, '\a'); break;
        case 'r': out_putc(o, '\r'); break;
        case '\\': out_putc(o, '\\'); break;
        case 'c': return;
        default: out_putc(o, '\\'); out_putc(o, *p); break;
      }
    }
    else if (*p == '%' && p[1]) {
      char spec[16], val[4096];
      size_t sl = 0;
      const char *s = NULL;
      spec[sl++] = '%';
      p++;
      while ((*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0' || isdigit((unsigned char)*p) ||
              *p == '.') && sl < 12)
        spec[sl++] = *p++;
      val[0] = '\0';
      switch (*p) {
        case '%': out_putc(o, '%'); continue;
        case 'p': s = e->disp; break;
        case 'f': s = ent_name(e); break;
        case 'h': {
          size_t n = strlen(e->disp);
          const char *d = e->disp;
          while (n > 1 && d[n - 1] == '/') n--;
          while (n > 0 && d[n - 1] != '/') n--;
          if (n == 0) s = ".";
          else {
            if (n > 1) n--;
            snprintf(val, sizeof(val), "%.*s", (int)n, d);
            s = val;
          }
          break;
        }
        case 'P': {
          size_t n = strlen(e->start);
          s = e->disp + (strncmp(e->disp, e->start, n) == 0 ? n : 0);
          while (*s == '/') s++;
          break;
        }
        case 'H': s = e->start; break;
        case 's': snprintf(val, sizeof(val), "%lld", e->st.size); s = val; break;
        case 'k': snprintf(val, sizeof(val), "%lld", (e->st.blocks + 1) / 2); s = val; break;
        case 'b': snprintf(val, sizeof(val), "%lld", e->st.blocks); s = val; break;
        case 'd': snprintf(val, sizeof(val), "%d", e->depth); s = val; break;
        case 'm': snprintf(val, sizeof(val), "%o", e->st.mode & 07777); s = val; break;
        case 'M': mode_string(val, &e->st); s = val; break;
        case 'n': snprintf(val, sizeof(val), "%lu", e->st.nlink); s = val; break;
        case 'i': snprintf(val, sizeof(val), "%llu", e->st.ino); s = val; break;
        case 'U': snprintf(val, sizeof(val), "%ld", e->st.uid); s = val; break;
        case 'G': snprintf(val, sizeof(val), "%ld", e->st.gid); s = val; break;
        case 'u': case 'g': {
          char *nm = *p == 'u' ? os_user_name(e->st.uid) : os_group_name(e->st.gid);
          snprintf(val, sizeof(val), "%s", nm);
          free(nm);
          s = val;
          break;
        }
        case 'y': case 'Y': {
          const OsStat *st = &e->st;
          OsStat t;
          if (*p == 'Y' && st->is_link && os_stat(e->native, &t) == 0) st = &t;
          val[0] = st->is_link ? 'l' : st->is_dir ? 'd' : st->is_fifo ? 'p' : st->is_sock ? 's' :
                   st->is_chr ? 'c' : st->is_blk ? 'b' : 'f';
          val[1] = '\0';
          s = val;
          break;
        }
        case 'l': {
          char *t = e->st.is_link ? os_readlink(e->native) : NULL;
          snprintf(val, sizeof(val), "%s", t ? t : "");
          free(t);
          s = val;
          break;
        }
        case 't': case 'a': case 'c': {
          time_t t = *p == 't' ? e->st.mtime : *p == 'a' ? e->st.atime : e->st.ctime;
          put_time(o, t, 'c');
          continue;
        }
        case 'T': case 'A': case 'C': {
          time_t t = *p == 'T' ? e->st.mtime : *p == 'A' ? e->st.atime : e->st.ctime;
          if (p[1]) put_time(o, t, *++p);
          continue;
        }
        default:
          out_putc(o, '%');
          out_putc(o, *p);
          continue;
      }
      spec[sl++] = 's';
      spec[sl] = '\0';
      out_printf(o, spec, s ? s : "");
    }
    else out_putc(o, *p);
  }
}


static int is_empty (const Ent *e) {
  if (e->st.is_dir) {
    Vec names;
    int empty;
    vec_init(&names);
    empty = os_listdir(e->native, &names) == 0 && names.n == 0;
    vec_free(&names);
    return empty;
  }
  return !e->st.is_link && e->st.size == 0 && !e->st.is_fifo && !e->st.is_chr;
}


static int cmp_num (const FNode *n, long long v) {
  if (n->cmp == '+') return v > n->num;
  if (n->cmp == '-') return v < n->num;
  return v == n->num;
}


static int type_char (const OsStat *st) {
  return st->is_link ? 'l' : st->is_dir ? 'd' : st->is_fifo ? 'p' : st->is_sock ? 's' :
         st->is_chr ? 'c' : st->is_blk ? 'b' : 'f';
}


static int eval (Find *f, FNode *n, Ent *e) {
  switch (n->kind) {
    case F_AND: return eval(f, n->a, e) && !f->quit && eval(f, n->b, e);
    case F_OR: return eval(f, n->a, e) || (!f->quit && eval(f, n->b, e));
    case F_COMMA: eval(f, n->a, e); return !f->quit && eval(f, n->b, e);
    case F_NOT: return !eval(f, n->a, e);
    case F_TRUE: return 1;
    case F_FALSE: return 0;
    case F_NAME: {
      char *base = xstrdup(ent_name(e));
      size_t bl = strlen(base);
      int r;
      while (bl > 1 && base[bl - 1] == '/') base[--bl] = '\0';
      r = pat_match(n->str, base, n->icase ? PM_NOCASE : 0);
      free(base);
      return r;
    }
    case F_PATH: return pat_match(n->str, e->disp, n->icase ? PM_NOCASE : 0);
    case F_REGEX: return regex_match(n->re, e->disp, strlen(e->disp), 0, 0, NULL);
    case F_LNAME: {
      char *t;
      int r;
      if (!e->st.is_link || (t = os_readlink(e->native)) == NULL) return 0;
      r = pat_match(n->str, t, n->icase ? PM_NOCASE : 0);
      free(t);
      return r;
    }
    case F_TYPE: {
      int t = type_char(&e->st);
      return strchr(n->str, t) != NULL;
    }
    case F_SIZE: {
      long long units = (e->st.size + n->unit - 1) / n->unit;
      return cmp_num(n, units);
    }
    case F_EMPTY: return is_empty(e);
    case F_TIME: {
      time_t t = n->which == 'a' ? e->st.atime : n->which == 'c' ? e->st.ctime : e->st.mtime;
      time_t ref = f->now;
      long long age, v;
      if (f->daystart) {	/* from the end of today */
        struct tm *tm = localtime(&ref);
        if (tm) {
          tm->tm_hour = 23;
          tm->tm_min = 59;
          tm->tm_sec = 59;
          ref = mktime(tm) + 1;
        }
      }
      age = (long long)(ref - t);
      v = age >= 0 ? age / n->unit : -((-age + n->unit - 1) / n->unit);
      if (n->unit == 60) {	/* -mmin: GNU compares the real ages */
        if (n->cmp == '+') return age > n->num * 60;
        if (n->cmp == '-') return age < n->num * 60;
        return v == n->num;
      }
      return cmp_num(n, v);
    }
    case F_NEWER: {
      time_t t = n->which == 'a' ? e->st.atime : n->which == 'c' ? e->st.ctime : e->st.mtime;
      return t > n->when;
    }
    case F_PERM: {
      unsigned m = e->st.mode & 07777;
      if (n->which == '-') return (m & n->mode) == n->mode;
      if (n->which == '/') return n->mode == 0 || (m & n->mode) != 0;
      return m == n->mode;
    }
    case F_USER: case F_GROUP: {
      long id = n->kind == F_USER ? e->st.uid : e->st.gid;
      char *nm, num[24];
      int r;
      if (strspn(n->str, "0123456789") == strlen(n->str)) return atol(n->str) == id;
      nm = n->kind == F_USER ? os_user_name(id) : os_group_name(id);
      r = strcmp(nm, n->str) == 0 || strcmp(ll_to_str(id, num), n->str) == 0;
      free(nm);
      return r;
    }
    case F_UID: return cmp_num(n, e->st.uid);
    case F_GID: return cmp_num(n, e->st.gid);
    case F_LINKS: return cmp_num(n, (long long)e->st.nlink);
    case F_ACCESS: return os_access(e->native, n->which);
    case F_SAMEFILE: return os_same_file(e->native, n->str);
    case F_PRINT:
      out_puts(f->o, e->disp);
      out_putc(f->o, '\n');
      return 1;
    case F_PRINT0:
      out_puts(f->o, e->disp);
      out_putc(f->o, '\0');
      return 1;
    case F_PRINTF:
      do_printf(f, n->str, e);
      return 1;
    case F_LS:
      ls_long_line(f->o, e->disp, e->native, &e->st, 0);
      return 1;
    case F_PRUNE:
      if (!f->depth_first) f->prune = 1;
      return 1;
    case F_QUIT:
      f->quit = 1;
      return 1;
    case F_DELETE: {
      int r;
      if (e->depth == 0 && strcmp(e->disp, ".") == 0) return 1;	/* GNU leaves "." */
      r = (e->st.is_dir && !e->st.is_link) ? os_rmdir(e->native) : os_unlink(e->native);
#ifdef _WIN32
      if (r != 0 && e->st.is_link && e->st.is_dir) r = os_rmdir(e->native);
      if (r != 0 && os_errcode() == OS_E_ACCES && !e->st.is_dir) {
        os_chmod(e->native, 0666);
        r = os_unlink(e->native);
      }
#endif
      if (r != 0) {
        out_flush(f->o);
        tool_err(f->err, "find", "cannot delete '%s': %s", e->disp, os_errmsg());
        f->status = 1;
        return 0;
      }
      return 1;
    }
    case F_EXEC: {
      Vec argv;
      size_t i;
      int st;
      char *dir = NULL;
      const char *path = e->disp;
      char *dpath = NULL;
      if (n->dir) {	/* -execdir: in the file's folder, as ./name */
        dir = path_dirname(e->native);
        dpath = xstrcat3("./", ent_name(e), "");
        path = dpath;
      }
      if (n->plus) {
        if (n->dir && n->batch.n > 0 && (n->batch_dir == NULL || strcmp(n->batch_dir, dir) != 0))
          flush_batch(f, n);
        if (n->dir && n->batch_dir == NULL) n->batch_dir = xstrdup(dir);
        else if (n->dir && strcmp(n->batch_dir, dir) != 0) {
          free(n->batch_dir);
          n->batch_dir = xstrdup(dir);
        }
        vec_push(&n->batch, xstrdup(path));
        if (n->batch.n >= 500) flush_batch(f, n);
        free(dir);
        free(dpath);
        return 1;
      }
      vec_init(&argv);
      for (i = 0; i < n->argv.n; i++) vec_push(&argv, subst_braces(n->argv.v[i], path));
      if (n->ask) {
        Buf q;
        buf_init(&q);
        for (i = 0; i < argv.n; i++) {
          if (i) buf_putc(&q, ' ');
          buf_puts(&q, argv.v[i]);
        }
        out_flush(f->o);
        if (!tool_ask(f->in, f->err, "< %s > ? ", q.s)) {
          buf_free(&q);
          vec_free(&argv);
          free(dir);
          free(dpath);
          return 0;
        }
        buf_free(&q);
      }
      st = run_exec(f, n, &argv, dir);
      vec_free(&argv);
      free(dir);
      free(dpath);
      return st == 0;
    }
  }
  return 0;
}


static void visit (Find *f, const char *disp, const char *native, const char *start, int depth) {
  Ent e;
  int follow = f->follow == 1 || (f->follow == 2 && depth == 0);
  if (f->quit || tool_stop() || f->o->failed) return;
  e.disp = disp;
  e.native = native;
  e.start = start;
  e.depth = depth;
  if ((follow ? os_stat(native, &e.st) : os_lstat(native, &e.st)) != 0) {
    if (follow && os_lstat(native, &e.st) == 0) {
      /* a dangling link: seen as a link */
    }
    else {
      out_flush(f->o);
      tool_err(f->err, "find", "'%s': %s", disp, os_errmsg());
      f->status = 1;
      return;
    }
  }
  f->prune = 0;
  if (!f->depth_first && depth >= f->mindepth) eval(f, f->root, &e);
  if (e.st.is_dir && !e.st.is_link && !f->prune && !f->quit &&
      (f->maxdepth < 0 || depth < f->maxdepth)) {
    Vec names;
    size_t i;
    vec_init(&names);
    if (os_listdir(native, &names) != 0) {
      out_flush(f->o);
      tool_err(f->err, "find", "'%s': %s", disp, os_errmsg());
      f->status = 1;
    }
    for (i = 0; i < names.n && !f->quit && !tool_stop(); i++) {
      char *cd = tool_join(disp, names.v[i]), *cn = path_join(native, names.v[i]);
      visit(f, cd, cn, start, depth + 1);
      free(cd);
      free(cn);
    }
    vec_free(&names);
  }
  if (f->depth_first && depth >= f->mindepth && !f->quit) {
    f->prune = 0;
    eval(f, f->root, &e);
  }
}


int t_find (int argc, char **argv, int in, int out, int err) {
  Find f;
  Out o;
  Vec paths;
  int i;
  size_t k;
  memset(&f, 0, sizeof(f));
  f.maxdepth = -1;
  f.in = in;
  f.out = out;
  f.err = err;
  f.now = time(NULL);
  out_init(&o, out);
  f.o = &o;
  vec_init(&paths);
  for (i = 1; i < argc; i++) {	/* -H -L -P, then the paths */
    if (strcmp(argv[i], "-L") == 0) f.follow = 1;
    else if (strcmp(argv[i], "-H") == 0) f.follow = 2;
    else if (strcmp(argv[i], "-P") == 0) f.follow = 0;
    else if (strcmp(argv[i], "--help") == 0) return tool_help(out, "find");
    else if (strcmp(argv[i], "--") == 0) { i++; break; }
    else break;
  }
  for (; i < argc; i++) {
    const char *a = argv[i];
    if (a[0] == '-' || strcmp(a, "(") == 0 || strcmp(a, "!") == 0) break;
    vec_push(&paths, xstrdup(a));
  }
  f.args = argv + i;
  f.nargs = argc - i;
  if (f.nargs > 0) {
    f.root = parse_or(&f);
    if (f.root == NULL) {
      vec_free(&paths);
      free(f.execs);
      return 1;
    }
    if (f.pos < f.nargs) {
      tool_err(err, "find", "unexpected `%s'", f.args[f.pos]);
      fnode_free(f.root);
      vec_free(&paths);
      free(f.execs);
      return 1;
    }
  }
  else f.root = fnode(F_TRUE);
  if (!has_action(f.root)) {	/* no action: -print the ones that match */
    FNode *a = fnode(F_AND);
    a->a = f.root;
    a->b = fnode(F_PRINT);
    f.root = a;
  }
  if (paths.n == 0) vec_push(&paths, xstrdup("."));
  for (k = 0; k < paths.n && !f.quit && !tool_stop(); k++) {
    char *native = path_to_native(paths.v[k]);
    visit(&f, paths.v[k], native, paths.v[k], 0);
    free(native);
  }
  for (i = 0; i < f.nexecs; i++) flush_batch(&f, f.execs[i]);
  out_flush(&o);
  fnode_free(f.root);
  free(f.execs);
  vec_free(&paths);
  return tool_stop() ? 130 : f.status;
}
