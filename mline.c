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


const Vec *line_hist (void) {
  return &hist;
}


static void hist_push (const char *s) {
  if (hist.n > 0 && strcmp(hist.v[hist.n - 1], s) == 0) return;
  if (hist.n >= MMC_HISTORY_MAX) {
    free(hist.v[0]);
    memmove(hist.v, hist.v + 1, hist.n * sizeof(char *));	/* with NULL */
    hist.n--;
  }
  vec_push(&hist, xstrdup(s));
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
  if (lines > 2 * MMC_HISTORY_MAX) {	/* file grew too much: rewrite it */
    int fd = os_open(hist_file, OS_WRITE);
    size_t i;
    if (fd < 0) return;
    for (i = 0; i < hist.n; i++) fd_printf(fd, "%s\n", hist.v[i]);
    os_close(fd);
  }
}


void line_hist_add (const char *s) {
  size_t before = hist.n;
  const char *last = hist.n ? hist.v[hist.n - 1] : NULL;
  if (s[0] == '\0' || s[0] == ' ' || (last && strcmp(last, s) == 0)) return;
  hist_push(s);
  if (hist_file != NULL && (hist.n != before || before >= MMC_HISTORY_MAX)) {
    int fd = os_open(hist_file, OS_APPEND);
    if (fd >= 0) {
      fd_printf(fd, "%s\n", s);
      os_close(fd);
    }
  }
}


