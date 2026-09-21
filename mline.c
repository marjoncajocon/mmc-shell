/*
** mline.c - line editor
**
** One implementation for every system: the terminal is put in raw mode
** and talks VT sequences (Windows 10+ consoles do that too).
** Editing, history and Tab completion; UTF-8 aware.
*/

#include "mmc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
** {==================================================================
** History
** ===================================================================
*/

static Vec hist;
static char *hist_file = NULL;
static size_t hist_written = 0;	/* lines already in the file: for history -a */


const Vec *line_hist (void) {
  return &hist;
}


/* $HISTSIZE lines are kept in memory, $HISTFILESIZE in the file */
static size_t hist_limit (const char *name, size_t fallback) {
  const char *v = var_get(name);
  long long n;
  if (v == NULL || *v == '\0') return fallback;
  if (str_to_ll(v, &n) != 0) return fallback;
  if (n < 0) return (size_t)-1;	/* negative: no limit, like bash */
  return (size_t)n;
}


/* what $HISTCONTROL says; unset means ignoreboth, as mmc always did */
enum { H_IGNSPACE = 1, H_IGNDUPS = 2, H_ERASEDUPS = 4 };

static int hist_control (void) {
  const char *v = var_get("HISTCONTROL");
  int flags = 0;
  if (v == NULL) return H_IGNSPACE | H_IGNDUPS;
  while (*v != '\0') {
    size_t n = strcspn(v, ":");
    if (n == 11 && strncmp(v, "ignorespace", n) == 0) flags |= H_IGNSPACE;
    else if (n == 10 && strncmp(v, "ignoredups", n) == 0) flags |= H_IGNDUPS;
    else if (n == 10 && strncmp(v, "ignoreboth", n) == 0) flags |= H_IGNSPACE | H_IGNDUPS;
    else if (n == 9 && strncmp(v, "erasedups", n) == 0) flags |= H_ERASEDUPS;
    v += n;
    if (*v == ':') v++;
  }
  return flags;
}


static void hist_trim (void) {
  size_t max = hist_limit("HISTSIZE", MMC_HISTORY_MAX);
  while (hist.n > max) {
    free(hist.v[0]);
    memmove(hist.v, hist.v + 1, hist.n * sizeof(char *));	/* with NULL */
    hist.n--;
    if (hist_written > 0) hist_written--;
  }
}


static void hist_push (const char *s) {
  if (hist.n > 0 && strcmp(hist.v[hist.n - 1], s) == 0) return;
  vec_push(&hist, xstrdup(s));
  hist_trim();
}


void line_hist_load (const char *native) {
  char *text = read_file(native, NULL);
  char *line;
  size_t lines = 0;
  free(hist_file);
  hist_file = xstrdup(native);
  if (text == NULL) return;
  for (line = text; *line != '\0';) {
    char *end = line + strcspn(line, "\n");
    int last = (*end == '\0');
    *end = '\0';
    if (end > line && end[-1] == '\r') end[-1] = '\0';
    if (line[0] != '\0') hist_push(line);
    lines++;
    if (last) break;
    line = end + 1;
  }
  free(text);
  hist_written = hist.n;
  if (lines > 2 * hist_limit("HISTFILESIZE", MMC_HISTORY_MAX)) {	/* it grew too much: rewrite it */
    line_hist_write(hist_file);
  }
}


void line_hist_add (const char *s) {
  int ctl = hist_control();
  const char *last = hist.n ? hist.v[hist.n - 1] : NULL;
  size_t before = hist.n;
  if (s[0] == '\0') return;
  if ((ctl & H_IGNSPACE) && s[0] == ' ') return;
  if ((ctl & H_IGNDUPS) && last != NULL && strcmp(last, s) == 0) return;
  if (ctl & H_ERASEDUPS) {	/* keep only the newest of the same line */
    size_t i = 0;
    while (i < hist.n) {
      if (strcmp(hist.v[i], s) == 0) line_hist_delete((int)i);
      else i++;
    }
  }
  hist_push(s);
  if (hist.n == before && !(ctl & H_ERASEDUPS)) return;	/* nothing new */
  if (hist_file != NULL) {
    int fd = os_open(hist_file, OS_APPEND);
    if (fd >= 0) {
      fd_printf(fd, "%s\n", s);
      os_close(fd);
      hist_written = hist.n;
    }
  }
}


/* history -a: the lines added since the file was last written */
void line_hist_append (const char *native) {
  int fd;
  size_t i;
  if (hist_written >= hist.n) return;
  fd = os_open(native, OS_APPEND);
  if (fd < 0) return;
  for (i = hist_written; i < hist.n; i++) fd_printf(fd, "%s\n", hist.v[i]);
  os_close(fd);
  hist_written = hist.n;
}


void line_hist_delete (int index) {
  if (index < 0 || (size_t)index >= hist.n) return;
  free(hist.v[index]);
  memmove(hist.v + index, hist.v + index + 1, (hist.n - (size_t)index) * sizeof(char *));
  hist.n--;
}


void line_hist_write (const char *native) {
  int fd = os_open(native, OS_WRITE);
  size_t i;
  if (fd < 0) return;
  for (i = 0; i < hist.n; i++) fd_printf(fd, "%s\n", hist.v[i]);
  os_close(fd);
  if (hist_file != NULL && strcmp(native, hist_file) == 0) hist_written = hist.n;
}


const char *line_hist_file (void) {
  return hist_file;
}


void line_hist_clear (void) {
  vec_free(&hist);
  hist_written = 0;
  if (hist_file != NULL) {
    int fd = os_open(hist_file, OS_WRITE);
    if (fd >= 0) os_close(fd);
  }
}

/* }================================================================== */


typedef struct Edit {
  char *buf;
  size_t len, cap, pos;
  const char *prompt;
  size_t pwidth;
  size_t crow;	/* the row the cursor was left on, counted from the prompt's */
  size_t erow;	/* the last row the line took */
} Edit;


static void ed_insert (Edit *e, const char *s, size_t n) {
  if (e->len + n + 1 > e->cap) {
    while (e->len + n + 1 > e->cap) e->cap *= 2;
    e->buf = (char *)xrealloc(e->buf, e->cap);
  }
  memmove(e->buf + e->pos + n, e->buf + e->pos, e->len - e->pos + 1);
  memcpy(e->buf + e->pos, s, n);
  e->len += n;
  e->pos += n;
}


static void ed_delete (Edit *e, size_t from, size_t to) {
  if (from >= to) return;
  memmove(e->buf + from, e->buf + to, e->len - to + 1);
  e->len -= to - from;
  if (e->pos >= to) e->pos -= to - from;
  else if (e->pos > from) e->pos = from;
}


static void ed_set (Edit *e, const char *s) {
  e->len = e->pos = 0;
  e->buf[0] = '\0';
  ed_insert(e, s, strlen(s));
}


static size_t ed_prev (const Edit *e, size_t p) {
  if (p == 0) return 0;
  do p--; while (p > 0 && ((unsigned char)e->buf[p] & 0xC0) == 0x80);
  return p;
}


static size_t ed_next (const Edit *e, size_t p) {
  if (p >= e->len) return e->len;
  do p++; while (p < e->len && ((unsigned char)e->buf[p] & 0xC0) == 0x80);
  return p;
}


static size_t ed_word_left (const Edit *e, size_t p) {
  while (p > 0 && e->buf[p - 1] == ' ') p--;
  while (p > 0 && e->buf[p - 1] != ' ') p--;
  return p;
}


static size_t ed_word_right (const Edit *e, size_t p) {
  while (p < e->len && e->buf[p] == ' ') p++;
  while (p < e->len && e->buf[p] != ' ') p++;
  return p;
}


/* the code point at s, and its length in *n */
static unsigned ed_decode (const char *s, size_t *n) {
  const unsigned char *u = (const unsigned char *)s;
  unsigned cp = u[0];
  size_t k, need = (cp >= 0xF0) ? 4 : (cp >= 0xE0) ? 3 : (cp >= 0xC0) ? 2 : 1;
  if (need > 1) cp &= 0x3F >> (need - 1);
  for (k = 1; k < need; k++) {
    if ((u[k] & 0xC0) != 0x80) break;
    cp = (cp << 6) | (u[k] & 0x3F);
  }
  *n = k;
  return cp;
}


static void put_rows (Buf *o, size_t n, char dir) {
  char move[32];
  if (n == 0) return;
  sprintf(move, "\033[%lu%c", (unsigned long)n, dir);
  buf_puts(o, move);
}


/*
** Redraws the line. A long line goes on over as many rows as it needs:
** the terminal wraps it, we only keep count of where each character
** lands, so that the cursor can be put back on the right one.
*/
static void ed_refresh (Edit *e) {
  Buf o;
  int cols = os_term_cols();
  size_t row, col, crow = 0, ccol = 0, k;
  int pending;	/* the last column is full: the terminal wraps on the next character */
  if (cols < 20) cols = 80;
  row = e->pwidth / (size_t)cols;
  col = e->pwidth % (size_t)cols;
  pending = (e->pwidth > 0 && col == 0);
  buf_init(&o);
  put_rows(&o, e->crow, 'A');
  buf_putc(&o, '\r');
  buf_puts(&o, e->prompt);
  for (k = 0; k <= e->len;) {
    size_t n = 1;
    unsigned cp;
    int w;
    if (k == e->pos) {
      crow = row;
      ccol = col;
    }
    if (k == e->len) break;
    cp = ed_decode(e->buf + k, &n);
    w = (cp == '\n' || cp == '\t') ? 1 : uc_width(cp);
    if (w == 2 && col + 2 > (size_t)cols) {	/* no room for a wide one: next row */
      buf_puts(&o, "\033[K\r\n");
      row++;
      col = 0;
      if (k == e->pos) {
        crow = row;
        ccol = 0;
      }
    }
    /* a pasted block keeps its newlines: they show as a return sign */
    if (cp == '\n') buf_puts(&o, "\xe2\x86\xb5");
    else if (cp == '\t') buf_putc(&o, ' ');
    else buf_putn(&o, e->buf + k, n);
    k += n;
    if (w > 0) pending = 0;
    col += (size_t)w;
    if (col >= (size_t)cols) {
      row++;
      col = 0;
      pending = 1;
    }
  }
  if (pending) buf_puts(&o, "\r\n");	/* go down for real, so that the rows add up */
  buf_puts(&o, "\033[J");
  put_rows(&o, row - crow, 'A');
  buf_putc(&o, '\r');
  put_rows(&o, ccol, 'C');
  e->crow = crow;
  e->erow = row;
  os_write(1, o.s, o.len);
  buf_free(&o);
}


/* the cursor to a new row under the whole line, for output of our own */
static void ed_leave (Edit *e) {
  Buf o;
  buf_init(&o);
  put_rows(&o, e->erow - e->crow, 'B');
  buf_puts(&o, "\r\n");
  os_write(1, o.s, o.len);
  buf_free(&o);
  e->crow = e->erow = 0;
}


/*
** {==================================================================
** Tab completion
** ===================================================================
*/

static int starts_with (const char *s, const char *prefix) {
  return m_fnncmp(s, prefix, strlen(prefix)) == 0;
}


static void push_unique (Vec *v, char *s) {
  size_t i;
  for (i = 0; i < v->n; i++) {
    if (strcmp(v->v[i], s) == 0) {
      free(s);
      return;
    }
  }
  vec_push(v, s);
}


static void complete_commands (const char *prefix, Vec *out) {
  Vec names, dirs;
  size_t i, k;
  char *path = var_get("PATH") ? xstrdup(var_get("PATH")) : NULL;
  vec_init(&names);
  vec_init(&dirs);
  builtin_names(&names);
  alias_names(&names);
  func_names(&names);
  for (i = 0; i < names.n; i++)
    if (strncmp(names.v[i], prefix, strlen(prefix)) == 0)
      push_unique(out, xstrdup(names.v[i]));
  if (path) path_list_split(path, &dirs);
  for (i = 0; i < dirs.n; i++) {
    Vec files;
    char *dir = path_to_native(dirs.v[i]);
    vec_init(&files);
    os_listdir(dir, &files);
    for (k = 0; k < files.n; k++) {
      char *name = files.v[k];
      if (!starts_with(name, prefix)) continue;
#ifdef _WIN32
      {	/* only programs, shown without their extension */
        char *dot = strrchr(name, '.');
        if (dot == NULL || (m_stricmp(dot, ".exe") != 0 &&
            m_stricmp(dot, ".cmd") != 0 && m_stricmp(dot, ".bat") != 0 &&
            m_stricmp(dot, ".com") != 0))
          continue;
        *dot = '\0';
      }
#endif
      push_unique(out, xstrdup(name));
    }
    free(dir);
    vec_free(&files);
  }
  free(path);
  vec_free(&names);
  vec_free(&dirs);
}


/* 'word' is "dir/prefix"; candidates are whole words again */
static void complete_files (const char *word, Vec *out) {
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
    if (!starts_with(files.v[i], base)) continue;
    if (files.v[i][0] == '.' && base[0] != '.') continue;
    full = path_join(native, files.v[i]);
    os_stat(full, &st);
    free(full);
    vec_push(out, xstrcat3(dirpart, files.v[i], st.is_dir ? "/" : ""));
  }
  vec_free(&files);
  free(native);
  free(lookup);
  free(dirpart);
}


