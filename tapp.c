/*
** tapp.c - behaviour of mmc-term
**
** Everything between the backend (keys, mouse, window) and the core
** (grid, parser, renderer): tabs, key mapping, selection and clipboard,
** scrolling, zoom, themes, the popup menu, the configuration.
*/

#include "mterm.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


enum { M_COPY = 1, M_PASTE, M_SELECT_ALL, M_BIGGER, M_SMALLER,
       M_MORE_CLEAR, M_LESS_CLEAR, M_NEW_TAB, M_RENAME_TAB, M_CLOSE_TAB,
       M_NEW_WINDOW, M_ABOUT, M_THEME /* + theme number, keep last */ };

/* one shell with its own screen; the window shows one tab at a time */
typedef struct Tab {
  Grid *g;
  Vt vt;
  Pty *pty;	/* NULL until started */
  Theme theme;	/* the window's, plus what the program changed (OSC 4, 10 ..) */
  char label[64];	/* the name the user gave it, "" = none: wins over title */
  char title[256];	/* what the program set, "" = none */
  char name[64];	/* the program: the tab says this without a title */
  char cwd[512];	/* what the shell reported with OSC 7 */
  int hold;	/* --hold: stays open when the program ends */
  int done, news;	/* the program ended; output came while hidden */
} Tab;

static struct {
  AppArgs args;
  Config cfg;
  Theme theme;
  char *exe, *exe_dir, *root, *conf;
  Tab *tabs[TAB_MAX];
  int ntabs, cur;
  Grid *g;	/* the grid of the tab on screen */
  Frame frame;
  Scene scene;
  Menu menu;
  float scale;
  int zoom;	/* points added to the configured font size */
  int pad, strip;
  int custom, head;	/* our own title bar and its height (0: none) */
  int bar;	/* height of the tab bar (0: one tab, no bar) */
  int hot_tab, hot_close, pressed_close;
  unsigned tab_click_at;	/* a double click on a tab renames it */
  int tab_click;
  int renaming;	/* the tab on screen is being renamed; A.edit is the text */
  char edit[64];
  char edit_pill[96];	/* no tab bar: the name is typed in a pill */
  int hot_button, pressed_button;
  int win_w, win_h;
  int dirty, focused, fullscreen, resizing;
  int blink_on;
  unsigned now, blink_at, bar_until, pill_until;
  unsigned quiet_until;	/* hidden tabs redraw after a resize: not news */
  char pill[32];
  int bar_hover, bar_drag, bar_grab;
  /* selection: anchor and moving end, y as in grid_line() */
  int selecting, sel_mode, has_sel;
  int ax, ay, bx, by;
  unsigned click_at;
  int clicks, click_x, click_y;
  int mouse_x, mouse_y;
  int mouse_held;	/* the button a program is being told about (0: none) */
  int mouse_cx, mouse_cy;	/* the cell the last report named */
  int exit_code;	/* of the program that ended last */
} A;

#define CUR	(A.tabs[A.cur])

/* where the terminal starts: below the title bar, the tabs and the strip */
#define TOP	(A.head + A.bar + A.strip)


static void build_scene (void);
static void apply_bar (void);
static void new_tab (void);
static void close_tab (int i);
static void switch_tab (int i);
static const char *window_title (void);
static void rename_start (void);
static void rename_end (int keep);
static void rename_add (const char *utf8);


static void touch (void) {
  A.dirty = 1;
  win_redraw();
}


int app_exit_code (void) {
  return A.exit_code;
}


/*
** {==================================================================
** Geometry
** ===================================================================
*/

static int bar_height (void) {
  int h;
  if (A.ntabs < 2) return 0;
  h = (int)(30.0f * A.scale + 0.5f);
  return h < font_cell_h() + 10 ? font_cell_h() + 10 : h;
}


static void apply_font (void) {
  float px = (float)(A.cfg.font_size + A.zoom) * A.scale * 96.0f / 72.0f;
  font_set_px(px);
  A.bar = bar_height();
  A.pad = (int)((float)A.cfg.padding * A.scale + 0.5f);
  A.strip = (int)(2.0f * A.scale + 0.5f);	/* the thin blue to green line */
  if (A.strip < 1) A.strip = 1;
  A.head = 0;
  if (A.custom && !A.fullscreen) {
    A.head = (int)(30.0f * A.scale + 0.5f);
    if (A.head < font_cell_h() + 8) A.head = font_cell_h() + 8;
  }
}


static void size_for (int cols, int rows, int *w, int *h) {
  *w = 2 * A.pad + cols * font_cell_w();
  *h = TOP + 2 * A.pad + rows * font_cell_h();
}


void app_initial_size (int *w, int *h) {
  size_for(A.cfg.cols, A.cfg.rows, w, h);
}


void app_snap_size (int *w, int *h) {
  int cols = (*w - 2 * A.pad + font_cell_w() / 2) / font_cell_w();
  int rows = (*h - TOP - 2 * A.pad + font_cell_h() / 2) / font_cell_h();
  size_for(cols < 20 ? 20 : cols, rows < 4 ? 4 : rows, w, h);
}


void app_on_resize (int w, int h) {
  int cols = (w - 2 * A.pad) / font_cell_w();
  int rows = (h - TOP - 2 * A.pad) / font_cell_h();
  if (w <= 0 || h <= 0) return;
  if (cols < 2) cols = 2;
  if (rows < 1) rows = 1;
  A.win_w = w;
  A.win_h = h;
  frame_resize(&A.frame, w, h);
  if (A.g != NULL && (cols != A.g->cols || rows != A.g->rows)) {
    int i;
    for (i = 0; i < A.ntabs; i++) {	/* hidden tabs too: all have one size */
      grid_resize(A.tabs[i]->g, cols, rows);
      if (A.tabs[i]->pty != NULL) pty_resize(A.tabs[i]->pty, cols, rows);
    }
    A.quiet_until = A.now + 1000;
    A.has_sel = 0;
    if (A.resizing) {
      sprintf(A.pill, "%d x %d", cols, rows);
      A.pill_until = A.now + 900;
    }
  }
  A.menu.open = 0;
  touch();
}


void app_resizing (int on) {
  A.resizing = on;
  if (!on) A.pill_until = A.now + 500;
}


/* keeps the number of columns and rows and changes the window instead */
static void refit_window (void) {
  int w, h;
  if (A.fullscreen) {
    app_on_resize(A.win_w, A.win_h);
    return;
  }
  size_for(A.g->cols, A.g->rows, &w, &h);
  win_set_size(w, h);
}


/* the tab bar came or went: the window grows or shrinks by it, so the
** shells keep their rows; a maximized window has to give rows instead */
static void apply_bar (void) {
  int bar = bar_height();
  if (bar != A.bar) {
    A.bar = bar;
    if (!A.fullscreen && win_is_maximized()) app_on_resize(A.win_w, A.win_h);
    else refit_window();
  }
  touch();
}


void app_on_dpi (float scale) {
  if (scale < 0.5f) scale = 1.0f;
  A.scale = scale;
  apply_font();
  touch();
}


static void zoom (int delta) {
  int z = (delta == 0) ? 0 : A.zoom + delta;
  if (A.cfg.font_size + z < 6 || A.cfg.font_size + z > 72) return;
  A.zoom = z;
  apply_font();
  refit_window();
  touch();
}

/* }================================================================== */


/*
** {==================================================================
** Theme, clipboard, windows
** ===================================================================
*/

/* 'a' of 255 parts of color y over color x */
static uint32_t blend (uint32_t x, uint32_t y, int a) {
  uint32_t r = (((x >> 16) & 0xFF) * (uint32_t)(255 - a) + ((y >> 16) & 0xFF) * (uint32_t)a) / 255;
  uint32_t g = (((x >> 8) & 0xFF) * (uint32_t)(255 - a) + ((y >> 8) & 0xFF) * (uint32_t)a) / 255;
  uint32_t b = ((x & 0xFF) * (uint32_t)(255 - a) + (y & 0xFF) * (uint32_t)a) / 255;
  return (r << 16) | (g << 8) | b;
}


