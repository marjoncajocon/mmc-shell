/*
** csed.c - sed, the stream editor, GNU flavour
**
**   sed [-nEsuz] [-i[SUFFIX]] [-l N] [-e script]... [-f file]... [script] [file...]
** Addresses: N  $  /re/I M  \cREc  first~step  a1,a2  a1,+N  a1,~N  0,/re/  !
** Commands:  { } = a i c b t T : d D e F g G h H l n N p P q Q r R s w W x y z #
** s flags:   g p N i I m M e w file;  in the replacement: & \1..\9 \n \t
**            \L \U \l \u \E
*/

#include "mmc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


enum { A_NONE, A_LINE, A_LAST, A_RE, A_STEP, A_ZERO, A_PLUS, A_MULT };

typedef struct SAddr {
  int type;
  long long n, step;
  Regex *re;	/* A_RE; NULL: the last regex used */
} SAddr;

typedef struct SCmd {
  SAddr a1, a2;
  int neg;
  char cmd;
  int active;	/* a range is on */
  long long end;
  char *text;	/* a i c: the text; b t T ':': label; r R w W: file */
  size_t tlen;
  int jump;	/* '{': after its '}'; b t T: the command to go to (-1: end) */
  Regex *re;	/* s */
  int re_last;	/* s//: the last regex */
  char *repl;
  size_t rlen;
  int global, print, nth, eval, icase;
  int wfile;	/* index in Sed.wfiles, -1 none */
  unsigned *yfrom, *yto;	/* y */
  size_t ylen;
  int code;	/* q Q exit code; l width */
  int has_num;
} SCmd;

typedef struct WFile {
  char *name;
  int fd;
  int own;
} WFile;

typedef struct RFile {	/* R: a file read one line at a time */
  char *name;
  In in;
  int open;
} RFile;

typedef struct Sed {
  SCmd *cmds;
  int ncmds, cap;
  WFile *wfiles;
  int nwfiles;
  RFile *rfiles;
  int nrfiles;
  int quiet, ere, separate, inplace, unbuffered, zero, posix, debug;
  const char *suffix;
  int lwidth;
  /* running */
  Buf ps, hs;
  int ps_nl;	/* the last line read had its newline */
  long long lineno;
  int tflag, quit, exit_code;
  Regex *last_re;
  Vec appends;	/* texts to output at the end of the cycle; "\001file" reads a file */
  Out *o;
  int in, out, err;
  /* input */
  char **files;
  int nfiles, fi;
  In cur;
  int cur_open;
  const char *cur_name;
  char *la;	/* the next line (lookahead) */
  size_t la_len;
  int la_ok, la_nl;
  int status;
  /* parse */
  const char *src, *p, *end;
  Vec chunk_names;	/* for messages */
  Vec chunk_starts;
} Sed;


/*
** {==================================================================
** Parsing
** ===================================================================
*/

static int sed_perr (Sed *s, const char *msg) {
  size_t pos = (size_t)(s->p - s->src), k, which = 0;
  size_t start = 0;
  for (k = 0; k < s->chunk_starts.n; k++) {
    size_t st = (size_t)strtoull(s->chunk_starts.v[k], NULL, 10);
    if (st <= pos) {
      which = k;
      start = st;
    }
  }
  if (s->chunk_names.n > 0 && s->chunk_names.v[which][0] == '-')
    fd_printf(s->err, "sed: -e expression #%s, char %lu: %s\n", s->chunk_names.v[which] + 1,
              (unsigned long)(pos - start), msg);
  else if (s->chunk_names.n > 0)
    fd_printf(s->err, "sed: file %s line %lu: %s\n", s->chunk_names.v[which],
              (unsigned long)1, msg);
  else fd_printf(s->err, "sed: %s\n", msg);
  return -1;
}


static void skip_ws (Sed *s) {
  while (s->p < s->end && (*s->p == ' ' || *s->p == '\t')) s->p++;
}


static void skip_ws_nl (Sed *s) {
  while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == ';')) s->p++;
}


/* text up to delim (not escaped) -> Buf; regex: [..] can hold delim */
static int read_delimited (Sed *s, char delim, Buf *b, int is_regex) {
  while (s->p < s->end) {
    char c = *s->p;
    if (c == '\\' && s->p + 1 < s->end) {
      char n = s->p[1];
      s->p += 2;
      if (n == delim && delim != '\n') buf_putc(b, delim);
      else if (n == '\n') buf_puts(b, is_regex ? "\\n" : "\n");
      else if (n == 'n' && !is_regex) buf_putc(b, '\n');	/* y: \n */
      else {
        buf_putc(b, '\\');
        buf_putc(b, n);
      }
      continue;
    }
    if (c == delim) {
      s->p++;
      return 0;
    }
    if (is_regex && c == '[') {	/* a bracket expression: delim is plain there */
      const char *q = s->p + 1;
      if (q < s->end && *q == '^') q++;
      if (q < s->end && *q == ']') q++;
      while (q < s->end && *q != ']' && *q != '\n') {
        if (q[0] == '[' && q + 1 < s->end && (q[1] == ':' || q[1] == '.' || q[1] == '=')) {
          char e = q[1];
          q += 2;
          while (q + 1 < s->end && !(q[0] == e && q[1] == ']')) q++;
          if (q + 1 < s->end) q += 2;
          continue;
        }
        q++;
      }
      if (q < s->end && *q == ']') {
        buf_putn(b, s->p, (size_t)(q + 1 - s->p));
        s->p = q + 1;
        continue;
      }
    }
    if (c == '\n' && is_regex) break;
    buf_putc(b, c);
    s->p++;
  }
  return -1;
}


static Regex *compile_re (Sed *s, const char *pat, size_t len, int flags) {
  char *err = NULL;
  Regex *re = regex_new_n(pat, len, (s->ere ? RE_EXTENDED : 0) | flags, &err);
  if (re == NULL) {
    char msg[300];
    snprintf(msg, sizeof(msg), "%s", err ? err : "invalid regular expression");
    free(err);
    sed_perr(s, msg);
  }
  return re;
}