static size_t common_prefix (const Vec *v) {
  size_t n = strlen(v->v[0]), i, k;
  for (i = 1; i < v->n; i++) {
    for (k = 0; k < n && v->v[i][k] != '\0'; k++)
      if (m_fnncmp(v->v[0] + k, v->v[i] + k, 1) != 0) break;
    n = k;
  }
  while (n > 0 && ((unsigned char)v->v[0][n] & 0xC0) == 0x80) n--;
  return n;
}


/* what a candidate shows as: the last part of its path, "dir/" kept whole */
static const char *shown_name (const char *s) {
  size_t n = strlen(s);
  if (n > 1 && s[n - 1] == '/') {
    const char *p = s + n - 1;
    while (p > s && p[-1] != '/') p--;
    return p;
  }
  return path_basename(s);
}


static void show_candidates (const Vec *v) {
  Buf o;
  size_t i, width = 0, cols, col = 0;
  size_t limit = v->n > 120 ? 120 : v->n;
  buf_init(&o);
  for (i = 0; i < limit; i++) {
    const char *s = shown_name(v->v[i]);
    size_t w = utf8_count(s, strlen(s));
    if (w > width) width = w;
  }
  width += 3;
  cols = (size_t)os_term_cols() / width;
  if (cols < 1) cols = 1;
  for (i = 0; i < limit; i++) {
    const char *s = shown_name(v->v[i]);
    size_t w = utf8_count(s, strlen(s));
    buf_puts(&o, s);
    if (++col >= cols || i + 1 == limit) {
      buf_puts(&o, "\r\n");
      col = 0;
    }
    else for (; w < width; w++) buf_putc(&o, ' ');
  }
  if (limit < v->n) {
    char more[64];
    sprintf(more, "... and %lu more\r\n", (unsigned long)(v->n - limit));
    buf_puts(&o, more);
  }
  os_write(1, o.s, o.len);
  buf_free(&o);
}