static void set_theme (const char *name, int save) {
  int i;
  strncpy(A.cfg.theme, name, sizeof(A.cfg.theme) - 1);
  theme_apply(&A.theme, theme_find(name), &A.cfg);
  for (i = 0; i < A.ntabs; i++) {
    A.tabs[i]->theme = A.theme;
    A.tabs[i]->g->all_dirty = 1;
  }
  /* a quiet frame: the header color with only a hint of the logo blue */
  win_set_chrome(A.theme.bg, blend(A.theme.ui, A.theme.accent1, A.theme.dark ? 70 : 110),
                 A.theme.fg, A.theme.dark);
  if (save && A.conf != NULL) config_set_key(A.conf, "theme", A.cfg.theme);
  touch();
}


static void next_theme (void) {
  set_theme(theme_next(A.cfg.theme)->name, 1);
}


static void sel_range (int *x0, int *y0, int *x1, int *y1) {
  int swap = (A.by < A.ay) || (A.by == A.ay && A.bx < A.ax);
  *x0 = swap ? A.bx : A.ax;
  *y0 = swap ? A.by : A.ay;
  *x1 = swap ? A.ax : A.bx;
  *y1 = swap ? A.ay : A.by;
}


static void copy_selection (void) {
  int x0, y0, x1, y1;
  char *text;
  if (!A.has_sel) return;
  sel_range(&x0, &y0, &x1, &y1);
  text = grid_text(A.g, x0, y0, x1, y1);
  if (text[0] != '\0') win_set_clipboard(text);
  free(text);
}


static void select_all (void) {
  A.ax = 0;
  A.ay = -A.g->sb_len;
  A.bx = A.g->cols - 1;
  A.by = A.g->rows - 1;
  A.has_sel = 1;
  touch();
}


static void send (const char *s, size_t n) {
  if (CUR->pty != NULL) pty_write(CUR->pty, s, n);
  if (A.g->view != 0) grid_set_view(A.g, 0);	/* typing shows the live screen */
  A.blink_on = 1;
  A.blink_at = A.now;
  touch();
}


static void send_str (const char *s) {
  send(s, strlen(s));
}


void app_on_paste (const char *utf8) {
  Buf b;
  const char *p;
  if (utf8 == NULL || utf8[0] == '\0') return;
  if (A.renaming) {
    rename_add(utf8);
    return;
  }
  buf_init(&b);
  if (A.g->bracketed) buf_puts(&b, "\033[200~");
  for (p = utf8; *p; p++) {
    if (*p == '\r' && p[1] == '\n') continue;	/* CRLF -> CR */
    if (*p == '\n') buf_putc(&b, '\r');
    else if (*p == '\033') continue;	/* pasted text must not send commands */
    else buf_putc(&b, *p);
  }
  if (A.g->bracketed) buf_puts(&b, "\033[201~");
  send(b.s, b.len);
  buf_free(&b);
}


/* goes where the shell of the tab on screen said it is (OSC 7), so what
** starts next starts there; returns the folder to come back to, or NULL */
static char *enter_cwd (void) {
  char *back;
  if (CUR->cwd[0] == '\0') return NULL;
  back = os_getcwd();
  if (os_chdir(CUR->cwd) != 0) {
    free(back);
    return NULL;
  }
  return back;
}


static void leave_cwd (char *back) {
  if (back == NULL) return;
  os_chdir(back);
  free(back);
}


static void new_window (void) {
  char *argv[2];
  static const int fds[3] = {0, 1, 2};
  OsProc proc;
  long pid;
  char *back = enter_cwd();
  argv[0] = A.exe;
  argv[1] = NULL;
  if (os_spawn(A.exe, argv, NULL, fds, 3, &proc, &pid) == 0) os_detach(proc);
  leave_cwd(back);
}


static void about (void) {
  char text[512];
  sprintf(text, "%s %s\nthe terminal window of the mmc shell\nby %s\n\n"
          "font: %.100s   theme: %.30s\n%d x %d cells, %d lines of history",
          TERM_NAME, MMC_VERSION, MMC_AUTHOR, font_name(), A.theme.name,
          A.g->cols, A.g->rows, A.g->sb_len);
  win_message("About " TERM_NAME, text);
}

/* see-through window: 30 .. 100 percent, remembered in the config file */
static void change_opacity (int delta) {
  char value[16];
  int o = A.cfg.opacity + delta;
  if (o > 100) o = 100;
  if (o < 30) o = 30;
  A.cfg.opacity = o;
  win_set_opacity(o);
  sprintf(value, "%d", o);
  if (A.conf != NULL) config_set_key(A.conf, "opacity", value);
  sprintf(A.pill, "opacity %d%%", o);
  A.pill_until = A.now + 900;
  touch();
}

/* }================================================================== */


/*
** {==================================================================
** Popup menu
** ===================================================================
*/

static void menu_add (const char *label, const char *hint, int id) {
  Menu *m = &A.menu;
  if (m->n >= MENU_MAX) return;
  m->label[m->n] = label;
  m->hint[m->n] = hint;
  m->id[m->n] = id;
  m->n++;
}


static void menu_open (int x, int y) {
  static char titles[8][40];
  Menu *m = &A.menu;
  const Theme *cur = theme_find(A.cfg.theme), *next = theme_next(A.cfg.theme), *t;
  int i, widest = 0, row = font_cell_h() + 10;
  memset(m, 0, sizeof(*m));
  menu_add("Copy", "Ctrl+Shift+C", M_COPY);
  menu_add("Paste", "Ctrl+Shift+V", M_PASTE);
  menu_add("Select all", NULL, M_SELECT_ALL);
  menu_add(NULL, NULL, 0);
  menu_add("New tab", "Ctrl+Shift+T", M_NEW_TAB);
  menu_add("Rename tab", "double click", M_RENAME_TAB);
  menu_add("Close tab", "Ctrl+Shift+W", M_CLOSE_TAB);
  menu_add("New window", "Ctrl+Shift+N", M_NEW_WINDOW);
  menu_add(NULL, NULL, 0);
  for (i = 0; i < 8 && (t = theme_at(i)) != NULL; i++) {	/* the current one ticked */
    sprintf(titles[i], "%.20s theme", t->title);
    menu_add(titles[i], t == cur ? "\xE2\x9C\x93" : t == next ? "Ctrl+Shift+L" : NULL,
             M_THEME + i);
  }
  menu_add(NULL, NULL, 0);
  menu_add("Bigger text", "Ctrl +", M_BIGGER);
  menu_add("Smaller text", "Ctrl -", M_SMALLER);
  menu_add("More transparent", "Ctrl+Shift+wheel", M_MORE_CLEAR);
  menu_add("Less transparent", NULL, M_LESS_CLEAR);
  menu_add(NULL, NULL, 0);
  menu_add("About " TERM_NAME, NULL, M_ABOUT);
  m->h = 12;
  for (i = 0; i < m->n; i++) {
    int w = m->label[i] ? draw_text_width(m->label[i]) : 0;
    if (m->label[i] && m->hint[i]) w += draw_text_width(m->hint[i]) + 3 * font_cell_w();
    if (w > widest) widest = w;
    m->h += m->label[i] ? row : 9;
  }
  m->w = widest + 32;
  m->x = (x + m->w + 8 > A.win_w) ? A.win_w - m->w - 8 : x;
  m->y = (y + m->h + 8 > A.win_h) ? A.win_h - m->h - 8 : y;
  if (m->x < 4) m->x = 4;
  if (m->y < 4) m->y = 4;
  m->hot = -1;
  m->open = 1;
  touch();
}