/* an address at s->p; 0 none, 1 one, -1 error */
static int parse_addr (Sed *s, SAddr *a, int second) {
  memset(a, 0, sizeof(*a));
  if (s->p >= s->end) return 0;
  if (isdigit((unsigned char)*s->p)) {
    char *e;
    a->n = strtoll(s->p, &e, 10);
    s->p = e;
    a->type = A_LINE;
    if (s->p < s->end && *s->p == '~' && !second) {
      s->p++;
      a->step = strtoll(s->p, &e, 10);
      s->p = e;
      a->type = A_STEP;
    }
    else if (a->n == 0 && !second) a->type = A_ZERO;
    return 1;
  }
  if (*s->p == '$') {
    s->p++;
    a->type = A_LAST;
    return 1;
  }
  if (second && (*s->p == '+' || *s->p == '~')) {
    char *e;
    a->type = *s->p == '+' ? A_PLUS : A_MULT;
    s->p++;
    if (s->p >= s->end || !isdigit((unsigned char)*s->p)) return sed_perr(s, "expected newer version of sed");
    a->n = strtoll(s->p, &e, 10);
    s->p = e;
    return 1;
  }
  if (*s->p == '/' || *s->p == '\\') {
    char delim = '/';
    Buf b;
    int flags = 0;
    if (*s->p == '\\') {
      s->p++;
      if (s->p >= s->end) return sed_perr(s, "unexpected end");
      delim = *s->p;
    }
    s->p++;
    buf_init(&b);
    if (read_delimited(s, delim, &b, 1) != 0) {
      buf_free(&b);
      return sed_perr(s, "unterminated address regex");
    }
    while (s->p < s->end && (*s->p == 'I' || *s->p == 'M')) {
      flags |= *s->p == 'I' ? RE_ICASE : RE_NEWLINE;
      s->p++;
    }
    a->type = A_RE;
    if (b.len > 0) {
      a->re = compile_re(s, b.s, b.len, flags);
      if (a->re == NULL) {
        buf_free(&b);
        return -1;
      }
    }
    buf_free(&b);
    return 1;
  }
  return 0;
}


/* the text of a i c: one-liner "a text", or "a\" then lines */
static char *read_text (Sed *s, size_t *len) {
  Buf b;
  buf_init(&b);
  skip_ws(s);
  if (s->p < s->end && *s->p == '\\') {	/* a\ then the text; spaces after it are kept */
    const char *q = s->p + 1;
    while (q < s->end && (*q == ' ' || *q == '\t')) q++;
    if (q < s->end && *q == '\n') s->p = q + 1;
    else s->p++;
  }
  while (s->p < s->end && *s->p != '\n') {
    char c = *s->p++;
    if (c == '\\' && s->p < s->end) {
      c = *s->p++;
      if (c == '\n') {
        buf_putc(&b, '\n');
        continue;
      }
      if (c == 't') c = '\t';
    }
    buf_putc(&b, c);
  }
  *len = b.len;
  return buf_take(&b);
}


/* to the end of the line (file names) */
static char *read_to_eol (Sed *s) {
  const char *st;
  skip_ws(s);
  st = s->p;
  while (s->p < s->end && *s->p != '\n') s->p++;
  return xstrndup(st, (size_t)(s->p - st));
}


/* labels end at a newline or ';' */
static char *read_label (Sed *s) {
  const char *st;
  skip_ws(s);
  st = s->p;
  while (s->p < s->end && *s->p != '\n' && *s->p != ';') s->p++;
  {
    const char *e = s->p;
    while (e > st && (e[-1] == ' ' || e[-1] == '\t')) e--;
    return xstrndup(st, (size_t)(e - st));
  }
}


static int wfile_index (Sed *s, const char *name) {
  int i, fd, own = 1;
  for (i = 0; i < s->nwfiles; i++)
    if (strcmp(s->wfiles[i].name, name) == 0) return i;
  if (strcmp(name, "/dev/stdout") == 0) {
    fd = -2;
    own = 0;
  }
  else if (strcmp(name, "/dev/stderr") == 0) {
    fd = s->err;
    own = 0;
  }
  else {
    char *native = path_to_native(name);
    fd = os_open(native, OS_WRITE);
    free(native);
    if (fd < 0) {
      tool_err(s->err, "sed", "couldn't open file %s: %s", name, os_errmsg());
      return -2;
    }
  }
  s->wfiles = (WFile *)xrealloc(s->wfiles, (size_t)(s->nwfiles + 1) * sizeof(WFile));
  s->wfiles[s->nwfiles].name = xstrdup(name);
  s->wfiles[s->nwfiles].fd = fd;
  s->wfiles[s->nwfiles].own = own;
  return s->nwfiles++;
}


static unsigned *utf8_chars (const char *s, size_t len, size_t *n) {
  unsigned *v = (unsigned *)xmalloc((len + 1) * sizeof(unsigned));
  size_t i = 0, k = 0;
  while (i < len) {
    int l = utf8_len(s + i);
    unsigned cp = 0;
    int j;
    if (l < 1 || i + (size_t)l > len) l = 1;
    for (j = 0; j < l; j++) cp = (cp << 8) | (unsigned char)s[i + (size_t)j];	/* the bytes, packed */
    v[k++] = cp;
    i += (size_t)l;
  }
  *n = k;
  return v;
}