/*
** The words of the line up to the cursor, for COMP_WORDS: quotes are
** kept as they were typed, blanks separate, an escaped blank does not.
*/
void line_words_at (const char *line, size_t upto, Vec *out, size_t *cword) {
  size_t i = 0;
  *cword = 0;
  while (i < upto) {
    Buf w;
    int any = 0;
    while (i < upto && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= upto) break;
    buf_init(&w);
    while (i < upto && line[i] != ' ' && line[i] != '\t') {
      if (line[i] == '\\' && i + 1 < upto) {
        buf_putc(&w, line[i + 1]);
        i += 2;
      }
      else if (line[i] == '\'' || line[i] == '"') {
        char q = line[i++];
        while (i < upto && line[i] != q) buf_putc(&w, line[i++]);
        if (i < upto) i++;
      }
      else buf_putc(&w, line[i++]);
      any = 1;
    }
    vec_push(out, any && w.s ? buf_take(&w) : xstrdup(""));
    if (any && !w.s) buf_free(&w);
  }
  /* the word the cursor is in (a blank before it means a new, empty one) */
  if (upto > 0 && (line[upto - 1] == ' ' || line[upto - 1] == '\t')) {
    vec_push(out, xstrdup(""));
    *cword = out->n - 1;
  }
  else *cword = out->n > 0 ? out->n - 1 : 0;
}