void line_hist_clear (void) {
  vec_free(&hist);
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


/* redraws the line; long lines scroll sideways */
static void ed_refresh (const Edit *e) {
  Buf o;
  char move[32];
  int cols = os_term_cols();
  size_t avail, start = 0, end, shown = 0;
  size_t ccol = utf8_count(e->buf, e->pos);
  if (cols < 20) cols = 80;
  avail = (size_t)cols - 1 - (e->pwidth < (size_t)cols - 8 ? e->pwidth : 0);
  while (ccol > avail) {
    start = ed_next(e, start);
    ccol--;
  }
  for (end = start; end < e->len && shown < avail; shown++)
    end = ed_next(e, end);
  buf_init(&o);
  buf_putc(&o, '\r');
  buf_puts(&o, e->prompt);
  buf_putn(&o, e->buf + start, end - start);
  buf_puts(&o, "\033[K\r");
  if (e->pwidth + ccol > 0) {
    sprintf(move, "\033[%luC", (unsigned long)(e->pwidth + ccol));
    buf_puts(&o, move);
  }
  os_write(1, o.s, o.len);
  buf_free(&o);
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
  char *path = os_getenv("PATH");
  vec_init(&names);
  vec_init(&dirs);
  builtin_names(&names);
  alias_names(&names);
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
    char *home = os_getenv("HOME");
    lookup = xstrcat3(home ? home : "", dirpart + 1, "");
    free(home);
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


static void show_candidates (const Vec *v) {
  Buf o;
  size_t i, width = 0, cols, col = 0;
  size_t limit = v->n > 120 ? 120 : v->n;
  buf_init(&o);
  buf_puts(&o, "\r\n");
  for (i = 0; i < limit; i++) {
    size_t w = utf8_count(path_basename(v->v[i]), strlen(path_basename(v->v[i])));
    if (w > width) width = w;
  }
  width += 3;
  cols = (size_t)os_term_cols() / width;
  if (cols < 1) cols = 1;
  for (i = 0; i < limit; i++) {
    const char *s = v->v[i];
    size_t n = strlen(s), w;
    if (n > 1 && s[n - 1] == '/') {	/* basename of "dir/" */
      const char *p = s + n - 1;
      while (p > s && p[-1] != '/') p--;
      s = p;
    }
    else s = path_basename(s);
    w = utf8_count(s, strlen(s));
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


static void ed_complete (Edit *e) {
  Vec cands;
  Buf word, esc;
  size_t ws = e->pos, i, keep;
  int command;
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
  if (command) complete_commands(word.s, &cands);
  if (cands.n == 0) {
    command = 0;
    complete_files(word.s, &cands);
  }
  if (cands.n == 0) {
    os_write(1, "\a", 1);
    buf_free(&word);
    vec_free(&cands);
    return;
  }
  vec_sort(&cands);
  keep = common_prefix(&cands);
  if (cands.n > 1 && keep <= strlen(word.s)) {
    show_candidates(&cands);
    buf_free(&word);
    vec_free(&cands);
    return;
  }
  buf_init(&esc);
  for (i = 0; i < keep; i++) {
    char c = cands.v[0][i];
    if (strchr(" \t\"'$;&|<>#*?[()", c) != NULL) buf_putc(&esc, '\\');
    buf_putc(&esc, c);
  }
  if (cands.n == 1 && (keep == 0 || cands.v[0][keep - 1] != '/'))
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
       K_WLEFT, K_WRIGHT, K_WDEL };

static int read_escape (void) {
  char seq[16];
  int n = 0, c = os_tty_getbyte();
  if (c == 'b') return K_WLEFT;	/* Alt-b, Alt-f, Alt-Backspace */
  if (c == 'f') return K_WRIGHT;
  if (c == 127 || c == 8) return K_WDEL;
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
        case 3: return K_DEL;
      }
  }
  return K_NONE;
}


char *line_read (const char *prompt) {
  Edit e;
  size_t hpos = hist.n;	/* hist.n means "the line being typed" */
  char *typed = NULL;
  char *result = NULL;
  int done = 0;
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
  e.pwidth = utf8_count(prompt, strlen(prompt));
  ed_refresh(&e);
  while (!done) {
    int c = os_tty_getbyte();
    int key = K_NONE;
    if (c < 0) {	/* terminal went away */
      free(e.buf);
      e.buf = NULL;
      break;
    }
    if (c == 27) key = read_escape();
    else if (c == 1) key = K_HOME;	/* Ctrl-A */
    else if (c == 5) key = K_END;	/* Ctrl-E */
    else if (c == 2) key = K_LEFT;	/* Ctrl-B */
    else if (c == 6) key = K_RIGHT;	/* Ctrl-F */
    else if (c == 16) key = K_UP;	/* Ctrl-P */
    else if (c == 14) key = K_DOWN;	/* Ctrl-N */
    else if (c == 23) key = K_WDEL;	/* Ctrl-W */
    else if (c == '\r' || c == '\n') {
      done = 1;
      continue;
    }
    else if (c == 3) {	/* Ctrl-C: drop the line */
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
    else if (c == 11) ed_delete(&e, e.pos, e.len);	/* Ctrl-K */
    else if (c == 21) ed_delete(&e, 0, e.pos);	/* Ctrl-U */
    else if (c == 12) os_write(1, "\033[H\033[2J", 7);	/* Ctrl-L */
    else if (c >= 32) {	/* text; collect a whole UTF-8 sequence */
      char u[4];
      int n = 1, need = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
      u[0] = (char)c;
      while (n < need && (c = os_tty_getbyte()) >= 0) u[n++] = (char)c;
      ed_insert(&e, u, (size_t)n);
    }
    switch (key) {
      case K_LEFT: e.pos = ed_prev(&e, e.pos); break;
      case K_RIGHT: e.pos = ed_next(&e, e.pos); break;
      case K_HOME: e.pos = 0; break;
      case K_END: e.pos = e.len; break;
      case K_WLEFT: e.pos = ed_word_left(&e, e.pos); break;
      case K_WRIGHT: e.pos = ed_word_right(&e, e.pos); break;
      case K_DEL: ed_delete(&e, e.pos, ed_next(&e, e.pos)); break;
      case K_WDEL: ed_delete(&e, ed_word_left(&e, e.pos), e.pos); break;
      case K_UP:
      case K_DOWN:
        if (key == K_UP && hpos == 0) break;
        if (key == K_DOWN && hpos == hist.n) break;
        if (hpos == hist.n) {	/* remember what was being typed */
          free(typed);
          typed = xstrdup(e.buf);
        }
        hpos += (key == K_UP) ? (size_t)-1 : 1;
        ed_set(&e, hpos == hist.n ? (typed ? typed : "") : hist.v[hpos]);
        break;
      default: break;
    }
    ed_refresh(&e);
  }
  if (e.buf != NULL) {
    if (e.len > 0) {	/* show the whole line before moving on */
      e.pos = e.len;
      ed_refresh(&e);
    }
    result = e.buf;
  }
  os_write(1, "\r\n", 2);
  os_tty_raw(0);
  free(typed);
  return result;
}