static int parse_script (Sed *s) {
  Vec labels_need;	/* "index name" of b/t/T */
  int *stack = NULL, depth = 0, k;
  vec_init(&labels_need);
  for (;;) {
    SCmd *c;
    int r;
    skip_ws_nl(s);
    if (s->p >= s->end) break;
    if (*s->p == '#') {
      while (s->p < s->end && *s->p != '\n') s->p++;
      continue;
    }
    if (s->ncmds == s->cap) {
      s->cap = s->cap ? s->cap * 2 : 16;
      s->cmds = (SCmd *)xrealloc(s->cmds, (size_t)s->cap * sizeof(SCmd));
    }
    c = &s->cmds[s->ncmds];
    memset(c, 0, sizeof(*c));
    c->wfile = -1;
    c->jump = -1;
    if ((r = parse_addr(s, &c->a1, 0)) < 0) goto fail;
    if (r > 0) {
      skip_ws(s);
      if (s->p < s->end && *s->p == ',') {
        s->p++;
        skip_ws(s);
        if ((r = parse_addr(s, &c->a2, 1)) < 0) goto fail;
        if (r == 0) {
          sed_perr(s, "unexpected `,'");
          goto fail;
        }
        if (c->a2.type == A_STEP) c->a2.type = A_LINE;
      }
      if (c->a1.type == A_ZERO && !(c->a2.type == A_RE)) {
        sed_perr(s, "invalid usage of line address 0");
        goto fail;
      }
    }
    skip_ws(s);
    while (s->p < s->end && *s->p == '!') {
      c->neg = 1;
      s->p++;
      skip_ws(s);
    }
    if (s->p >= s->end) {
      sed_perr(s, "missing command");
      goto fail;
    }
    c->cmd = *s->p++;
    switch (c->cmd) {
      case '{':
        stack = (int *)xrealloc(stack, (size_t)(depth + 1) * sizeof(int));
        stack[depth++] = s->ncmds;
        break;
      case '}':
        if (depth == 0) {
          sed_perr(s, "unexpected `}'");
          goto fail;
        }
        if (c->a1.type != A_NONE) {
          sed_perr(s, "} doesn't want any addresses");
          goto fail;
        }
        s->cmds[stack[--depth]].jump = s->ncmds + 1;
        break;
      case '=': case 'd': case 'D': case 'g': case 'G': case 'h': case 'H': case 'n':
      case 'N': case 'p': case 'P': case 'x': case 'z': case 'F':
        break;
      case 'a': case 'i': case 'c':
        c->text = read_text(s, &c->tlen);
        break;
      case ':':
        if (c->a1.type != A_NONE) {
          sed_perr(s, ": doesn't want any addresses");
          goto fail;
        }
        c->text = read_label(s);
        if (c->text[0] == '\0') {
          sed_perr(s, "\":\" lacks a label");
          goto fail;
        }
        break;
      case 'b': case 't': case 'T': {
        char num[24];
        Buf nb;
        c->text = read_label(s);
        if (c->text[0] != '\0') {
          buf_init(&nb);
          buf_printf(&nb, "%s %s", ll_to_str(s->ncmds, num), c->text);
          vec_push(&labels_need, buf_take(&nb));
        }
        break;
      }
      case 'r': case 'R': case 'w': case 'W':
        c->text = read_to_eol(s);
        if (c->text[0] == '\0') {
          sed_perr(s, "missing filename in r/R/w/W commands");
          goto fail;
        }
        if (c->cmd == 'w' || c->cmd == 'W') {
          if ((c->wfile = wfile_index(s, c->text)) < -1) goto fail;
        }
        break;
      case 'l': case 'q': case 'Q': case 'L':
        skip_ws(s);
        c->code = c->cmd == 'l' ? -1 : 0;
        if (s->p < s->end && isdigit((unsigned char)*s->p)) {
          char *e;
          c->code = (int)strtol(s->p, &e, 10);
          s->p = e;
          c->has_num = 1;
        }
        break;
      case 'e':
        c->text = read_to_eol(s);
        break;
      case 's': {
        char delim;
        Buf re, rp;
        int flags = 0;
        if (s->p >= s->end || *s->p == '\n' || *s->p == '\\') {
          sed_perr(s, "unterminated `s' command");
          goto fail;
        }
        delim = *s->p++;
        buf_init(&re);
        buf_init(&rp);
        if (read_delimited(s, delim, &re, 1) != 0) {
          buf_free(&re);
          buf_free(&rp);
          sed_perr(s, "unterminated `s' command");
          goto fail;
        }
        /* the replacement: keep the escapes, they are read when used */
        while (s->p < s->end && *s->p != delim) {
          if (*s->p == '\\' && s->p + 1 < s->end) {
            if (s->p[1] == delim) buf_putc(&rp, delim);
            else if (s->p[1] == '\n') buf_putc(&rp, '\n');
            else {
              buf_putc(&rp, '\\');
              buf_putc(&rp, s->p[1]);
            }
            s->p += 2;
            continue;
          }
          buf_putc(&rp, *s->p++);
        }
        if (s->p >= s->end) {
          buf_free(&re);
          buf_free(&rp);
          sed_perr(s, "unterminated `s' command");
          goto fail;
        }
        s->p++;
        for (;;) {	/* the flags */
          if (s->p >= s->end) break;
          if (*s->p == 'g') { c->global = 1; s->p++; }
          else if (*s->p == 'p') { c->print++; s->p++; }
          else if (*s->p == 'i' || *s->p == 'I') { flags |= RE_ICASE; s->p++; }
          else if (*s->p == 'm' || *s->p == 'M') { flags |= RE_NEWLINE; s->p++; }
          else if (*s->p == 'e') { c->eval = 1; s->p++; }
          else if (isdigit((unsigned char)*s->p)) {
            char *e;
            c->nth = (int)strtol(s->p, &e, 10);
            s->p = e;
            if (c->nth == 0) {
              buf_free(&re);
              buf_free(&rp);
              sed_perr(s, "number option to `s' command may not be zero");
              goto fail;
            }
          }
          else if (*s->p == 'w') {
            s->p++;
            c->text = read_to_eol(s);
            if ((c->wfile = wfile_index(s, c->text)) < -1) {
              buf_free(&re);
              buf_free(&rp);
              goto fail;
            }
            break;
          }
          else break;
        }
        if (re.len == 0) c->re_last = 1;
        else {
          c->re = compile_re(s, re.s, re.len, flags);
          if (c->re == NULL) {
            buf_free(&re);
            buf_free(&rp);
            goto fail;
          }
        }
        c->icase = flags;
        c->rlen = rp.len;
        c->repl = buf_take(&rp);
        buf_free(&re);
        if (c->re != NULL) {	/* \N beyond the groups */
          size_t i;
          for (i = 0; i + 1 < c->rlen; i++) {
            if (c->repl[i] == '\\') {
              if (isdigit((unsigned char)c->repl[i + 1]) && c->repl[i + 1] - '0' > regex_nsub(c->re)) {
                char msg[80];
                snprintf(msg, sizeof(msg), "invalid reference \\%c on `s' command's RHS", c->repl[i + 1]);
                sed_perr(s, msg);
                goto fail;
              }
              i++;
            }
          }
        }
        break;
      }
      case 'y': {
        char delim;
        Buf a, b;
        if (s->p >= s->end) {
          sed_perr(s, "unterminated `y' command");
          goto fail;
        }
        delim = *s->p++;
        buf_init(&a);
        buf_init(&b);
        if (read_delimited(s, delim, &a, 0) != 0 || read_delimited(s, delim, &b, 0) != 0) {
          buf_free(&a);
          buf_free(&b);
          sed_perr(s, "unterminated `y' command");
          goto fail;
        }
        {	/* \\ is a backslash */
          Buf *bs[2];
          int j;
          bs[0] = &a;
          bs[1] = &b;
          for (j = 0; j < 2; j++) {
            size_t x, y = 0;
            for (x = 0; x < bs[j]->len; x++) {
              if (bs[j]->s[x] == '\\' && x + 1 < bs[j]->len) {
                char n = bs[j]->s[++x];
                bs[j]->s[y++] = n == 't' ? '\t' : n == 'r' ? '\r' : n;
              }
              else bs[j]->s[y++] = bs[j]->s[x];
            }
            bs[j]->len = y;
          }
        }
        {
          size_t na, nb;
          c->yfrom = utf8_chars(a.s ? a.s : "", a.len, &na);
          c->yto = utf8_chars(b.s ? b.s : "", b.len, &nb);
          buf_free(&a);
          buf_free(&b);
          if (na != nb) {
            sed_perr(s, "strings for `y' command are different lengths");
            goto fail;
          }
          c->ylen = na;
        }
        break;
      }
      default: {
        char msg[64];
        snprintf(msg, sizeof(msg), "unknown command: `%c'", s->p[-1]);
        sed_perr(s, msg);
        goto fail;
      }
    }
    s->ncmds++;
    /* after a command: spaces, then ; newline } # or the end */
    skip_ws(s);
    if (s->p < s->end && *s->p != ';' && *s->p != '\n' && *s->p != '}' && *s->p != '#') {
      if (strchr("{aic:btTrRwWe", s->cmds[s->ncmds - 1].cmd) == NULL) {
        s->p++;
        sed_perr(s, "extra characters after command");
        goto fail_counted;
      }
    }
  }
  if (depth > 0) {
    s->p = s->src;	/* GNU says char 0 */
    sed_perr(s, "unmatched `{'");
    goto fail_counted;
  }
  /* branches to their labels */
  for (k = 0; k < (int)labels_need.n; k++) {
    char *sp = strchr(labels_need.v[k], ' ');
    int at = atoi(labels_need.v[k]), j, found = -1;
    for (j = 0; j < s->ncmds; j++)
      if (s->cmds[j].cmd == ':' && strcmp(s->cmds[j].text, sp + 1) == 0) found = j;
    if (found < 0) {
      fd_printf(s->err, "sed: -e expression #1, char %d: can't find label for jump to `%s'\n", 0, sp + 1);
      free(stack);
      vec_free(&labels_need);
      return -1;
    }
    s->cmds[at].jump = found;
  }
  free(stack);
  vec_free(&labels_need);
  return 0;
fail:
  /* the command being parsed is not counted: free what it had */
  free(s->cmds[s->ncmds].text);
  free(s->cmds[s->ncmds].repl);
  if (s->cmds[s->ncmds].re) regex_free(s->cmds[s->ncmds].re);
  if (s->cmds[s->ncmds].a1.re) regex_free(s->cmds[s->ncmds].a1.re);
  if (s->cmds[s->ncmds].a2.re) regex_free(s->cmds[s->ncmds].a2.re);
  free(s->cmds[s->ncmds].yfrom);
  free(s->cmds[s->ncmds].yto);
fail_counted:
  free(stack);
  vec_free(&labels_need);
  return -1;
}