static void ed_complete (Edit *e) {
  Vec cands;
  Buf word, esc;
  size_t ws = e->pos, i, keep;
  int command;
  unsigned copts = 0;
  int from_rule = 0;
  while (ws > 0 && (e->buf[ws - 1] != ' ' ||
                    (ws > 1 && e->buf[ws - 2] == '\\')))
    ws--;
  buf_init(&word);
  for (i = ws; i < e->pos; i++) {	/* undo "\ " escapes */
    if (e->buf[i] == '\\' && i + 1 < e->pos) i++;
    buf_putc(&word, e->buf[i]);
  }
  buf_putc(&word, '\0');
  for (i = ws; i > 0 && e->buf[i - 1] == ' '; i--)
    ;
  command = (i == 0 || strchr("|;&", e->buf[i - 1]) != NULL) &&
            word.s[0] != '\0' && strchr(word.s, '/') == NULL;
  vec_init(&cands);
  {	/* a rule from "complete" comes first: git, npm, docker ... */
    Vec words;
    size_t cword = 0;
    vec_init(&words);
    line_words_at(e->buf, e->pos, &words, &cword);
    from_rule = comp_for_line(e->buf, e->pos, &words, cword, word.s, &cands, &copts);
    vec_free(&words);
  }
  if (!from_rule) {
    if (command) complete_commands(word.s, &cands);
    if (cands.n == 0) {
      command = 0;
      complete_files(word.s, &cands);
    }
  }
  if (cands.n == 0) {
    os_write(1, "\a", 1);
    buf_free(&word);
    vec_free(&cands);
    return;
  }
  if (from_rule && (copts & COMP_FILENAMES)) {	/* -o filenames: mark the folders */
    for (i = 0; i < cands.n; i++) {
      OsStat st;
      char *old = cands.v[i];
      char *native = path_to_native(old);
      size_t len = strlen(old);
      if (os_stat(native, &st) == 0 && st.is_dir && (len == 0 || old[len - 1] != '/')) {
        cands.v[i] = xstrcat3(old, "/", "");
        free(old);
      }
      free(native);
    }
  }
  if (!(copts & COMP_NOSORT)) vec_sort(&cands);
  keep = common_prefix(&cands);
  if (cands.n > 1 && keep <= strlen(word.s)) {
    ed_leave(e);
    show_candidates(&cands);
    buf_free(&word);
    vec_free(&cands);
    return;
  }
  buf_init(&esc);
  for (i = 0; i < keep; i++) {
    char c = cands.v[0][i];
    /* what a rule gave goes in as it is (git's own completion puts the
    ** space there itself); only file names are protected */
    int quote = !from_rule || (copts & COMP_FILENAMES) != 0;
    if (quote && !(copts & COMP_NOQUOTE) && strchr(" \t\"'$;&|<>#*?[()", c) != NULL)
      buf_putc(&esc, '\\');
    buf_putc(&esc, c);
  }
  if (cands.n == 1 && !(copts & COMP_NOSPACE) &&
      (keep == 0 || cands.v[0][keep - 1] != '/'))
    buf_putc(&esc, ' ');
  ed_delete(e, ws, e->pos);
  ed_insert(e, esc.s ? esc.s : "", esc.len);
  buf_free(&esc);
  buf_free(&word);
  vec_free(&cands);
}

/* }================================================================== */


/* plain read for pipes, files and terminals we cannot drive */
static char *read_plain (int tty) {
  Buf b;
  int c, any = 0;
  buf_init(&b);
  for (;;) {
    unsigned char ch;
    if (tty) c = os_tty_getbyte();
    else c = (os_read(0, &ch, 1) == 1) ? ch : -1;
    if (c < 0) break;
    any = 1;
    if (c == '\n') break;
    buf_putc(&b, (char)c);
  }
  if (!any) {
    buf_free(&b);
    return NULL;
  }
  if (b.len > 0 && b.s[b.len - 1] == '\r') b.s[--b.len] = '\0';
  return buf_take(&b);
}


/* reads the rest of an escape sequence; returns an editing action */
enum { K_NONE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_DEL,
       K_WLEFT, K_WRIGHT, K_WDEL, K_PASTE, K_KILLWORD, K_LASTARG, K_YANKPOP,
       K_UPCASE, K_DOWNCASE, K_CAPWORD, K_HISTFIRST, K_HISTLAST };

static int read_escape (void) {
  char seq[16];
  int n = 0, c = os_tty_getbyte();
  switch (c) {	/* Alt and a key */
    case 'b': return K_WLEFT;
    case 'f': return K_WRIGHT;
    case 127: case 8: return K_WDEL;
    case 'd': return K_KILLWORD;
    case '.': case '_': return K_LASTARG;
    case 'y': return K_YANKPOP;
    case 'u': return K_UPCASE;
    case 'l': return K_DOWNCASE;
    case 'c': return K_CAPWORD;
    case '<': return K_HISTFIRST;
    case '>': return K_HISTLAST;
  }
  if (c != '[' && c != 'O') return K_NONE;
  for (;;) {
    c = os_tty_getbyte();
    if (c < 0) return K_NONE;
    if (c >= 0x40 && c <= 0x7E) break;
    if (n < (int)sizeof(seq) - 1) seq[n++] = (char)c;
  }
  seq[n] = '\0';
  switch (c) {
    case 'A': return K_UP;
    case 'B': return K_DOWN;
    case 'C': return strchr(seq, ';') ? K_WRIGHT : K_RIGHT;
    case 'D': return strchr(seq, ';') ? K_WLEFT : K_LEFT;
    case 'H': return K_HOME;
    case 'F': return K_END;
    case '~':
      switch (atoi(seq)) {
        case 1: case 7: return K_HOME;
        case 4: case 8: return K_END;
        case 3: return strchr(seq, ';') ? K_KILLWORD : K_DEL;	/* Ctrl-Delete */
        case 200: return K_PASTE;	/* bracketed paste starts */
      }
  }
  return K_NONE;
}