static int menu_hit (int x, int y) {
  const Menu *m = &A.menu;
  int i, top = m->y + 6, row = font_cell_h() + 10;
  if (x < m->x || x >= m->x + m->w) return -1;
  for (i = 0; i < m->n; i++) {
    int h = m->label[i] ? row : 9;
    if (y >= top && y < top + h) return m->label[i] ? i : -1;
    top += h;
  }
  return -1;
}


static void menu_do (int id) {
  switch (id) {
    case M_COPY: copy_selection(); break;
    case M_PASTE: win_request_paste(); break;
    case M_SELECT_ALL: select_all(); break;
    case M_BIGGER: zoom(1); break;
    case M_SMALLER: zoom(-1); break;
    case M_MORE_CLEAR: change_opacity(-5); break;
    case M_LESS_CLEAR: change_opacity(5); break;
    case M_NEW_TAB: new_tab(); break;
    case M_RENAME_TAB: rename_start(); break;
    case M_CLOSE_TAB: close_tab(A.cur); break;
    case M_NEW_WINDOW: new_window(); break;
    case M_ABOUT: about(); break;
    default:
      if (id >= M_THEME && theme_at(id - M_THEME) != NULL)
        set_theme(theme_at(id - M_THEME)->name, 1);
      break;
  }
}

/* }================================================================== */


/*
** {==================================================================
** Renaming the tab on screen: type, Enter keeps, Esc drops; an empty
** name gives the tab back the title its program sets
** ===================================================================
*/

static void rename_start (void) {
  const Tab *t = CUR;
  A.menu.open = 0;
  A.renaming = 1;
  A.edit[0] = '\0';	/* start from what the tab says now */
  rename_add(t->label[0] ? t->label : t->title[0] ? t->title : t->name);
  touch();
}


static void rename_end (int keep) {
  char *p = A.edit, *e;
  if (!A.renaming) return;
  A.renaming = 0;
  if (keep) {
    while (*p == ' ') p++;	/* no spaces around it */
    e = p + strlen(p);
    while (e > p && e[-1] == ' ') *--e = '\0';
    strcpy(CUR->label, p);
    win_set_title(window_title());
  }
  touch();
}


/* adds typed or pasted text; stops at a line end, never cuts a character */
static void rename_add (const char *utf8) {
  size_t have = strlen(A.edit);
  for (; *utf8 != '\0'; utf8++) {
    unsigned char c = (unsigned char)*utf8;
    size_t n = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
    if (c == '\r' || c == '\n') break;
    if (c < 0x20 || c == 0x7F) continue;
    if (have + n >= sizeof(A.edit)) break;
    memcpy(A.edit + have, utf8, n);
    have += n;
    utf8 += n - 1;
  }
  A.edit[have] = '\0';
  touch();
}


static void rename_key (int key, int mods, uint32_t cp) {
  if (key == TK_ENTER) rename_end(1);
  else if (key == TK_ESCAPE) rename_end(0);
  else if (key == TK_BACKSPACE) {
    size_t n = strlen(A.edit);
    while (n > 0 && ((unsigned char)A.edit[n - 1] & 0xC0) == 0x80) n--;	/* UTF-8 tail */
    if (n > 0) n--;
    if (mods & TM_CTRL) n = 0;	/* Ctrl+Backspace: all of it */
    A.edit[n] = '\0';
    touch();
  }
  else if (key == TK_CHAR && (mods & TM_CTRL) && (cp == 'V' || cp == 'v'))
    win_request_paste();
}

/* }================================================================== */


/*
** {==================================================================
** Keyboard
** ===================================================================
*/

static void scroll_view (int lines) {
  grid_set_view(A.g, A.g->view + lines);
  A.bar_until = A.now + 1200;
  touch();
}


/* xterm style: ESC [ 1 ; mods X   or   ESC [ n ; mods ~ */
static void send_csi (int mods, int number, char final) {
  char buf[32];
  int m = 1 + ((mods & TM_SHIFT) ? 1 : 0) + ((mods & TM_ALT) ? 2 : 0) +
          ((mods & TM_CTRL) ? 4 : 0);
  if (final == '~') {
    if (m > 1) sprintf(buf, "\033[%d;%d~", number, m);
    else sprintf(buf, "\033[%d~", number);
  }
  else if (m > 1) sprintf(buf, "\033[1;%d%c", m, final);
  else sprintf(buf, "\033%c%c", (A.g->app_cursor || number) ? 'O' : '[', final);
  send_str(buf);
}


static int shortcut (int key, int mods, uint32_t cp) {
  int cs = (mods & TM_CTRL) && (mods & TM_SHIFT);
  if (key == TK_CHAR) {
    if (cp >= 'a' && cp <= 'z') cp -= 32;
    if (cs && cp == 'C') copy_selection();
    else if (cs && cp == 'V') win_request_paste();
    else if (cs && cp == 'N') new_window();
    else if (cs && cp == 'T') new_tab();
    else if (cs && cp == 'W') close_tab(A.cur);
    else if (cs && cp == 'L') next_theme();
    else if (cs && cp == 'A') select_all();
    else if ((mods & TM_CTRL) && (cp == '=' || cp == '+')) zoom(1);
    else if ((mods & TM_CTRL) && cp == '-') zoom(-1);
    else if ((mods & TM_CTRL) && cp == '0') zoom(0);
    else if ((mods & TM_CTRL) && cp == ' ') send("\0", 1);
    else return 0;
    return 1;
  }
  if (key == TK_INSERT && (mods & TM_CTRL)) copy_selection();
  else if (key == TK_INSERT && (mods & TM_SHIFT)) win_request_paste();
  else if (key == TK_PGUP && (mods & TM_SHIFT)) scroll_view(A.g->rows - 1);
  else if (key == TK_PGDN && (mods & TM_SHIFT)) scroll_view(-(A.g->rows - 1));
  else if (key == TK_TAB && (mods & TM_CTRL) && A.ntabs > 1)	/* next, previous */
    switch_tab((A.cur + ((mods & TM_SHIFT) ? A.ntabs - 1 : 1)) % A.ntabs);
  else if (key == TK_HOME && cs) scroll_view(A.g->sb_len);
  else if (key == TK_END && cs) scroll_view(-A.g->sb_len);
  else if (key == TK_F1 + 10 || (key == TK_ENTER && (mods & TM_ALT))) {
    A.fullscreen = !A.fullscreen;
    apply_font();	/* the title bar hides in full screen */
    win_set_fullscreen(A.fullscreen);
  }
  else return 0;
  return 1;
}


int app_on_key (int key, int mods, uint32_t cp) {
  static const int fn_number[] = {11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 23, 24};
  if (A.menu.open) {
    A.menu.open = 0;
    touch();
    if (key == TK_ESCAPE) return 1;
  }
  if (A.renaming) {	/* every key belongs to the name, none to the shell */
    rename_key(key, mods, cp);
    return 1;
  }
  if (shortcut(key, mods, cp)) return 1;
  switch (key) {
    case TK_UP: send_csi(mods, 0, 'A'); break;
    case TK_DOWN: send_csi(mods, 0, 'B'); break;
    case TK_RIGHT: send_csi(mods, 0, 'C'); break;
    case TK_LEFT: send_csi(mods, 0, 'D'); break;
    case TK_HOME: send_csi(mods, 0, 'H'); break;
    case TK_END: send_csi(mods, 0, 'F'); break;
    case TK_INSERT: send_csi(mods, 2, '~'); break;
    case TK_DELETE: send_csi(mods, 3, '~'); break;
    case TK_PGUP: send_csi(mods, 5, '~'); break;
    case TK_PGDN: send_csi(mods, 6, '~'); break;
    case TK_ENTER: send_str((mods & TM_ALT) ? "\033\r" : "\r"); break;
    case TK_TAB: send_str((mods & TM_SHIFT) ? "\033[Z" : "\t"); break;
    case TK_BACKSPACE:
      send_str((mods & (TM_ALT | TM_CTRL)) ? "\033\177" : "\177");
      break;
    case TK_ESCAPE: send_str("\033"); break;
    default:
      if (key >= TK_F1 && key < TK_F1 + 12) {
        int n = key - TK_F1;
        if (n < 4) send_csi(mods, 1, (char)('P' + n));	/* SS3 P..S */
        else send_csi(mods, fn_number[n], '~');
        break;
      }
      return 0;
  }
  return 1;
}