/* }================================================================== */


/*
** {==================================================================
** Input: a stream of lines over the files, one line of lookahead
** ===================================================================
*/

static int open_next (Sed *s) {
  while (s->fi < s->nfiles) {
    const char *name = s->files[s->fi++];
    if (in_open(&s->cur, name, s->in) == 0) {
      s->cur_open = 1;
      s->cur_name = name;
      return 1;
    }
    {
      char *native = path_to_native(name);
      OsStat st;
      if (os_stat(native, &st) == 0 && st.is_dir) tool_err(s->err, "sed", "couldn't edit %s: not a regular file", name);
      else tool_err(s->err, "sed", "can't read %s: %s", name, os_errmsg());
      free(native);
    }
    s->status = 2;
  }
  return 0;
}


/* fills the lookahead; 0 at the end of the input */
static int fetch (Sed *s) {
  char *line;
  size_t len;
  int had;
  char eol = s->zero ? '\0' : '\n';
  if (s->la_ok) return 1;
  for (;;) {
    if (!s->cur_open && !open_next(s)) return 0;
    if (in_line(&s->cur, &line, &len, eol, &had)) {
      free(s->la);
      s->la = (char *)xmalloc(len + 1);
      memcpy(s->la, line, len);
      s->la[len] = '\0';
      s->la_len = len;
      s->la_nl = had;
      s->la_ok = 1;
      return 1;
    }
    in_close(&s->cur);
    s->cur_open = 0;
  }
}


static int is_last (Sed *s) {
  return !fetch(s);
}