/* the text of a bracketed paste, up to ESC [ 2 0 1 ~ */
static void read_paste (Edit *e) {
  static const char end[] = "\033[201~";
  Buf b;
  size_t match = 0;
  int last = 0;
  buf_init(&b);
  for (;;) {
    int c = os_tty_getbyte();
    if (c < 0) break;
    if ((char)c == end[match]) {
      if (++match == sizeof(end) - 1) break;
      continue;
    }
    if (match > 0) {	/* a false start: keep what looked like the end */
      buf_putn(&b, end, match);
      match = 0;
      if ((char)c == end[0]) {
        match = 1;
        continue;
      }
    }
    if (c == '\n' && last == '\r') {	/* CRLF: the \r already made the newline */
      last = c;
      continue;
    }
    last = c;
    if (c == '\r') c = '\n';
    buf_putc(&b, (char)c);
  }
  if (b.len > 0) ed_insert(e, b.s, b.len);
  buf_free(&b);
}


/*
** {==================================================================
** Ctrl-R: search backwards through the history while typing
** ===================================================================
*/

/* the newest entry at or before 'from' that contains 'what' ((size_t)-1: none) */
static size_t hist_find (const char *what, size_t from, int back) {
  size_t i = from;
  if (what[0] == '\0') return (hist.n > 0 && back) ? hist.n - 1 : (size_t)-1;
  if (back) {
    while (i != (size_t)-1) {
      if (i < hist.n && strstr(hist.v[i], what) != NULL) return i;
      i--;
    }
  }
  else {
    for (; i < hist.n; i++)
      if (strstr(hist.v[i], what) != NULL) return i;
  }
  return (size_t)-1;
}


static void search_show (const char *what, const char *line, int back, int failed) {
  Buf o;
  int cols = os_term_cols();
  size_t avail;
  buf_init(&o);
  buf_putc(&o, '\r');
  if (failed) buf_puts(&o, "(failed ");
  buf_puts(&o, back ? "(reverse-i-search)`" : "(i-search)`");
  buf_puts(&o, what);
  buf_puts(&o, "': ");
  if (cols < 20) cols = 80;
  avail = (size_t)cols - 1;
  if (o.len < avail) {	/* the match, cut off at the right edge */
    size_t room = avail - utf8_count(o.s, o.len), k = 0, shown = 0;
    while (line[k] != '\0' && shown < room) {
      if (((unsigned char)line[k] & 0xC0) != 0x80) shown++;
      buf_putc(&o, line[k] == '\n' ? ' ' : line[k]);
      k++;
    }
  }
  buf_puts(&o, "\033[K");
  os_write(1, o.s, o.len);
  buf_free(&o);
}


/*
** Runs the search. The line found is put into 'e'. Returns 0 when the
** search was dropped (Ctrl-G, Ctrl-C) and the old line must come back,
** 1 when a line was taken over, and 2 when Enter asked to run it too.
** *key is an editing action still to do (an arrow key ends the search).
*/
static int ed_search (Edit *e, int *key) {
  Buf what;
  size_t found = (size_t)-1;
  int back = 1, failed = 0, status = 1;
  buf_init(&what);
  *key = K_NONE;
  {	/* the search takes the line's place: all of its rows go */
    Buf o;
    buf_init(&o);
    put_rows(&o, e->crow, 'A');
    buf_puts(&o, "\r\033[J");
    os_write(1, o.s, o.len);
    buf_free(&o);
    e->crow = e->erow = 0;
  }
  search_show("", "", back, 0);	/* an empty search shows an empty line, like bash */
  for (;;) {
    int c = os_tty_getbyte();
    size_t next;
    if (c < 0) {
      status = 0;
      break;
    }
    if (c == 18 || c == 19) {	/* Ctrl-R, Ctrl-S: the next match */
      back = (c == 18);
      if (found == (size_t)-1) next = hist_find(what.s ? what.s : "", back ? (hist.n ? hist.n - 1 : (size_t)-1) : 0, back);
      else if (back) next = (found == 0) ? (size_t)-1 : hist_find(what.s ? what.s : "", found - 1, back);
      else next = hist_find(what.s ? what.s : "", found + 1, back);
      if (next == (size_t)-1) failed = 1;
      else {
        found = next;
        failed = 0;
      }
    }
    else if (c == 127 || c == 8) {	/* Backspace: one character less */
      if (what.len > 0) {
        size_t k = what.len;
        do k--; while (k > 0 && ((unsigned char)what.s[k] & 0xC0) == 0x80);
        what.len = k;
        what.s[k] = '\0';
      }
      found = hist_find(what.s ? what.s : "", hist.n ? hist.n - 1 : (size_t)-1, 1);
      back = 1;
      failed = (found == (size_t)-1 && what.len > 0);
    }
    else if (c == 7 || c == 3) {	/* Ctrl-G, Ctrl-C: forget it */
      status = 0;
      break;
    }
    else if (c == '\r' || c == '\n') {
      status = 2;
      break;
    }
    else if (c == 27) {	/* Esc, or an arrow key: keep the line, stop searching */
      *key = read_escape();
      break;
    }
    else if (c < 32 || c == 127) break;	/* any other key: keep the line */
    else {	/* a character of the search word */
      buf_putc(&what, (char)c);
      next = hist_find(what.s, found == (size_t)-1 ? (hist.n ? hist.n - 1 : (size_t)-1) : found, back);
      if (next == (size_t)-1) {	/* not here: look on from where we are */
        next = hist_find(what.s, back ? (found == (size_t)-1 || found == 0 ? (size_t)-1 : found - 1) : found + 1, back);
      }
      if (next == (size_t)-1) failed = 1;
      else {
        found = next;
        failed = 0;
      }
    }
    search_show(what.s ? what.s : "", found == (size_t)-1 ? "" : hist.v[found], back, failed);
  }
  if (status != 0 && found != (size_t)-1) ed_set(e, hist.v[found]);
  buf_free(&what);
  os_write(1, "\r\033[K", 4);
  return status;
}