void app_on_text (const char *utf8, int mods) {
  if (utf8 == NULL || utf8[0] == '\0') return;
  if (A.menu.open) A.menu.open = 0;
  if (A.renaming) {
    if (!(mods & TM_ALT)) rename_add(utf8);
    return;
  }
  if (mods & TM_ALT) send_str("\033");
  send_str(utf8);
}

/* }================================================================== */


/*
** {==================================================================
** Our own title bar: hide, zoom, close
** ===================================================================
*/

static int button_at (int x, int y) {
  int i, bx, by, bw, bh;
  if (A.head == 0 || y >= A.head) return -1;
  build_scene();
  for (i = 0; i < 3; i++) {
    draw_button_rect(&A.frame, &A.scene, i, &bx, &by, &bw, &bh);
    if (x >= bx - 4 && x < bx + bw + 4) return i;
  }
  return -1;
}


/* the tab (A.ntabs: the + button) at this point, or -1 */
static int tab_at (int x, int y, int *on_close) {
  int i, tx, ty, tw, th;
  *on_close = 0;
  if (A.bar == 0 || y < A.head || y >= A.head + A.bar) return -1;
  build_scene();
  for (i = 0; i <= A.ntabs; i++) {
    draw_tab_rect(&A.frame, &A.scene, i, &tx, &ty, &tw, &th);
    if (x < tx || x >= tx + tw) continue;
    if (i < A.ntabs) {
      draw_tab_close_rect(&A.frame, &A.scene, i, &tx, &ty, &tw, &th);
      *on_close = x >= tx - 3 && x < tx + tw + 3 && y >= ty - 3 && y < ty + th + 3;
    }
    return i;
  }
  return -1;
}


int app_hit_test (int x, int y) {
  int close;
  if (A.menu.open && x >= A.menu.x && x < A.menu.x + A.menu.w &&
      y >= A.menu.y && y < A.menu.y + A.menu.h)
    return HIT_UI;
  if (A.bar > 0 && y >= A.head && y < A.head + A.bar) {
    if (tab_at(x, y, &close) >= 0) return HIT_UI;
    return A.head > 0 ? HIT_CAPTION : HIT_UI;	/* empty bar moves the window */
  }
  if (y >= TOP && x >= A.win_w - 14 && A.g->sb_len > 0 && !A.g->alt)
    return HIT_UI;	/* the scrollbar */
  if (A.head == 0 || y >= A.head || y < 0) return HIT_CLIENT;
  return button_at(x, y) >= 0 ? HIT_BUTTON : HIT_CAPTION;
}


/* returns 1 if the mouse event belonged to the title bar */
static int header_mouse (int type, int button, int x, int y) {
  int hit = button_at(x, y);
  if (hit != A.hot_button) {
    A.hot_button = hit;
    touch();
  }
  if (A.head == 0 || (y >= A.head && A.pressed_button < 0)) return 0;
  if (type == TMS_DOWN && button == 1) A.pressed_button = hit;
  else if (type == TMS_UP && button == 1) {
    int pressed = A.pressed_button;
    A.pressed_button = -1;
    if (pressed >= 0 && pressed == hit) {
      if (hit == 0) win_minimize();
      else if (hit == 1) win_toggle_maximize();
      else win_close();
    }
  }
  else if (type == TMS_DOWN && button == 3) menu_open(x, y);
  return 1;
}


/* returns 1 if the mouse event belonged to the tab bar: a click shows a
** tab, on its x (or a middle click) closes it, + opens a new one */
static int tabbar_mouse (int type, int button, int x, int y) {
  int close, hit;
  if (A.bar == 0 || A.selecting || A.bar_drag) return 0;
  hit = tab_at(x, y, &close);
  if (hit != A.hot_tab || close != A.hot_close) {
    A.hot_tab = hit;
    A.hot_close = close;
    touch();
  }
  if (type == TMS_UP && button == 1 && A.pressed_close >= 0) {
    int pressed = A.pressed_close;
    A.pressed_close = -1;
    if (hit == pressed && close) close_tab(pressed);
    return 1;
  }
  if (y < A.head || y >= A.head + A.bar) return 0;
  if (type == TMS_DOWN && button == 1) {
    if (hit == A.ntabs) new_tab();
    else if (hit >= 0 && close) A.pressed_close = hit;
    else if (hit >= 0) {
      int twice = (hit == A.tab_click && A.now - A.tab_click_at < 450);
      A.tab_click = twice ? -1 : hit;	/* a third click starts over */
      A.tab_click_at = A.now;
      if (twice && hit == A.cur) rename_start();
      else switch_tab(hit);
    }
  }
  else if (type == TMS_DOWN && button == 2 && hit >= 0 && hit < A.ntabs) close_tab(hit);
  else if (type == TMS_DOWN && button == 3) menu_open(x, y);
  return 1;
}

/* }================================================================== */

/*
** {==================================================================
** Mouse
** ===================================================================
*/

static void cell_at (int x, int y, int *cx, int *cy) {
  int col = (x - A.pad) / font_cell_w();
  int row = (y - TOP - A.pad) / font_cell_h();
  if (x < A.pad) col = 0;
  if (col >= A.g->cols) col = A.g->cols - 1;
  if (row < 0) row = 0;
  if (row >= A.g->rows) row = A.g->rows - 1;
  *cx = col;
  *cy = row - A.g->view;
}


static int is_word_char (const Line *l, int x) {
  uint32_t ch;
  if (l == NULL || x < 0 || x >= l->n) return 0;
  if (l->c[x].attr & A_WCONT) return 1;
  ch = l->c[x].ch;
  return ch > ' ' && (ch > 127 || strchr("\"'`()[]{}<>|;,", (int)ch) == NULL);
}


/*
** {==================================================================
** Telling a program about the mouse (vim, htop, lazygit ...)
** ===================================================================
*/

/* the program asked for mouse reports, and Shift is not held */
static int mouse_to_program (int mods) {
  return A.g->mouse != 0 && !(mods & TM_SHIFT) && A.g->view == 0 && !A.menu.open;
}


static int mouse_mods (int mods) {
  return ((mods & TM_SHIFT) ? 4 : 0) | ((mods & TM_ALT) ? 8 : 0) |
         ((mods & TM_CTRL) ? 16 : 0);
}


/* one report: SGR (1006) when the program asked for it, else the old bytes */
static void mouse_report (int code, int cx, int cy, int release) {
  char buf[64];
  if (cx < 0) cx = 0;
  if (cy < 0) cy = 0;
  if (cx >= A.g->cols) cx = A.g->cols - 1;
  if (cy >= A.g->rows) cy = A.g->rows - 1;
  if (A.g->mouse_sgr) {
    sprintf(buf, "\033[<%d;%d;%d%c", code, cx + 1, cy + 1, release ? 'm' : 'M');
    send_str(buf);
    return;
  }
  if (release) code = 3 | (code & ~3);	/* the old way cannot say which button */
  if (cx > 222 || cy > 222) return;	/* further right it cannot count */
  sprintf(buf, "\033[M%c%c%c", (char)(32 + code), (char)(33 + cx), (char)(33 + cy));
  send(buf, 6);
}