/* the next line into the pattern space (append: after a newline) */
static int next_line (Sed *s, int append) {
  if (!fetch(s)) return 0;
  if (append) buf_putc(&s->ps, s->zero ? '\0' : '\n');
  else s->ps.len = 0;
  buf_putn(&s->ps, s->la, s->la_len);
  if (s->ps.s == NULL) buf_putn(&s->ps, "", 0);
  s->ps_nl = s->la_nl;
  s->la_ok = 0;
  s->lineno++;
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** Running
** ===================================================================
*/

static const char *ps_str (Sed *s) {
  if (s->ps.s == NULL) {
    buf_putc(&s->ps, 'x');
    s->ps.len = 0;
  }
  s->ps.s[s->ps.len] = '\0';
  return s->ps.s;
}


static void put_ps (Sed *s, Out *o, size_t len, int nl) {
  ps_str(s);
  out_putn(o, s->ps.s, len);
  if (nl) out_putc(o, s->zero ? '\0' : '\n');
}


static Out *wfile_out (Sed *s, int k, Out *tmp) {
  if (s->wfiles[k].fd == -2) return s->o;
  out_init(tmp, s->wfiles[k].fd);
  return tmp;
}


static int re_match (Sed *s, Regex *re, const char *str, size_t len, size_t start, size_t *m) {
  if (re == NULL) re = s->last_re;
  if (re == NULL) {
    tool_err(s->err, "sed", "no previous regular expression");
    s->quit = 1;
    s->exit_code = 1;
    return 0;
  }
  s->last_re = re;
  return regex_match(re, str, len, start, 0, m);
}


static int addr_hit (Sed *s, SAddr *a) {
  switch (a->type) {
    case A_LINE: return s->lineno == a->n;
    case A_LAST: return is_last(s);
    case A_STEP:
      if (a->step <= 0) return s->lineno == a->n;
      return s->lineno >= a->n && (s->lineno - a->n) % a->step == 0;
    case A_RE: {
      ps_str(s);
      return re_match(s, a->re, s->ps.s, s->ps.len, 0, NULL);
    }
  }
  return 0;
}


static int selected (Sed *s, SCmd *c) {
  int m;
  if (c->a1.type == A_NONE) m = 1;
  else if (c->a2.type == A_NONE) m = addr_hit(s, &c->a1);
  else if (!c->active) {
    if ((c->a1.type == A_ZERO && s->lineno == 1) || addr_hit(s, &c->a1)) {
      m = 1;
      c->active = 1;
      switch (c->a2.type) {
        case A_LINE:
          if (c->a2.n <= s->lineno) c->active = 0;
          break;
        case A_PLUS:
          c->end = s->lineno + c->a2.n;
          if (c->a2.n == 0) c->active = 0;
          break;
        case A_MULT:
          if (c->a2.n <= 0 || s->lineno % c->a2.n == 0) c->active = 0;
          break;
        case A_LAST:
          if (is_last(s)) c->active = 0;
          break;
        case A_RE:
          /* 0,/re/: the first line can end it too */
          if (c->a1.type == A_ZERO && addr_hit(s, &c->a2)) c->active = 0;
          break;
      }
    }
    else m = 0;
  }
  else {
    m = 1;
    switch (c->a2.type) {
      case A_LINE: if (s->lineno >= c->a2.n) c->active = 0; break;
      case A_PLUS: if (s->lineno >= c->end) c->active = 0; break;
      case A_MULT: if (s->lineno % c->a2.n == 0) c->active = 0; break;
      case A_LAST: if (is_last(s)) c->active = 0; break;
      case A_RE: if (addr_hit(s, &c->a2)) c->active = 0; break;
    }
  }
  return c->neg ? !m : m;
}


static void flush_appends (Sed *s) {
  size_t i;
  for (i = 0; i < s->appends.n; i++) {
    const char *a = s->appends.v[i];
    if (a[0] == '\001') {	/* r file */
      char *native = path_to_native(a + 1), *text;
      size_t len;
      text = strcmp(a + 1, "/dev/stdin") == 0 ? NULL : read_file(native, &len);
      if (text) {
        out_putn(s->o, text, len);
        free(text);
      }
      free(native);
    }
    else if (a[0] == '\002') {	/* R: one line, already read, with its newline */
      out_puts(s->o, a + 1);
    }
    else out_puts(s->o, a);
  }
  vec_free(&s->appends);
  vec_init(&s->appends);
}


/* 'l': the pattern space, unambiguous */
static void do_list (Sed *s, int width) {
  const unsigned char *p;
  int col = 0, utf8 = tool_utf8();
  size_t i;
  Out *o = s->o;
  ps_str(s);
  p = (const unsigned char *)s->ps.s;
  if (width < 0) width = s->lwidth;
  for (i = 0; i < s->ps.len; i++) {
    char buf[16];
    int n = 0;
    unsigned char c = p[i];
    if (c == '\\') n = sprintf(buf, "\\\\");
    else if (c == '\a') n = sprintf(buf, "\\a");
    else if (c == '\b') n = sprintf(buf, "\\b");
    else if (c == '\f') n = sprintf(buf, "\\f");
    else if (c == '\n') n = sprintf(buf, "\\n");
    else if (c == '\r') n = sprintf(buf, "\\r");
    else if (c == '\t') n = sprintf(buf, "\\t");
    else if (c == '\v') n = sprintf(buf, "\\v");
    else if (c >= 32 && c < 127) { buf[0] = (char)c; n = 1; }
    else if (utf8 && c >= 0xC0) {
      int l = utf8_len((const char *)p + i), k;
      if (l > 1 && i + (size_t)l <= s->ps.len) {
        for (k = 0; k < l; k++) buf[k] = (char)p[i + (size_t)k];
        n = l;
        i += (size_t)l - 1;
        if (width > 1 && col + 1 > width - 1) {
          out_puts(o, "\\\n");
          col = 0;
        }
        out_putn(o, buf, (size_t)n);
        col++;
        continue;
      }
      n = sprintf(buf, "\\%03o", c);
    }
    else n = sprintf(buf, "\\%03o", c);
    if (width > 1 && col + n > width - 1) {
      out_puts(o, "\\\n");
      col = 0;
    }
    out_putn(o, buf, (size_t)n);
    col += n;
  }
  out_puts(o, "$\n");
}


static void case_put (Buf *b, const char *s, size_t n, int *mode, int *once) {
  size_t i;
  for (i = 0; i < n; i++) {
    int c = (unsigned char)s[i];
    if (*once) {
      c = *once == 'u' ? toupper(c) : tolower(c);
      *once = 0;
    }
    else if (*mode == 'U') c = toupper(c);
    else if (*mode == 'L') c = tolower(c);
    buf_putc(b, (char)c);
  }
}


static void expand_repl (SCmd *c, const char *src, const size_t *m, int nsub, Buf *b) {
  size_t i;
  int mode = 0, once = 0;
  for (i = 0; i < c->rlen; i++) {
    char ch = c->repl[i];
    if (ch == '\\' && i + 1 < c->rlen) {
      char n = c->repl[++i];
      if (n >= '0' && n <= '9') {
        int g = n - '0';
        if (g <= nsub && m[2 * g] != (size_t)-1)
          case_put(b, src + m[2 * g], m[2 * g + 1] - m[2 * g], &mode, &once);
      }
      else if (n == '&') case_put(b, "&", 1, &mode, &once);
      else if (n == 'n') buf_putc(b, '\n');
      else if (n == 't') buf_putc(b, '\t');
      else if (n == 'r') buf_putc(b, '\r');
      else if (n == 'a') buf_putc(b, '\a');
      else if (n == 'f') buf_putc(b, '\f');
      else if (n == 'v') buf_putc(b, '\v');
      else if (n == 'L' || n == 'U') { mode = n; once = 0; }
      else if (n == 'E') { mode = 0; once = 0; }
      else if (n == 'l' || n == 'u') once = n;
      else case_put(b, &n, 1, &mode, &once);
    }
    else if (ch == '&') case_put(b, src + m[0], m[1] - m[0], &mode, &once);
    else case_put(b, &ch, 1, &mode, &once);
  }
}


static int do_subst (Sed *s, SCmd *c) {
  Regex *re = c->re ? c->re : s->last_re;
  size_t *m, start = 0, done = 0, prev_end = (size_t)-1;
  int nsub, count = 0, did = 0;
  Buf out;
  const char *src;
  size_t len;
  if (re == NULL) {
    tool_err(s->err, "sed", "no previous regular expression");
    s->quit = 1;
    s->exit_code = 1;
    return 0;
  }
  s->last_re = re;
  nsub = regex_nsub(re);
  m = (size_t *)xmalloc(2 * ((size_t)nsub + 1) * sizeof(size_t));
  src = ps_str(s);
  len = s->ps.len;
  buf_init(&out);
  while (start <= len && regex_match(re, src, len, start, 0, m)) {
    /* an empty match right after the last match does not count */
    if (m[1] == m[0] && m[0] == prev_end) {
      if (m[0] >= len) break;
      start = m[0] + (size_t)(utf8_len(src + m[0]) > 0 ? utf8_len(src + m[0]) : 1);
      continue;
    }
    count++;
    if (count >= (c->nth ? c->nth : 1)) {
      buf_putn(&out, src + done, m[0] - done);
      expand_repl(c, src, m, nsub, &out);
      done = m[1];
      did = 1;
      if (!c->global) break;
    }
    prev_end = m[1];
    if (m[1] == m[0]) {	/* empty: step over one character */
      if (m[0] >= len) break;
      start = m[0] + (size_t)(utf8_len(src + m[0]) > 0 ? utf8_len(src + m[0]) : 1);
    }
    else start = m[1];
  }
  free(m);
  if (!did) {
    buf_free(&out);
    return 0;
  }
  buf_putn(&out, src + done, len - done);
  buf_free(&s->ps);
  s->ps = out;
  if (s->ps.s == NULL) buf_putn(&s->ps, "", 0);
  s->tflag = 1;
  if (c->eval) {	/* e: run the result, it becomes the output */
    size_t olen;
    char *res = sh_capture(ps_str(s), &olen);
    s->ps.len = 0;
    if (res) {
      while (olen > 0 && res[olen - 1] == '\n') olen--;
      buf_putn(&s->ps, res, olen);
      free(res);
    }
  }
  if (c->print) {
    int k;
    for (k = 0; k < c->print; k++) put_ps(s, s->o, s->ps.len, 1);
  }
  if (c->wfile >= 0) {
    Out tmp, *wo = wfile_out(s, c->wfile, &tmp);
    put_ps(s, wo, s->ps.len, 1);
    if (wo == &tmp) out_flush(&tmp);
  }
  return 1;
}


static void do_y (Sed *s, SCmd *c) {
  Buf out;
  size_t i = 0;
  ps_str(s);
  buf_init(&out);
  while (i < s->ps.len) {
    int l = utf8_len(s->ps.s + i), j;
    unsigned cp = 0;
    size_t k;
    if (l < 1 || i + (size_t)l > s->ps.len) l = 1;
    for (j = 0; j < l; j++) cp = (cp << 8) | (unsigned char)s->ps.s[i + (size_t)j];
    for (k = 0; k < c->ylen; k++)
      if (c->yfrom[k] == cp) {
        cp = c->yto[k];
        break;
      }
    if (k < c->ylen) {	/* the packed bytes back out */
      char b[4];
      int n = 0;
      unsigned v = cp;
      do {
        b[n++] = (char)(v & 0xFF);
        v >>= 8;
      } while (v && n < 4);
      while (n > 0) buf_putc(&out, b[--n]);
    }
    else buf_putn(&out, s->ps.s + i, (size_t)l);
    i += (size_t)l;
  }
  buf_free(&s->ps);
  s->ps = out;
  if (s->ps.s == NULL) buf_putn(&s->ps, "", 0);
}


enum { CYCLE_END, CYCLE_DELETE, CYCLE_RESTART, CYCLE_QUIT, CYCLE_QUIT_NOPRINT };

/* runs the script once over the pattern space */
static int run_cycle (Sed *s) {
  int pc = 0;
  while (pc < s->ncmds) {
    SCmd *c = &s->cmds[pc];
    if (tool_stop()) return CYCLE_QUIT_NOPRINT;
    if (c->cmd == '}' || c->cmd == ':') {
      pc++;
      continue;
    }
    if (!selected(s, c)) {
      pc = c->cmd == '{' ? c->jump : pc + 1;
      continue;
    }
    if (s->quit) return CYCLE_QUIT_NOPRINT;
    switch (c->cmd) {
      case '{': break;
      case '=': out_printf(s->o, "%lld\n", s->lineno); break;
      case 'a': {
        Buf b;
        buf_init(&b);
        buf_putn(&b, c->text, c->tlen);
        buf_putc(&b, '\n');
        vec_push(&s->appends, buf_take(&b));
        break;
      }
      case 'i':
        out_putn(s->o, c->text, c->tlen);
        out_putc(s->o, '\n');
        break;
      case 'c':
        /* a range: the text once, at its end */
        if (!(c->a2.type != A_NONE && c->active && !c->neg)) {
          out_putn(s->o, c->text, c->tlen);
          out_putc(s->o, '\n');
        }
        return CYCLE_DELETE;
      case 'b':
        pc = c->jump >= 0 ? c->jump : s->ncmds;
        continue;
      case 't':
        if (s->tflag) {
          s->tflag = 0;
          pc = c->jump >= 0 ? c->jump : s->ncmds;
          continue;
        }
        break;
      case 'T':
        if (!s->tflag) {
          pc = c->jump >= 0 ? c->jump : s->ncmds;
          continue;
        }
        s->tflag = 0;
        break;
      case 'd': return CYCLE_DELETE;
      case 'D': {
        char *nl;
        ps_str(s);
        nl = (char *)memchr(s->ps.s, s->zero ? '\0' : '\n', s->ps.len);
        if (nl == NULL) return CYCLE_DELETE;
        {
          size_t cut = (size_t)(nl - s->ps.s) + 1;
          memmove(s->ps.s, s->ps.s + cut, s->ps.len - cut);
          s->ps.len -= cut;
        }
        return CYCLE_RESTART;
      }
      case 'e': {
        size_t olen;
        char *res;
        out_flush(s->o);
        if (c->text && c->text[0]) {	/* e command: its output first */
          res = sh_capture(c->text, &olen);
          if (res) {
            out_putn(s->o, res, olen);
            free(res);
          }
        }
        else {
          res = sh_capture(ps_str(s), &olen);
          s->ps.len = 0;
          if (res) {
            while (olen > 0 && res[olen - 1] == '\n') olen--;
            buf_putn(&s->ps, res, olen);
            free(res);
          }
        }
        break;
      }
      case 'F':
        out_puts(s->o, s->cur_name && strcmp(s->cur_name, "-") != 0 ? s->cur_name : "-");
        out_putc(s->o, '\n');
        break;
      case 'g':
        s->ps.len = 0;
        buf_putn(&s->ps, s->hs.s ? s->hs.s : "", s->hs.len);
        break;
      case 'G':
        buf_putc(&s->ps, '\n');
        buf_putn(&s->ps, s->hs.s ? s->hs.s : "", s->hs.len);
        break;
      case 'h':
        s->hs.len = 0;
        buf_putn(&s->hs, ps_str(s), s->ps.len);
        break;
      case 'H':
        buf_putc(&s->hs, '\n');
        buf_putn(&s->hs, ps_str(s), s->ps.len);
        break;
      case 'x': {
        Buf t = s->ps;
        s->ps = s->hs;
        s->hs = t;
        if (s->ps.s == NULL) buf_putn(&s->ps, "", 0);
        break;
      }
      case 'l': do_list(s, c->has_num ? c->code : -1); break;
      case 'n':
        if (s->quit || is_last(s)) {
          if (!s->posix) return CYCLE_QUIT;
          return CYCLE_QUIT;
        }
        if (!s->quiet) put_ps(s, s->o, s->ps.len, s->ps_nl);
        flush_appends(s);
        next_line(s, 0);
        break;
      case 'N':
        if (is_last(s)) return s->posix ? CYCLE_QUIT_NOPRINT : CYCLE_QUIT;
        flush_appends(s);
        next_line(s, 1);
        break;
      case 'p': put_ps(s, s->o, s->ps.len, 1); break;
      case 'P': {
        char *nl;
        ps_str(s);
        nl = (char *)memchr(s->ps.s, '\n', s->ps.len);
        put_ps(s, s->o, nl ? (size_t)(nl - s->ps.s) : s->ps.len, 1);
        break;
      }
      case 'q':
        s->exit_code = c->code;
        return CYCLE_QUIT;
      case 'Q':
        s->exit_code = c->code;
        return CYCLE_QUIT_NOPRINT;
      case 'r':
        vec_push(&s->appends, xstrcat3("\001", c->text, ""));
        break;
      case 'R': {
        int k;
        RFile *rf = NULL;
        for (k = 0; k < s->nrfiles; k++)
          if (strcmp(s->rfiles[k].name, c->text) == 0) rf = &s->rfiles[k];
        if (rf == NULL) {
          s->rfiles = (RFile *)xrealloc(s->rfiles, (size_t)(s->nrfiles + 1) * sizeof(RFile));
          rf = &s->rfiles[s->nrfiles++];
          rf->name = xstrdup(c->text);
          rf->open = in_open(&rf->in, c->text, s->in) == 0 ? 1 : -1;
        }
        if (rf->open == 1) {
          char *line;
          size_t len;
          int had;
          if (in_line(&rf->in, &line, &len, '\n', &had)) {
            Buf b;
            buf_init(&b);
            buf_putc(&b, '\002');
            buf_putn(&b, line, len);
            buf_putc(&b, '\n');
            vec_push(&s->appends, buf_take(&b));
          }
        }
        break;
      }
      case 's': do_subst(s, c); break;
      case 'w': case 'W': {
        Out tmp, *wo = wfile_out(s, c->wfile, &tmp);
        char *nl;
        ps_str(s);
        nl = c->cmd == 'W' ? (char *)memchr(s->ps.s, '\n', s->ps.len) : NULL;
        put_ps(s, wo, nl ? (size_t)(nl - s->ps.s) : s->ps.len, 1);
        if (wo == &tmp) out_flush(&tmp);
        break;
      }
      case 'y': do_y(s, c); break;
      case 'z': s->ps.len = 0; break;
    }
    pc++;
  }
  return CYCLE_END;
}


/* the files as one stream (or one file, with -s / -i) */
static void run_stream (Sed *s, char **files, int nfiles) {
  int restart = 0;
  s->files = files;
  s->nfiles = nfiles;
  s->fi = 0;
  s->cur_open = 0;
  s->la_ok = 0;
  while (!s->quit) {
    int r;
    if (!restart) {
      if (!next_line(s, 0)) break;
      s->tflag = 0;
    }
    restart = 0;
    r = run_cycle(s);
    if (r == CYCLE_END || r == CYCLE_QUIT) {
      if (!s->quiet) put_ps(s, s->o, s->ps.len, s->ps_nl || !is_last(s));
    }
    if (r != CYCLE_QUIT_NOPRINT) flush_appends(s);
    else {
      vec_free(&s->appends);
      vec_init(&s->appends);
    }
    if (s->unbuffered) out_flush(s->o);
    if (r == CYCLE_QUIT || r == CYCLE_QUIT_NOPRINT) {
      s->quit = 1;
      break;
    }
    if (r == CYCLE_RESTART) {
      restart = 1;
      /* D: the next cycle starts on what is left, unless it is empty */
      if (s->ps.len == 0) restart = 0;
    }
    if (s->o->failed) break;
  }
  if (s->cur_open) {
    in_close(&s->cur);
    s->cur_open = 0;
  }
}


static int sed_inplace (Sed *s, const char *name) {
  char *native = path_to_native(name), *dir, *tmp, *files[1];
  OsStat st;
  int fd, k;
  Out o, *saved = s->o;
  char num[24];
  if (os_stat(native, &st) != 0 || st.is_dir) {
    if (st.is_dir) tool_err(s->err, "sed", "couldn't edit %s: not a regular file", name);
    else tool_err(s->err, "sed", "can't read %s: %s", name, os_errmsg());
    free(native);
    s->status = 4;
    return -1;
  }
  dir = path_dirname(native);
  for (k = 0;; k++) {
    char base[48];
    snprintf(base, sizeof(base), "sed%s.tmp", ll_to_str((long long)os_getpid() * 100 + k, num));
    tmp = path_join(dir, base);
    fd = os_open(tmp, OS_EXCL);
    if (fd >= 0 || k > 50) break;
    free(tmp);
  }
  free(dir);
  if (fd < 0) {
    tool_err(s->err, "sed", "couldn't open temporary file %s: %s", tmp, os_errmsg());
    free(tmp);
    free(native);
    s->status = 4;
    return -1;
  }
  out_init(&o, fd);
  s->o = &o;
  files[0] = (char *)name;
  s->lineno = 0;
  run_stream(s, files, 1);
  out_flush(&o);
  os_close(fd);
  s->o = saved;
  os_chmod(tmp, st.mode);
  if (s->suffix && *s->suffix) {	/* the backup: name + suffix, or * is the name */
    Buf b;
    char *bn;
    const char *p;
    buf_init(&b);
    if (strchr(s->suffix, '*')) {
      for (p = s->suffix; *p; p++) {
        if (*p == '*') buf_puts(&b, tool_base(name));
        else buf_putc(&b, *p);
      }
      if (strchr(s->suffix, '/') == NULL) {
        char *d = path_dirname(native), *j = path_join(d, b.s);
        free(d);
        bn = j;
      }
      else bn = path_to_native(b.s);
    }
    else {
      buf_puts(&b, native);
      buf_puts(&b, s->suffix);
      bn = xstrdup(b.s);
    }
    buf_free(&b);
    os_rename(native, bn);
    free(bn);
  }
  if (os_rename(tmp, native) != 0) {
    tool_err(s->err, "sed", "cannot rename %s: %s", tmp, os_errmsg());
    os_unlink(tmp);
    s->status = 4;
  }
  free(tmp);
  free(native);
  return 0;
}


int t_sed (int argc, char **argv, int in, int out, int err) {
  static const LongOpt lo[] = {
    {"quiet", 'n', 0}, {"silent", 'n', 0}, {"expression", 'e', 1}, {"file", 'f', 1},
    {"regexp-extended", 'E', 0}, {"in-place", 'i', 2}, {"separate", 's', 0},
    {"unbuffered", 'u', 0}, {"null-data", 'z', 0}, {"zero-terminated", 'z', 0},
    {"line-length", 'l', 1}, {"posix", 1001, 0}, {"debug", 1002, 0}, {"sandbox", 1003, 0},
    {"follow-symlinks", 1003, 0}, {"binary", 'b', 0}, {NULL, 0, 0}};
  Sed s;
  Opts g;
  Out o;
  Buf script;
  int c, have_script = 0, i, nexpr = 0;
  char num[24];
  char **av;
  Vec made;
  memset(&s, 0, sizeof(s));
  s.in = in;
  s.out = out;
  s.err = err;
  s.lwidth = 70;
  vec_init(&s.appends);
  vec_init(&s.chunk_names);
  vec_init(&s.chunk_starts);
  buf_init(&script);
  buf_init(&s.ps);
  buf_init(&s.hs);
  /* -i takes its suffix only when glued: -i.bak; -i alone is no suffix */
  av = (char **)xmalloc(((size_t)argc + 1) * sizeof(char *));
  vec_init(&made);
  for (i = 0; i < argc; i++) av[i] = argv[i];
  av[argc] = NULL;
  for (i = 1; i < argc; i++) {
    const char *p;
    if (strcmp(argv[i], "--") == 0) break;
    if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "-l") == 0) {
      i++;
      continue;
    }
    if (argv[i][0] != '-' || argv[i][1] == '-' || (p = strchr(argv[i], 'i')) == NULL) continue;
    if (strspn(argv[i] + 1, "nEsuzri") < (size_t)(p - argv[i])) continue;
    s.inplace = 1;
    s.suffix = p[1] ? p + 1 : NULL;
    av[i] = (p - argv[i] == 1) ? xstrdup("-s") : xstrndup(argv[i], (size_t)(p - argv[i]));
    vec_push(&made, av[i]);
  }
  opts_init(&g, "sed", argc, av, err);
  while ((c = opts_next(&g, "ne:f:ErsuzEl:b", lo)) != 0) {
    switch (c) {
      case 'n': s.quiet = 1; break;
      case 'e':
        if (script.len > 0) buf_putc(&script, '\n');
        vec_push(&s.chunk_starts, xstrdup(ll_to_str((long long)script.len, num)));
        vec_push(&s.chunk_names, xstrcat3("-", ll_to_str(++nexpr, num), ""));
        buf_puts(&script, g.arg);
        have_script = 1;
        break;
      case 'f': {
        char *native = path_to_native(g.arg), *text;
        size_t len;
        text = read_file(native, &len);
        free(native);
        if (text == NULL) {
          tool_err(err, "sed", "couldn't open file %s: %s", g.arg, os_errmsg());
          goto bad;
        }
        crlf_to_lf(text, &len);
        if (len > 0 && text[len - 1] == '\n') text[--len] = '\0';
        if (script.len > 0) buf_putc(&script, '\n');
        vec_push(&s.chunk_starts, xstrdup(ll_to_str((long long)script.len, num)));
        vec_push(&s.chunk_names, xstrdup(g.arg));
        buf_putn(&script, text, len);
        free(text);
        have_script = 1;
        break;
      }
      case 'E': case 'r': s.ere = 1; break;
      case 'i': s.inplace = 1; s.suffix = g.arg; break;
      case 's': s.separate = 1; break;
      case 'u': s.unbuffered = 1; break;
      case 'z': s.zero = 1; break;
      case 'l': s.lwidth = atoi(g.arg); break;
      case 1001: s.posix = 1; break;
      case 1002: case 1003: case 'b': break;
      case OPT_HELP: opts_free(&g); buf_free(&script); return tool_help(out, "sed");
      default: goto bad;
    }
  }
  if (!have_script) {
    if (g.ops.n == 0) {
      fd_printf(err, "Usage: sed [OPTION]... {script-only-if-no-other-script} [input-file]...\n");
      goto bad;
    }
    vec_push(&s.chunk_starts, xstrdup("0"));
    vec_push(&s.chunk_names, xstrdup("-1"));
    buf_puts(&script, g.ops.v[0]);
    free(g.ops.v[0]);
    memmove(g.ops.v, g.ops.v + 1, g.ops.n * sizeof(char *));
    g.ops.n--;
  }
  if (script.len >= 2 && script.s[0] == '#' && script.s[1] == 'n' &&
      (script.len == 2 || script.s[2] == '\n'))
    s.quiet = 1;
  s.src = script.s ? script.s : "";
  s.p = s.src;
  s.end = s.src + script.len;
  if (parse_script(&s) != 0) {
    s.exit_code = 1;
    goto done;
  }
  out_init(&o, out);
  s.o = &o;
  if (os_is_tty(out)) s.unbuffered = 1;
  if (s.inplace) {
    size_t k;
    if (g.ops.n == 0) {
      tool_err(err, "sed", "no input files");
      s.exit_code = 1;
      goto done;
    }
    for (k = 0; k < g.ops.n && !s.quit; k++) sed_inplace(&s, g.ops.v[k]);
  }
  else {
    if (g.ops.n == 0) vec_push(&g.ops, xstrdup("-"));
    if (s.separate) {
      size_t k;
      for (k = 0; k < g.ops.n && !s.quit; k++) {
        s.lineno = 0;
        run_stream(&s, g.ops.v + k, 1);
      }
    }
    else run_stream(&s, g.ops.v, (int)g.ops.n);
  }
  out_flush(&o);