/* }================================================================== */


/* columns the prompt takes: color sequences (ESC [ ... m) take none */
static size_t prompt_width (const char *p) {
  size_t n = 0;
  while (*p != '\0') {
    if (p[0] == '\033' && p[1] == '[') {
      for (p += 2; *p != '\0' && (*p < '@' || *p > '~'); p++)
        ;
      if (*p != '\0') p++;
    }
    else {
      if (((unsigned char)*p & 0xC0) != 0x80) n++;
      p++;
    }
  }
  return n;
}


/*
** {==================================================================
** Kill ring, undo and the other editing commands
** ===================================================================
*/

/* what a key did, for kills that add up and yanks that go round */
enum { A_OTHER, A_INSERT, A_KILL, A_YANK, A_LASTARG };

#define KILL_MAX 16
static char *kills[KILL_MAX];	/* kills[0] is the newest; kept between lines */
static size_t nkills = 0;
static size_t yank_at, yank_len, yank_idx;	/* the text the last yank put in */


/* cuts from..to into the kill ring; a kill right after a kill adds to it */
static void ed_kill (Edit *e, size_t from, size_t to, int last_act) {
  size_t n = to - from;
  if (from >= to) return;
  if (last_act == A_KILL && nkills > 0) {
    int back = (to == e->pos);	/* killed backwards: it goes in front */
    size_t ol = strlen(kills[0]);
    char *s = (char *)xmalloc(ol + n + 1);
    if (back) {
      memcpy(s, e->buf + from, n);
      memcpy(s + n, kills[0], ol + 1);
    }
    else {
      memcpy(s, kills[0], ol);
      memcpy(s + ol, e->buf + from, n);
      s[ol + n] = '\0';
    }
    free(kills[0]);
    kills[0] = s;
  }
  else {
    if (nkills == KILL_MAX) free(kills[--nkills]);
    memmove(kills + 1, kills, nkills * sizeof(char *));
    kills[0] = xstrndup(e->buf + from, n);
    nkills++;
  }
  ed_delete(e, from, to);
}


static void ed_yank (Edit *e, size_t idx) {
  yank_idx = idx;
  yank_at = e->pos;
  yank_len = strlen(kills[idx]);
  ed_insert(e, kills[idx], yank_len);
}


typedef struct Undo {
  char *text;
  size_t pos;
} Undo;

#define UNDO_MAX 200
static Undo undos[UNDO_MAX];
static size_t nundo = 0;


static void undo_push (const char *text, size_t pos) {
  if (nundo == UNDO_MAX) {	/* the oldest step goes */
    free(undos[0].text);
    memmove(undos, undos + 1, (UNDO_MAX - 1) * sizeof(Undo));
    nundo--;
  }
  undos[nundo].text = xstrdup(text);
  undos[nundo].pos = pos;
  nundo++;
}


static void undo_clear (void) {
  while (nundo > 0) free(undos[--nundo].text);
}


/* Ctrl-T: the character before the cursor changes place with the one under it */
static void ed_transpose (Edit *e) {
  size_t a, b, c;
  char tmp[8];
  if (e->pos == 0) return;
  if (e->pos >= e->len) e->pos = ed_prev(e, e->len);	/* at the end: the last two */
  b = e->pos;
  a = ed_prev(e, b);
  c = ed_next(e, b);
  if (a == b || b == c) return;
  memcpy(tmp, e->buf + a, b - a);
  memmove(e->buf + a, e->buf + b, c - b);
  memcpy(e->buf + a + (c - b), tmp, b - a);
  e->pos = c;
}


static int is_wordc (char c) {
  return isalnum((unsigned char)c) || (unsigned char)c >= 0x80;
}


/* Alt-u, Alt-l, Alt-c: the word from the cursor on to upper, lower, Capital */
static void ed_case_word (Edit *e, int how) {
  size_t p = e->pos;
  int first = 1;
  while (p < e->len && !is_wordc(e->buf[p])) p++;
  for (; p < e->len && is_wordc(e->buf[p]); p++) {
    unsigned char ch = (unsigned char)e->buf[p];
    if (ch < 0x80)
      e->buf[p] = (char)((how == 'u' || (how == 'c' && first)) ? toupper(ch) : tolower(ch));
    first = 0;
  }
  e->pos = p;
}


/* the last word of a history line, as it was typed (quotes and all) */
static int last_word (const char *s, size_t *at, size_t *len) {
  size_t i = 0, ws = 0, we = 0;
  int any = 0;
  while (s[i] != '\0') {
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (s[i] == '\0') break;
    ws = i;
    while (s[i] != '\0' && s[i] != ' ' && s[i] != '\t') {
      if (s[i] == '\\' && s[i + 1] != '\0') i += 2;
      else if (s[i] == '\'' || s[i] == '"') {
        char q = s[i++];
        while (s[i] != '\0' && s[i] != q) {
          if (q == '"' && s[i] == '\\' && s[i + 1] != '\0') i++;
          i++;
        }
        if (s[i] != '\0') i++;
      }
      else i++;
    }
    we = i;
    any = 1;
  }
  *at = ws;
  *len = we - ws;
  return any;
}