/* returns 1 when the event went to the program and the window is done with it */
static int mouse_send (int type, int button, int x, int y, int mods, int arg) {
  int cx, cy, code;
  cell_at(x, y, &cx, &cy);
  if (type == TMS_WHEEL) {
    int n = arg > 0 ? arg : -arg;
    code = (arg > 0 ? 64 : 65) | mouse_mods(mods);
    for (; n > 0; n--) mouse_report(code, cx, cy, 0);
    return 1;
  }
  if (type == TMS_DOWN) {
    if (button < 1 || button > 3) return 0;
    A.mouse_held = button;
    A.mouse_cx = cx;
    A.mouse_cy = cy;
    mouse_report((button - 1) | mouse_mods(mods), cx, cy, 0);
    return 1;
  }
  if (type == TMS_UP) {
    int held = A.mouse_held;
    A.mouse_held = 0;
    if (A.g->mouse == 9) return 1;	/* X10: presses only */
    if (held < 1 || held > 3) return 1;
    mouse_report((held - 1) | mouse_mods(mods), cx, cy, 1);
    return 1;
  }
  if (type == TMS_MOVE) {
    if (A.g->mouse != 1003 && !(A.g->mouse == 1002 && A.mouse_held)) return 1;
    if (cx == A.mouse_cx && cy == A.mouse_cy) return 1;	/* still the same cell */
    A.mouse_cx = cx;
    A.mouse_cy = cy;
    code = (A.mouse_held ? A.mouse_held - 1 : 3) | 32 | mouse_mods(mods);
    mouse_report(code, cx, cy, 0);
    return 1;
  }
  return 0;
}

/* }================================================================== */


/* grows the selection to whole words or whole lines */
static void sel_extend (void) {
  int swap = (A.by < A.ay) || (A.by == A.ay && A.bx < A.ax);
  int *lx = swap ? &A.bx : &A.ax, *ly = swap ? &A.by : &A.ay;
  int *rx = swap ? &A.ax : &A.bx, *ry = swap ? &A.ay : &A.by;
  if (A.sel_mode == 1) {
    const Line *l = grid_line(A.g, *ly), *r = grid_line(A.g, *ry);
    while (is_word_char(l, *lx) && is_word_char(l, *lx - 1)) (*lx)--;
    while (is_word_char(r, *rx) && is_word_char(r, *rx + 1)) (*rx)++;
  }
  else if (A.sel_mode == 2) {
    *lx = 0;
    *rx = A.g->cols - 1;
  }
}


static void scrollbar_drag (int y) {
  int bx, by, bw, bh, track_y = TOP + 4;
  int track_h = A.win_h - track_y - 4;
  draw_scrollbar_rect(&A.frame, &A.scene, &bx, &by, &bw, &bh);
  if (track_h - bh > 0 && A.g->sb_len > 0) {
    long pos = (long)(y - A.bar_grab - track_y) * A.g->sb_len / (track_h - bh);
    grid_set_view(A.g, A.g->sb_len - (int)pos);
  }
  A.bar_until = A.now + 1500;
  touch();
}


void app_on_mouse (int type, int button, int x, int y, int mods, int arg) {
  int cx, cy;
  A.mouse_x = x;
  A.mouse_y = y;
  if (A.renaming && type == TMS_DOWN) {	/* a click elsewhere keeps the name */
    int close;
    if (tab_at(x, y, &close) != A.cur || close) rename_end(1);
  }
  if (type == TMS_WHEEL) {
    if ((mods & TM_CTRL) && (mods & TM_SHIFT)) change_opacity(arg > 0 ? 5 : -5);
    else if (mods & TM_CTRL) zoom(arg > 0 ? 1 : -1);
    else if (A.bar > 0 && y >= A.head && y < A.head + A.bar)	/* over the tabs */
      switch_tab((A.cur + (arg > 0 ? A.ntabs - 1 : 1)) % A.ntabs);
    else if (mouse_to_program(mods)) mouse_send(type, button, x, y, mods, arg);
    else if (A.g->alt) {	/* full screen programs get arrow keys */
      int n = arg > 0 ? arg : -arg;
      for (; n > 0; n--) send_csi(0, 0, arg > 0 ? 'A' : 'B');
    }
    else scroll_view(arg);
    return;
  }
  if (type == TMS_LEAVE) {
    if (A.hot_button != -1 || A.hot_tab != -1) {
      A.hot_button = A.hot_tab = -1;
      touch();
    }
    if (A.menu.open && A.menu.hot != -1) {
      A.menu.hot = -1;
      touch();
    }
    A.bar_hover = 0;
    return;
  }
  if (header_mouse(type, button, x, y)) return;
  if (A.menu.open) {
    int hit = menu_hit(x, y);
    if (type == TMS_MOVE && hit != A.menu.hot) {
      A.menu.hot = hit;
      touch();
    }
    else if (type == TMS_DOWN) {
      int id = (hit >= 0) ? A.menu.id[hit] : 0;
      A.menu.open = 0;
      touch();
      menu_do(id);
    }
    return;
  }
  if (tabbar_mouse(type, button, x, y)) return;
  if (mouse_to_program(mods) && mouse_send(type, button, x, y, mods, arg)) return;
  cell_at(x, y, &cx, &cy);
  if (type == TMS_DOWN && button == 1) {
    int bx, by, bw, bh;
    draw_scrollbar_rect(&A.frame, &A.scene, &bx, &by, &bw, &bh);
    if (x >= A.win_w - 14 && A.g->sb_len > 0 && !A.g->alt) {
      A.bar_drag = 1;
      A.bar_grab = (y >= by && y < by + bh) ? y - by : bh / 2;
      scrollbar_drag(y);
      return;
    }
    if (A.now - A.click_at < 450 && cx == A.click_x && cy == A.click_y)
      A.clicks = A.clicks % 3 + 1;
    else A.clicks = 1;
    A.click_at = A.now;
    A.click_x = cx;
    A.click_y = cy;
    A.sel_mode = A.clicks - 1;
    if ((mods & TM_SHIFT) && A.has_sel) {	/* extend what is there */
      A.bx = cx;
      A.by = cy;
    }
    else {
      A.ax = A.bx = cx;
      A.ay = A.by = cy;
      A.has_sel = (A.sel_mode != 0);
    }
    A.selecting = 1;
    sel_extend();
    touch();
  }
  else if (type == TMS_MOVE) {
    int hover = (x >= A.win_w - 14);
    if (A.bar_drag) scrollbar_drag(y);
    else if (A.selecting && (cx != A.bx || cy != A.by || !A.has_sel)) {
      A.bx = cx;
      A.by = cy;
      A.has_sel = 1;
      sel_extend();
      touch();
    }
    if (hover != A.bar_hover) {
      A.bar_hover = hover;
      A.bar_until = A.now + 1200;
      touch();
    }
  }
  else if (type == TMS_UP && button == 1) {
    A.bar_drag = 0;
    if (A.selecting) {
      A.selecting = 0;
      if (A.has_sel && A.cfg.copy_on_select) copy_selection();
    }
  }
  else if (type == TMS_DOWN && button == 2) win_request_paste();
  else if (type == TMS_DOWN && button == 3) menu_open(x, y);
}

/* }================================================================== */


/*
** {==================================================================
** Shell output, timers, drawing
** ===================================================================
*/

static const char *window_title (void) {
  return CUR->label[0] ? CUR->label : CUR->title[0] ? CUR->title : TERM_NAME;
}


static void on_reply (void *ud, const char *s, size_t n) {
  Tab *t = (Tab *)ud;
  if (t->pty != NULL) pty_write(t->pty, s, n);
}


static void on_title (void *ud, const char *utf8) {
  Tab *t = (Tab *)ud;
  strncpy(t->title, utf8, sizeof(t->title) - 1);
  if (t == CUR) win_set_title(window_title());
  touch();
}


static void on_bell (void *ud) {
  (void)ud;
  win_flash();
}


/*
** {==================================================================
** OSC: colors a program asks about, the clipboard, the folder
** ===================================================================
*/