done:
  {
    int k;
    for (k = 0; k < s.ncmds; k++) {
      SCmd *cm = &s.cmds[k];
      free(cm->text);
      free(cm->repl);
      if (cm->re) regex_free(cm->re);
      if (cm->a1.re) regex_free(cm->a1.re);
      if (cm->a2.re) regex_free(cm->a2.re);
      free(cm->yfrom);
      free(cm->yto);
    }
    free(s.cmds);
    for (k = 0; k < s.nwfiles; k++) {
      if (s.wfiles[k].own) os_close(s.wfiles[k].fd);
      free(s.wfiles[k].name);
    }
    free(s.wfiles);
    for (k = 0; k < s.nrfiles; k++) {
      if (s.rfiles[k].open == 1) in_close(&s.rfiles[k].in);
      free(s.rfiles[k].name);
    }
    free(s.rfiles);
  }
  free(s.la);
  buf_free(&s.ps);
  buf_free(&s.hs);
  buf_free(&script);
  vec_free(&s.appends);
  vec_free(&s.chunk_names);
  vec_free(&s.chunk_starts);
  opts_free(&g);
  vec_free(&made);
  free(av);
  if (s.exit_code) return s.exit_code;
  return tool_stop() ? 130 : s.status;
bad:
  buf_free(&script);
  vec_free(&s.chunk_names);
  vec_free(&s.chunk_starts);
  opts_free(&g);
  vec_free(&made);
  free(av);
  return 1;
}