/*
** Ctrl-X Ctrl-E: the line goes into $VISUAL (or $EDITOR) and what is
** saved there is run, like bash. Returns 1 when there is something to run.
*/
static int ed_external (Edit *e) {
  const char *editor = var_get("VISUAL");
  char name[64], *dir, *file, *cmd, *q, *text;
  size_t n;
  int fd, saved = sh_status;
  if (editor == NULL || *editor == '\0') editor = var_get("EDITOR");
  if (editor == NULL || *editor == '\0') {
#ifdef _WIN32
    editor = "notepad";
#else
    editor = "vi";
#endif
  }
  dir = path_tmpdir();
  sprintf(name, "mmc-edit-%ld.sh", os_getpid());
  file = path_join(dir, name);
  free(dir);
  fd = os_open(file, OS_WRITE);
  if (fd < 0) {
    free(file);
    os_write(1, "\a", 1);
    return 0;
  }
  os_write(fd, e->buf, e->len);
  os_write(fd, "\n", 1);
  os_close(fd);
  e->pos = e->len;
  ed_refresh(e);
  ed_leave(e);
  os_write(1, "\033[?2004l", 8);
  os_tty_raw(0);
  q = shell_quote(file);
  cmd = xstrcat3(editor, " ", q);	/* $EDITOR unquoted: "code --wait" is two words */
  sh_run_string(cmd, "edit-and-execute", 0);
  sh_status = saved;
  free(cmd);
  free(q);
  text = read_file(file, &n);
  os_unlink(file);
  free(file);
  os_tty_raw(1);
  os_write(1, "\033[?2004h", 8);
  if (text == NULL) text = xstrdup("");
  {	/* CRLF from a Windows editor: the \r goes */
    char *r, *w;
    for (r = w = text; *r; r++)
      if (!(r[0] == '\r' && r[1] == '\n')) *w++ = *r;
    *w = '\0';
  }
  n = strlen(text);
  while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == ' ' || text[n - 1] == '\t'))
    text[--n] = '\0';
  ed_set(e, text);
  free(text);
  return e->len > 0;
}

/* }================================================================== */