/* "#rrggbb", "rgb:rr/gg/bb" and "rgb:rrrr/gggg/bbbb" */
static int osc_color (const char *v, uint32_t *out) {
  unsigned r, g, b;
  if (v[0] == '#' && strlen(v + 1) == 6) {
    *out = (uint32_t)strtoul(v + 1, NULL, 16);
    return 1;
  }
  if (strncmp(v, "rgb:", 4) == 0) {
    char *end;
    const char *p = v + 4;
    size_t digits;
    r = (unsigned)strtoul(p, &end, 16);
    digits = (size_t)(end - p);
    if (*end != '/') return 0;
    p = end + 1;
    g = (unsigned)strtoul(p, &end, 16);
    if (*end != '/') return 0;
    b = (unsigned)strtoul(end + 1, &end, 16);
    if (digits == 4) {	/* 16 bit per channel: take the top byte */
      r >>= 8;
      g >>= 8;
      b >>= 8;
    }
    *out = ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
    return 1;
  }
  return 0;
}


static void osc_reply_color (Tab *t, int code, int index, uint32_t rgb) {
  char buf[96];
  unsigned r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
  if (code == 4)
    sprintf(buf, "\033]4;%d;rgb:%02x%02x/%02x%02x/%02x%02x\033\\", index, r, r, g, g, b, b);
  else
    sprintf(buf, "\033]%d;rgb:%02x%02x/%02x%02x/%02x%02x\033\\", code, r, r, g, g, b, b);
  on_reply(t, buf, strlen(buf));
}


static size_t osc_unbase64 (const char *s, char *out, size_t max) {
  static const char *const abc =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  unsigned long acc = 0;
  int bits = 0;
  size_t n = 0;
  for (; *s != '\0'; s++) {
    const char *at;
    if (*s == '=' || *s == '\r' || *s == '\n') continue;
    at = strchr(abc, *s);
    if (at == NULL) return 0;	/* not base64: drop the whole thing */
    acc = (acc << 6) | (unsigned long)(at - abc);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n + 1 >= max) return n;
      out[n++] = (char)((acc >> bits) & 0xFF);
    }
  }
  out[n] = '\0';
  return n;
}


/* "file://host/d/w/mmc" or a plain path -> the folder a new window opens in */
static void osc_set_cwd (Tab *t, const char *text) {
  char *cwd = t->cwd;
  const char *p = text;
  char *w;
  if (strncmp(p, "file://", 7) == 0) {
    p += 7;
    p = strchr(p, '/');	/* past the host name */
    if (p == NULL) return;
  }
  strncpy(cwd, p, sizeof(t->cwd) - 1);
  cwd[sizeof(t->cwd) - 1] = '\0';
  for (w = cwd; *w != '\0'; w++) {	/* %20 and friends */
    if (w[0] == '%' && isxdigit((unsigned char)w[1]) && isxdigit((unsigned char)w[2])) {
      char hex[3];
      hex[0] = w[1];
      hex[1] = w[2];
      hex[2] = '\0';
      *w = (char)strtol(hex, NULL, 16);
      memmove(w + 1, w + 3, strlen(w + 3) + 1);
    }
  }
#ifdef _WIN32
  if (cwd[0] == '/' && isalpha((unsigned char)cwd[1]) && cwd[2] == '/') {
    char drive[4];	/* /d/w/mmc -> D:\w\mmc */
    drive[0] = (char)toupper((unsigned char)cwd[1]);
    drive[1] = ':';
    drive[2] = '\0';
    memmove(cwd + 2, cwd + 3, strlen(cwd + 3) + 1);
    cwd[0] = drive[0];
    cwd[1] = ':';
  }
  for (w = cwd; *w != '\0'; w++)
    if (*w == '/') *w = '\\';
#endif
}


static void on_osc (void *ud, int code, const char *text) {
  Tab *t = (Tab *)ud;
  uint32_t rgb;
  if (code == 7) {
    osc_set_cwd(t, text);
    return;
  }
  if (code == 52) {	/* the program puts something in the clipboard */
    const char *data = strchr(text, ';');
    char *plain;
    size_t n;
    if (data == NULL) return;
    data++;
    if (data[0] == '?') return;	/* reading it back is not allowed */
    plain = (char *)xmalloc(strlen(data) + 4);
    n = osc_unbase64(data, plain, strlen(data) + 3);
    if (n > 0) win_set_clipboard(plain);
    free(plain);
    return;
  }
  if (code == 4) {	/* one color of the palette */
    const char *semi = strchr(text, ';');
    int idx = atoi(text);
    if (semi == NULL || idx < 0 || idx > 255) return;
    if (semi[1] == '?') {
      osc_reply_color(t, 4, idx, theme_color(&t->theme, COL_IDX(idx), 1));
      return;
    }
    if (idx < 16 && osc_color(semi + 1, &rgb)) {
      t->theme.pal[idx] = rgb;
      t->g->all_dirty = 1;
      touch();
    }
    return;
  }
  if (text[0] == '?') {	/* 10, 11, 12: what are your colors? */
    osc_reply_color(t, code, 0, code == 10 ? t->theme.fg :
                             code == 11 ? t->theme.bg : t->theme.cursor);
    return;
  }
  if (!osc_color(text, &rgb)) return;
  if (code == 10) t->theme.fg = rgb;
  else if (code == 11) t->theme.bg = rgb;
  else t->theme.cursor = rgb;
  t->g->all_dirty = 1;
  touch();
}

/* }================================================================== */


/*
** {==================================================================
** Tabs
** ===================================================================
*/

/* tells the program of the tab on screen that it got or lost the focus */
static void focus_report (int on) {
  if (A.g != NULL && A.g->focus_events && !CUR->done && CUR->pty != NULL)
    pty_write(CUR->pty, on ? "\033[I" : "\033[O", 3);
}


/* after CUR changed: the window shows that tab from now on */
static void show_tab (void) {
  A.g = CUR->g;
  A.g->all_dirty = 1;
  CUR->news = 0;
  A.has_sel = A.selecting = A.bar_drag = A.mouse_held = 0;
  A.hot_tab = -1;
  A.pressed_close = -1;
  win_set_title(window_title());
  touch();
}


static void switch_tab (int i) {
  if (i < 0 || i >= A.ntabs || i == A.cur) return;
  rename_end(1);
  if (A.focused) focus_report(0);
  A.cur = i;
  show_tab();
  if (A.focused) focus_report(1);
}


static void tab_free (Tab *t) {
  if (t->pty != NULL) pty_close(t->pty);
  vt_free(&t->vt);
  grid_free(t->g);
  free(t);
}


/* the last tab takes the window with it (and stays until then) */
static void close_tab (int i) {
  Tab *t;
  if (i < 0 || i >= A.ntabs) return;
  rename_end(1);
  if (A.ntabs == 1) {
    win_close();
    return;
  }
  t = A.tabs[i];
  memmove(&A.tabs[i], &A.tabs[i + 1], (size_t)(A.ntabs - i - 1) * sizeof(Tab *));
  A.ntabs--;
  if (i < A.cur || A.cur == A.ntabs) A.cur--;
  tab_free(t);
  show_tab();
  if (A.focused) focus_report(1);
  apply_bar();
}


/* the program of tab t ended */
static void finish (Tab *t) {
  char note[64];
  int i;
  if (t->done) return;
  t->done = 1;
  pty_exited(t->pty, &A.exit_code);
  if (!t->hold) {
    for (i = 0; i < A.ntabs; i++)
      if (A.tabs[i] == t) {
        close_tab(i);
        break;
      }
    return;
  }
  sprintf(note, "\r\n\033[0;2m[process ended with code %d]\033[0m", A.exit_code);
  vt_feed(&t->vt, note, strlen(note));
  t->g->cursor_on = 0;
  touch();
}