char *line_read (const char *prompt) {
  Edit e;
  size_t hpos = hist.n;	/* hist.n means "the line being typed" */
  size_t larg_hist = 0, larg_at = 0, larg_len = 0;	/* what Alt-. put in */
  char *typed = NULL;
  char *result = NULL;
  char *before = NULL;
  int done = 0, last_act = A_OTHER;
  if (!os_is_tty(0)) return read_plain(0);
  if (!os_is_tty(1) || os_tty_raw(1) != 0) {
    fd_puts(1, prompt);
    return read_plain(1);
  }
  e.cap = 256;
  e.buf = (char *)xmalloc(e.cap);
  e.buf[0] = '\0';
  e.len = e.pos = 0;
  e.prompt = prompt;
  e.pwidth = prompt_width(prompt);
  e.crow = e.erow = 0;
  undo_clear();
  os_write(1, "\033[?2004h", 8);	/* bracketed paste on */
  ed_refresh(&e);
  while (!done) {
    int c = os_tty_getbyte();
    int key = K_NONE, act = A_OTHER, undo = 0;
    size_t bpos = e.pos;
    if (c < 0) {	/* terminal went away */
      free(e.buf);
      e.buf = NULL;
      break;
    }
    free(before);
    before = xstrdup(e.buf);	/* for undo */
    if (c == 27) key = read_escape();
    else if (c == 1) key = K_HOME;	/* Ctrl-A */
    else if (c == 5) key = K_END;	/* Ctrl-E */
    else if (c == 2) key = K_LEFT;	/* Ctrl-B */
    else if (c == 6) key = K_RIGHT;	/* Ctrl-F */
    else if (c == 16) key = K_UP;	/* Ctrl-P */
    else if (c == 14) key = K_DOWN;	/* Ctrl-N */
    else if (c == 23) key = K_WDEL;	/* Ctrl-W */
    else if (c == 18) {	/* Ctrl-R: search the history */
      char *old = xstrdup(e.buf);
      int r = ed_search(&e, &key);
      if (r == 0) ed_set(&e, old);
      free(old);
      hpos = hist.n;
      if (r == 2) {
        done = 1;
        continue;
      }
    }
    else if (c == '\r' || c == '\n') {
      done = 1;
      continue;
    }
    else if (c == 3) {	/* Ctrl-C: drop the line */
      e.pos = e.len;
      ed_refresh(&e);
      os_write(1, "^C", 2);
      e.len = e.pos = 0;
      e.buf[0] = '\0';
      done = 1;
      continue;
    }
    else if (c == 4) {	/* Ctrl-D: end of input on an empty line */
      if (e.len == 0) {
        free(e.buf);
        e.buf = NULL;
        break;
      }
      key = K_DEL;
    }
    else if (c == 127 || c == 8) ed_delete(&e, ed_prev(&e, e.pos), e.pos);
    else if (c == 9) ed_complete(&e);
    else if (c == 11) {	/* Ctrl-K */
      ed_kill(&e, e.pos, e.len, last_act);
      act = A_KILL;
    }
    else if (c == 21) {	/* Ctrl-U */
      ed_kill(&e, 0, e.pos, last_act);
      act = A_KILL;
    }
    else if (c == 25) {	/* Ctrl-Y: the last thing killed comes back */
      if (nkills > 0) {
        ed_yank(&e, 0);
        act = A_YANK;
      }
    }
    else if (c == 20) ed_transpose(&e);	/* Ctrl-T */
    else if (c == 31) undo = 1;	/* Ctrl-_ */
    else if (c == 24) {	/* Ctrl-X and a second key */
      int c2 = os_tty_getbyte();
      if (c2 == 21) undo = 1;	/* Ctrl-X Ctrl-U */
      else if (c2 == 5) {	/* Ctrl-X Ctrl-E: in the editor, then run */
        if (ed_external(&e)) {
          done = 1;
          continue;
        }
      }
    }
    else if (c == 12) {	/* Ctrl-L */
      os_write(1, "\033[H\033[2J", 7);
      e.crow = e.erow = 0;
    }
    else if (c >= 32) {	/* text; collect a whole UTF-8 sequence */
      char u[4];
      int n = 1, need = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
      u[0] = (char)c;
      while (n < need && (c = os_tty_getbyte()) >= 0) u[n++] = (char)c;
      ed_insert(&e, u, (size_t)n);
      act = A_INSERT;
    }
    switch (key) {
      case K_LEFT: e.pos = ed_prev(&e, e.pos); break;
      case K_RIGHT: e.pos = ed_next(&e, e.pos); break;
      case K_HOME: e.pos = 0; break;
      case K_END: e.pos = e.len; break;
      case K_WLEFT: e.pos = ed_word_left(&e, e.pos); break;
      case K_WRIGHT: e.pos = ed_word_right(&e, e.pos); break;
      case K_DEL: ed_delete(&e, e.pos, ed_next(&e, e.pos)); break;
      case K_WDEL:
        ed_kill(&e, ed_word_left(&e, e.pos), e.pos, last_act);
        act = A_KILL;
        break;
      case K_KILLWORD:	/* Alt-d, Ctrl-Delete */
        ed_kill(&e, e.pos, ed_word_right(&e, e.pos), last_act);
        act = A_KILL;
        break;
      case K_YANKPOP:	/* Alt-y right after a yank: the kill before that one instead */
        if (last_act == A_YANK && nkills > 1) {
          ed_delete(&e, yank_at, yank_at + yank_len);
          ed_yank(&e, (yank_idx + 1) % nkills);
          act = A_YANK;
        }
        break;
      case K_UPCASE: ed_case_word(&e, 'u'); break;
      case K_DOWNCASE: ed_case_word(&e, 'l'); break;
      case K_CAPWORD: ed_case_word(&e, 'c'); break;
      case K_LASTARG: {	/* Alt-.: the last word of the line before; again: of the one before that */
        size_t i = (last_act == A_LASTARG) ? larg_hist : hist.n, at = 0, len = 0;
        int found = 0;
        while (i > 0 && !found) found = last_word(hist.v[--i], &at, &len);
        if (!found) {
          os_write(1, "\a", 1);
          if (last_act == A_LASTARG) act = A_LASTARG;
          break;
        }
        if (last_act == A_LASTARG) ed_delete(&e, larg_at, larg_at + larg_len);
        larg_hist = i;
        larg_at = e.pos;
        larg_len = len;
        ed_insert(&e, hist.v[i] + at, len);
        act = A_LASTARG;
        break;
      }
      case K_PASTE: read_paste(&e); break;
      case K_UP:
      case K_DOWN:
      case K_HISTFIRST:	/* Alt-< */
      case K_HISTLAST: {	/* Alt-> */
        size_t to = (key == K_UP) ? (hpos == 0 ? 0 : hpos - 1) :
                    (key == K_DOWN) ? (hpos == hist.n ? hist.n : hpos + 1) :
                    (key == K_HISTFIRST) ? 0 : hist.n;
        if (to == hpos) break;
        if (hpos == hist.n) {	/* remember what was being typed */
          free(typed);
          typed = xstrdup(e.buf);
        }
        hpos = to;
        ed_set(&e, hpos == hist.n ? (typed ? typed : "") : hist.v[hpos]);
        break;
      }
      default: break;
    }
    if (undo) {
      if (nundo > 0) {
        nundo--;
        ed_set(&e, undos[nundo].text);
        e.pos = undos[nundo].pos <= e.len ? undos[nundo].pos : e.len;
        free(undos[nundo].text);
      }
      else os_write(1, "\a", 1);
    }
    /* typing is a step a word; every other change is a step of its own */
    else if (strcmp(before, e.buf) != 0 &&
             !(act == A_INSERT && last_act == A_INSERT && bpos > 0 && before[bpos - 1] != ' '))
      undo_push(before, bpos);
    last_act = act;
    ed_refresh(&e);
  }
  free(before);
  if (e.buf != NULL) {
    if (e.len > 0) {	/* show the whole line before moving on */
      e.pos = e.len;
      ed_refresh(&e);
    }
    result = e.buf;
  }
  os_write(1, "\033[?2004l\r\n", 10);	/* bracketed paste off */
  os_tty_raw(0);
  free(typed);
  undo_clear();
  return result;
}


/* for select: bytes up to the delimiter; NULL at the end of input */
char *line_read_raw (int fd, int delim, int nchars, int silent, int timeout_ms,
                     int *timed_out) {
  Buf b;
  int any = 0;
  (void)silent;
  *timed_out = 0;
  buf_init(&b);
  for (;;) {
    unsigned char c;
    if (nchars >= 0 && (int)b.len >= nchars) break;
    if (timeout_ms >= 0 && os_wait_readable(fd, timeout_ms) == 0) {
      *timed_out = 1;
      break;
    }
    if (os_read(fd, &c, 1) != 1) break;
    any = 1;
    if (c == (unsigned char)delim) break;
    buf_putc(&b, (char)c);
  }
  if (!any) {
    buf_free(&b);
    return NULL;
  }
  if (b.len > 0 && b.s[b.len - 1] == '\r') b.s[--b.len] = '\0';
  if (b.s == NULL) return xstrdup("");
  return buf_take(&b);
}