void app_on_wake (void) {
  static char buf[65536];
  int i = 0;
  while (i < A.ntabs) {
    Tab *t = A.tabs[i];
    size_t total = 0;
    long n = 0;
    int before = A.ntabs;
    if (t->pty == NULL) {
      i++;
      continue;
    }
    /* with --hold keep reading after the end: late output still arrives */
    while ((n = pty_read(t->pty, buf, sizeof(buf))) > 0) {
      vt_feed(&t->vt, buf, (size_t)n);
      total += (size_t)n;
      if (total > ((size_t)2 << 20)) {	/* stay responsive under a flood */
        win_wake();
        break;
      }
    }
    if (total > 0 && t == CUR) {
      if (A.has_sel && !A.selecting && A.g->view == 0) A.has_sel = 0;
      touch();
    }
    else if (total > 0 && !t->news && A.now > A.quiet_until) {
      t->news = 1;	/* not for the repaint every shell does on a resize */
      touch();
    }
    if (n < 0 || (n == 0 && pty_exited(t->pty, NULL))) finish(t);
    if (A.ntabs == before) i++;	/* else tab i is gone and i is the next one */
  }
}


int app_fds (int *fds, int max) {
  int i, n = 0;
  for (i = 0; i < A.ntabs && n < max; i++)
    if (A.tabs[i]->pty != NULL && pty_fd(A.tabs[i]->pty) >= 0)
      fds[n++] = pty_fd(A.tabs[i]->pty);
  return n;
}


/* the window closes: every shell ends; the screens stay until exit */
void app_close_all (void) {
  int i;
  for (i = 0; i < A.ntabs; i++) {
    if (A.tabs[i]->pty == NULL) continue;
    pty_close(A.tabs[i]->pty);
    A.tabs[i]->pty = NULL;
    A.tabs[i]->done = 1;
  }
}

/* }================================================================== */


void app_on_focus (int on) {
  A.focused = on;
  A.blink_on = 1;
  A.blink_at = A.now;
  focus_report(on);
  if (!on) {
    A.menu.open = 0;
    A.selecting = A.bar_drag = 0;
    rename_end(1);
  }
  touch();
}


void app_on_tick (unsigned now) {
  int i;
  A.now = now;
  if (A.focused && A.cfg.cursor_blink && now - A.blink_at >= 530) {
    A.blink_on = !A.blink_on;
    A.blink_at = now;
    touch();
  }
  if (A.selecting) {	/* dragging beyond the edge scrolls */
    if (A.mouse_y < TOP + A.pad) scroll_view(1);
    else if (A.mouse_y > A.win_h - A.pad) scroll_view(-1);
  }
  if (A.bar_until != 0 && now > A.bar_until && !A.bar_hover && !A.bar_drag) {
    A.bar_until = 0;
    touch();
  }
  if (A.pill_until != 0 && now > A.pill_until && !A.resizing) {
    A.pill_until = 0;
    touch();
  }
  for (i = 0; i < A.ntabs; i++)
    if (!A.tabs[i]->done && A.tabs[i]->pty != NULL && pty_exited(A.tabs[i]->pty, NULL)) {
      app_on_wake();
      break;
    }
}


static void build_scene (void) {
  Scene *s = &A.scene;
  int i;
  memset(s, 0, sizeof(*s));
  s->g = A.g;
  s->t = &CUR->theme;
  s->pad = A.pad;
  s->strip = A.strip;
  s->head = A.head;
  s->title = window_title();
  s->bar = A.bar;
  s->ntabs = A.ntabs;
  s->cur_tab = A.cur;
  s->hot_tab = A.hot_tab;
  s->hot_close = A.hot_close;
  for (i = 0; i < A.ntabs; i++) {
    const Tab *t = A.tabs[i];
    s->tab_title[i] = t->label[0] ? t->label : t->title[0] ? t->title : t->name;
    s->tab_news[i] = (unsigned char)t->news;
  }
  s->hot_button = A.hot_button;
  s->maximized = A.head > 0 && win_is_maximized();
  s->cw = font_cell_w();
  s->ch = font_cell_h();
  s->ascent = font_ascent();
  s->focused = A.focused;
  s->blink_on = A.blink_on || !A.cfg.cursor_blink;
  s->cursor_style = A.cfg.cursor;
  s->no_bold = A.cfg.no_bold;
  s->has_sel = A.has_sel;
  if (A.has_sel) sel_range(&s->sx0, &s->sy0, &s->sx1, &s->sy1);
  s->bar_alpha = (A.bar_drag || A.bar_hover) ? 230 :
                 (A.g->view > 0 || A.bar_until != 0) ? 150 : 0;
  s->pill = (A.pill_until != 0) ? A.pill : NULL;
  if (A.renaming && A.bar > 0) s->edit = A.edit;	/* typed in the tab itself */
  else if (A.renaming) {	/* no tab bar: in a pill in the middle */
    sprintf(A.edit_pill, "tab name: %s\xe2\x96\x8f", A.edit);	/* U+258F bar */
    s->pill = A.edit_pill;
  }
  s->menu = &A.menu;
}


const Frame *app_render (void) {
  if (!A.dirty || A.g == NULL || A.frame.px == NULL) return NULL;
  A.dirty = 0;
  build_scene();
  draw_scene(&A.frame, &A.scene);
  return &A.frame;
}

/* }================================================================== */


/*
** {==================================================================
** Start
** ===================================================================
*/

/* a tab with its screen and parser, nothing running in it yet */
static Tab *tab_make (int cols, int rows) {
  Tab *t = (Tab *)xmalloc(sizeof(Tab));
  memset(t, 0, sizeof(*t));
  t->g = grid_new(cols, rows, A.cfg.scrollback);
  vt_init(&t->vt, t->g);
  t->vt.ud = t;
  t->vt.reply = on_reply;
  t->vt.title = on_title;
  t->vt.bell = on_bell;
  t->vt.on_osc = on_osc;
  t->theme = A.theme;
  strcpy(t->name, MMC_NAME);
  return t;
}


/* the mmc folder: where the program is, unless that is <root>[/usr]/bin */
static char *find_root (const char *exe_dir) {
  char *dir = xstrdup(exe_dir);
  if (m_fncmp(path_basename(dir), "bin") == 0) {
    char *up = path_dirname(dir);
    free(dir);
    dir = up;
    if (m_fncmp(path_basename(dir), "usr") == 0) {
      up = path_dirname(dir);
      free(dir);
      dir = up;
    }
  }
  return dir;
}


int app_init (const AppArgs *args, const char *argv0) {
  char *etc, *fonts, *usr, *share;
  memset(&A, 0, sizeof(A));
  A.args = *args;
  A.exe = os_exe_path(argv0);
  A.exe_dir = path_dirname(A.exe);
  A.root = find_root(A.exe_dir);
  path_set_root(A.root);
  etc = path_join(A.root, "etc");
  mkdir_p(etc);
  A.conf = path_join(etc, "mmcterm.conf");
  config_defaults(&A.cfg);
  config_save_default(A.conf);
  config_load(&A.cfg, A.conf);
  if (args->theme != NULL) strncpy(A.cfg.theme, args->theme, sizeof(A.cfg.theme) - 1);
  if (args->font_size > 0)
    A.cfg.font_size = args->font_size < 6 ? 6 : args->font_size > 72 ? 72 : args->font_size;
  A.custom = win_custom_chrome(!A.cfg.native_titlebar);
  A.hot_button = A.pressed_button = -1;
  A.hot_tab = A.pressed_close = -1;
  usr = path_join(A.root, "usr");
  share = path_join(usr, "share");
  fonts = path_join(share, "fonts");
  font_add_dir(fonts);
  free(etc); free(usr); free(share); free(fonts);
  if (font_init(&A.cfg) != 0) {
    win_message(TERM_NAME, "No monospace font was found.\n"
                "Set font_file=... in etc/mmcterm.conf, or put a .ttf file in "
                "usr/share/fonts of the mmc folder.");
    return -1;
  }
  A.scale = win_scale();
  apply_font();
  theme_apply(&A.theme, theme_find(A.cfg.theme), &A.cfg);
  A.tabs[0] = tab_make(A.cfg.cols, A.cfg.rows);
  A.ntabs = 1;
  A.g = CUR->g;
  A.focused = A.blink_on = 1;
  return 0;
}


#ifdef _WIN32
#define EXE_EXT	".exe"
#else
#define EXE_EXT	""
#endif

/* "mmc" -> the program next to us, or somewhere in PATH */
static char *find_program (const char *name) {
  Vec dirs;
  size_t i;
  char *path, *file, *found = NULL;
  OsStat st;
  size_t n = strlen(name), e = strlen(EXE_EXT);
  file = (n > e && m_fncmp(name + n - e, EXE_EXT) == 0) ? xstrdup(name)
                                                        : xstrcat3(name, EXE_EXT, "");
  if (strpbrk(name, "/\\") != NULL) {
    char *native = path_to_native(file);
    free(file);
    return native;
  }
  vec_init(&dirs);
  vec_push(&dirs, xstrdup(A.exe_dir));
  if ((path = os_getenv("PATH")) != NULL) {
    path_list_split(path, &dirs);
    free(path);
  }
  for (i = 0; i < dirs.n && found == NULL; i++) {
    char *native = path_to_native(dirs.v[i]);
    char *cand = path_join(native, file);
    if (os_stat(cand, &st) == 0 && !st.is_dir) found = cand;
    else free(cand);
    free(native);
  }
  vec_free(&dirs);
  free(file);
  return found;
}


/* starts 'cmd' (NULL: the shell) in tab t; 0 if it runs */
static int tab_spawn (Tab *t, char **cmd) {
  char *one[2];
  char **argv = cmd;
  char *exe;
  const char *base;
  size_t n, e = strlen(EXE_EXT);
  if (argv == NULL) {
    one[0] = (char *)(A.cfg.shell[0] ? A.cfg.shell : MMC_NAME);
    one[1] = NULL;
    argv = one;
  }
  exe = find_program(argv[0]);
#ifndef _WIN32
  if (exe == NULL && cmd == NULL) {	/* no mmc here: the user's shell */
    exe = os_getenv("SHELL");
    if (exe == NULL) exe = xstrdup("/bin/sh");
  }
#endif
  if (exe == NULL) {
    char text[700];
    sprintf(text, "Cannot find the program to start: %.500s", argv[0]);
    win_message(TERM_NAME, text);
    return -1;
  }
  os_setenv("TERM", "xterm-256color");
  os_setenv("COLORTERM", "truecolor");
  os_setenv("MMC_TERM", "1");
  t->pty = pty_spawn(exe, argv, t->g->cols, t->g->rows);
  if (t->pty == NULL) {
    char text[700];
    sprintf(text, "Cannot start: %.500s", exe);
    win_message(TERM_NAME, text);
  }
  base = path_basename(exe);	/* "C:\...\nvim.exe" -> "nvim" */
  n = strlen(base);
  if (n > e && m_fncmp(base + n - e, EXE_EXT) == 0) n -= e;
  if (n >= sizeof(t->name)) n = sizeof(t->name) - 1;
  memcpy(t->name, base, n);
  t->name[n] = '\0';
  free(exe);
  return t->pty != NULL ? 0 : -1;
}


int app_start (void) {
  int ok;
  CUR->hold = A.args.hold;
  ok = tab_spawn(CUR, A.args.cmd);
  set_theme(A.cfg.theme, 0);
  win_set_opacity(A.cfg.opacity);
  return ok;
}


/* a new tab with the shell, in the folder of the tab on screen, at the end */
static void new_tab (void) {
  Tab *t;
  char *back;
  int ok;
  if (A.ntabs >= TAB_MAX) {
    win_flash();
    return;
  }
  t = tab_make(A.g->cols, A.g->rows);
  strcpy(t->cwd, CUR->cwd);
  back = enter_cwd();
  ok = tab_spawn(t, NULL);
  leave_cwd(back);
  if (ok != 0) {
    tab_free(t);
    return;
  }
  A.tabs[A.ntabs++] = t;
  switch_tab(A.ntabs - 1);
  apply_bar();
}


/* draws a sample without any window: used to check the look */
int app_render_test (const char *native) {
  static const char *const sample =
    "\033[32mmmc-term\033[0m render test \033[2m(dim)\033[0m\r\n"
    "\033[7;34m MMC \033[0m \033[1mbold\033[0m \033[3mitalic\033[0m "
    "\033[1;3mbold italic\033[0m \033[4munder\033[0m \033[9mstrike\033[0m "
    "\033[31mred \033[32mgreen \033[33myellow \033[34mblue \033[35mmagenta "
    "\033[36mcyan\033[0m\r\n"
    "The quick brown fox jumps over the lazy dog. 0O 1lI |{}[] => != -> ;:,.\r\n"
    "int main (void) { return printf(\"%d\\n\", 42) == 3; }  // ~ ` ' \" @ # $ % ^ & *\r\n"
    /* Powerline: a prompt with filled and thin arrows, rounds and slants */
    "\033[30;42m marjon \033[32;44m\xee\x82\xb0\033[30m ~/w/mmc \033[34;45m\xee\x82\xb0"
    "\033[30m \xee\x82\xa0 master \033[35;49m\xee\x82\xb0\033[0m "
    "\xee\x82\xb1 \xee\x82\xb3 \033[36m\xee\x82\xb6\033[30;46mround\033[36;49m\xee\x82\xb4"
    "\033[0m \033[33m\xee\x82\xba\033[30;43mslant\033[33;49m\xee\x82\xbc\033[0m "
    "\xee\x82\xb8\xee\x82\xbe \xee\x82\xa1 \xee\x82\xa2\r\n"
    /* Nerd icons: folder git linux windows apple terminal python, md file */
    "\033[33m\xef\x81\xbb\033[0m folder  \033[31m\xef\x87\x93\033[0m git  "
    "\xef\x85\xbc linux  \033[34m\xef\x85\xba\033[0m windows  \xef\x85\xb9 apple  "
    "\033[32m\xef\x84\xa0\033[0m terminal  \033[33m\xee\x9c\xbc\033[0m python  "
    "\xf3\xb0\x88\x94 file\r\n"
    "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\xac\xe2\x94\x80\xe2\x94\x90 "
    "\xe2\x95\x94\xe2\x95\x90\xe2\x95\x97 \xe2\x96\x80\xe2\x96\x84\xe2\x96\x88"
    "\xe2\x96\x91\xe2\x96\x92\xe2\x96\x93  \xce\xb1\xce\xb2\xce\xb3 \xc3\xa9\xc3\xa8"
    " \xe2\x86\x92 \xe2\x9c\x93 \xe2\x9c\x97\r\n"
    "\033[47;30m dark text on a light background \033[0m\r\n$ ";
  int w, h;
  grid_resize(A.g, 86, 11);
  /* two more tabs show the tab bar: one with a title, one with news */
  A.tabs[1] = tab_make(86, 11);
  strcpy(A.tabs[1]->title, "nvim notes.txt");
  A.tabs[2] = tab_make(86, 11);
  strcpy(A.tabs[2]->name, "build");
  A.tabs[2]->news = 1;
  A.ntabs = 3;
  A.bar = bar_height();
  size_for(A.g->cols, A.g->rows, &w, &h);
  frame_resize(&A.frame, w, h);
  A.win_w = w;
  A.win_h = h;
  vt_feed(&CUR->vt, sample, strlen(sample));
  A.dirty = 1;
  build_scene();
  draw_scene(&A.frame, &A.scene);
  return frame_save_bmp(&A.frame, native);
}

/* }================================================================== */
