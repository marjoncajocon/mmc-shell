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
       M_NEW_WINDOW, M_ABOUT, M_FIND, M_SPLIT_RIGHT, M_SPLIT_DOWN, M_CLOSE_PANE,
       M_BLINK,
       M_THEME /* + theme number, keep last */ };

/* one shell with its own screen: a tab has one, or several side by side */
typedef struct Pane {
  Grid *g;
  Vt vt;
  Pty *pty;	/* NULL until started */
  Theme theme;	/* the window's, plus what the program changed (OSC 4, 10 ..) */
  char title[256];	/* what the program set, "" = none */
  char name[64];	/* the program: the tab says this without a title */
  char cwd[512];	/* what the shell reported with OSC 7 */
  int hold;	/* --hold: stays open when the program ends */
  int done;	/* the program ended */
  struct Tab *tab;
  int x, y, cols, rows;	/* its place in the tab, in cells */
} Pane;

/* how a tab is divided: one pane, or two parts side by side (vertical)
** or one above the other; permille is the first part's share */
typedef struct Split {
  Pane *pane;
  int vertical, permille;
  struct Split *a, *b, *up;
} Split;

/* what the tab bar shows: the window shows one tab at a time */
typedef struct Tab {
  Split *root;
  Pane *panes[PANE_MAX];
  int npanes;
  Pane *focus;	/* the pane that gets the keys */
  char label[64];	/* the name the user gave it, "" = none: wins over title */
  int news;	/* output came while it was hidden */
} Tab;

static struct {
  AppArgs args;
  Config cfg;
  Theme theme;
  char *exe, *exe_dir, *root, *conf;
  Tab *tabs[TAB_MAX];
  int ntabs, cur;
  int cols, rows;	/* the terminal part of the window, in cells (all panes) */
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
  int tab_drag, tab_drag_x;	/* the tab held by the mouse (-1: none), where it was taken */
  int tab_moving;	/* it went far enough to be moving, not clicked */
  int renaming;	/* the tab on screen is being renamed; A.edit is the text */
  char edit[64];
  char edit_pill[96];	/* no tab bar: the name is typed in a pill */
  int hot_button, pressed_button;
  int win_w, win_h;
  int dirty, focused, fullscreen, resizing;
  int blink_on;
  int tblink;	/* blinking text is shown (it blinks only while some is on screen) */
  unsigned tblink_at;
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
  /* find: the text, the places it is (x0 y0 x1 y1 each), the one shown */
  int finding;
  char find[128];
  char find_box[200];
  int *fm, fcount, fcap, fcur;
  /* the link under the mouse, cells as in grid_line() */
  int has_hot, hx0, hy0, hx1, hy1;
  char hot_url[2048];
  unsigned sync_since;	/* the program asked to hold the screen (2026) */
  uint32_t look;	/* everything on screen but the rows, last time: see look_of */
  struct Split *div_drag;	/* the line between two panes being dragged */
  int frame_ok;	/* the frame holds a whole drawing: rows can be redrawn alone */
} A;

#define CUR	(A.tabs[A.cur])
#define CP	(CUR->focus)	/* the pane with the focus */

/* where the terminal starts: below the title bar, the tabs and the strip */
#define TOP	(A.head + A.bar + A.strip)


static void build_scene (void);
static void layout_tab (Tab *t);
static void split_pane (int vertical);
static void close_pane (Pane *p);
static void focus_toward (int dx, int dy);
static void focus_report (int on);
static Pane *pane_at (int x, int y);
static void focus_pane (Pane *p);
static int div_mouse (int type, int button, int x, int y);
static void apply_bar (void);
static void new_tab (void);
static void close_tab (int i);
static void switch_tab (int i);
static const char *window_title (void);
static void rename_start (void);
static void rename_end (int keep);
static void rename_add (const char *utf8);
static void find_open (void);
static void find_key (int key, int mods, uint32_t cp);
static void find_add (const char *utf8);


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
  int i, k;
  font_set_px(px);
  for (i = 0; i < A.ntabs; i++)	/* images are placed by the cell size */
    for (k = 0; k < A.tabs[i]->npanes; k++) {
      A.tabs[i]->panes[k]->g->cell_w = font_cell_w();
      A.tabs[i]->panes[k]->g->cell_h = font_cell_h();
    }
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
  A.frame_ok = 0;
  if (A.g != NULL && (cols != A.cols || rows != A.rows)) {
    int i;
    A.cols = cols;
    A.rows = rows;
    for (i = 0; i < A.ntabs; i++) layout_tab(A.tabs[i]);	/* hidden tabs too */
    A.quiet_until = A.now + 1000;
    A.has_sel = A.has_hot = 0;
    A.fcur = -1;
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
  size_for(A.cols, A.rows, &w, &h);
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
    int k;
    for (k = 0; k < A.tabs[i]->npanes; k++) {
      A.tabs[i]->panes[k]->theme = A.theme;
      A.tabs[i]->panes[k]->g->all_dirty = 1;
    }
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
  if (CP->pty != NULL) pty_write(CP->pty, s, n);
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
  if (A.finding) {
    find_add(utf8);
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
  if (CP->cwd[0] == '\0') return NULL;
  back = os_getcwd();
  if (os_chdir(CP->cwd) != 0) {
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

/* a steady cursor costs nothing; a blinking one redraws its row twice a second */
static void toggle_blink (void) {
  A.cfg.cursor_blink = !A.cfg.cursor_blink;
  A.blink_on = 1;
  A.blink_at = A.now;
  if (A.g->cy < A.g->rows) A.g->screen[A.g->cy].dirty = 1;
  if (A.conf != NULL) config_set_key(A.conf, "cursor_blink", A.cfg.cursor_blink ? "yes" : "no");
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
  menu_add("Find", "Ctrl+Shift+F", M_FIND);
  menu_add(NULL, NULL, 0);
  menu_add("New tab", "Ctrl+Shift+T", M_NEW_TAB);
  menu_add("Rename tab", "double click", M_RENAME_TAB);
  menu_add("Split right", "Ctrl+Shift+D", M_SPLIT_RIGHT);
  menu_add("Split down", "Ctrl+Shift+E", M_SPLIT_DOWN);
  if (CUR->npanes > 1) menu_add("Close pane", "Ctrl+Shift+W", M_CLOSE_PANE);
  menu_add("Close tab", CUR->npanes > 1 ? NULL : "Ctrl+Shift+W", M_CLOSE_TAB);
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
  menu_add("Blinking cursor", A.cfg.cursor_blink ? "\xE2\x9C\x93" : NULL, M_BLINK);
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
    case M_FIND: find_open(); break;
    case M_SPLIT_RIGHT: split_pane(1); break;
    case M_SPLIT_DOWN: split_pane(0); break;
    case M_CLOSE_PANE: close_pane(CP); break;
    case M_BLINK: toggle_blink(); break;
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
  rename_add(t->label[0] ? t->label : t->focus->title[0] ? t->focus->title : t->focus->name);
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
    else if (cs && cp == 'W') {	/* the pane, or the tab when it is its only one */
      if (CUR->npanes > 1) close_pane(CP);
      else close_tab(A.cur);
    }
    else if (cs && cp == 'D') split_pane(1);	/* side by side */
    else if (cs && cp == 'E') split_pane(0);	/* one above the other */
    else if (cs && cp == 'L') next_theme();
    else if (cs && cp == 'A') select_all();
    else if (cs && cp == 'F') find_open();
    else if ((mods & TM_CTRL) && (cp == '=' || cp == '+')) zoom(1);
    else if ((mods & TM_CTRL) && cp == '-') zoom(-1);
    else if ((mods & TM_CTRL) && cp == '0') zoom(0);
    else if ((mods & TM_CTRL) && cp == ' ') send("\0", 1);
    else return 0;
    return 1;
  }
  if (CUR->npanes > 1 && (mods & TM_ALT) && !(mods & (TM_CTRL | TM_SHIFT)) &&
      (key == TK_LEFT || key == TK_RIGHT || key == TK_UP || key == TK_DOWN)) {
    focus_toward(key == TK_LEFT ? -1 : key == TK_RIGHT ? 1 : 0,
                 key == TK_UP ? -1 : key == TK_DOWN ? 1 : 0);	/* Alt+arrow: the next pane */
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


/* CSI code ; mods u: a key the way the kitty keyboard protocol says it */
static void send_kitty (uint32_t code, int mods) {
  char buf[48];
  int m = 1 + ((mods & TM_SHIFT) ? 1 : 0) + ((mods & TM_ALT) ? 2 : 0) + ((mods & TM_CTRL) ? 4 : 0);
  if (m > 1) sprintf(buf, "\033[%lu;%du", (unsigned long)code, m);
  else sprintf(buf, "\033[%luu", (unsigned long)code);
  send_str(buf);
}


/* a key the kitty protocol says differently (flag 1: what legacy mixes up;
** flag 8: every key); returns 1 when sent */
static int kitty_key (int key, int mods, uint32_t cp) {
  int k = grid_kitty(A.g);
  uint32_t code = 0;
  if (k == 0) return 0;
  if (key == TK_ESCAPE) code = 27;
  else if (key == TK_ENTER || key == TK_TAB || key == TK_BACKSPACE) {
    if (!(k & 8) && !(mods & (TM_CTRL | TM_ALT | TM_SHIFT))) return 0;
    code = key == TK_ENTER ? 13 : key == TK_TAB ? 9 : 127;
  }
  else if (key == TK_CHAR && (mods & (TM_CTRL | TM_ALT))) {
    code = (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;	/* the key, not the shifted letter */
  }
  if (code == 0) return 0;
  send_kitty(code, mods);
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
  if (A.finding) {	/* and here to the search */
    find_key(key, mods, cp);
    return 1;
  }
  if (shortcut(key, mods, cp)) return 1;
  if (kitty_key(key, mods, cp)) return 1;
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
  if (A.finding) {
    if (!(mods & TM_ALT)) find_add(utf8);
    return;
  }
  if (grid_kitty(A.g) & 8 || ((grid_kitty(A.g) & 1) && (mods & TM_ALT))) {
    const char *p = utf8;	/* every character as a key of its own */
    while (*p) {
      const unsigned char *u = (const unsigned char *)p;
      uint32_t c = *u;
      int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0, j;
      if (extra) c &= (0x3Fu >> extra);
      for (j = 1; j <= extra && (u[j] & 0xC0) == 0x80; j++) c = (c << 6) | (u[j] & 0x3F);
      p += j;
      if (c < 0x20 && c != 0x1B) {	/* Ctrl+letter that came as text */
        send_kitty(c + 96, (mods & TM_ALT) | TM_CTRL);
        continue;
      }
      if (c >= 'A' && c <= 'Z') send_kitty(c + 32, (mods & TM_ALT) | TM_SHIFT);
      else send_kitty(c, mods & TM_ALT);
    }
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


/* the held tab goes to the place of the tab under the mouse; the others
** make room */
static void tab_drag_to (int x) {
  int i, tx, ty, tw, th, to = -1;
  Tab *t;
  if (!A.tab_moving) {
    if (x - A.tab_drag_x < 6 && A.tab_drag_x - x < 6) return;	/* a shaky click */
    A.tab_moving = 1;
  }
  build_scene();
  for (i = 0; i < A.ntabs; i++) {
    draw_tab_rect(&A.frame, &A.scene, i, &tx, &ty, &tw, &th);
    if (x >= tx && x < tx + tw) to = i;
  }
  if (x < 0) to = 0;
  if (to < 0) {	/* past the last tab (on the + or beyond) */
    draw_tab_rect(&A.frame, &A.scene, 0, &tx, &ty, &tw, &th);
    to = (x < tx) ? 0 : A.ntabs - 1;
  }
  if (to == A.tab_drag) return;
  t = A.tabs[A.tab_drag];
  if (to < A.tab_drag)
    memmove(&A.tabs[to + 1], &A.tabs[to], (size_t)(A.tab_drag - to) * sizeof(Tab *));
  else
    memmove(&A.tabs[A.tab_drag], &A.tabs[A.tab_drag + 1],
            (size_t)(to - A.tab_drag) * sizeof(Tab *));
  A.tabs[to] = t;
  A.cur = A.tab_drag = A.hot_tab = to;
  A.tab_click = -1;	/* a move is not the first half of a double click */
  touch();
}


/* returns 1 if the mouse event belonged to the tab bar: a click shows a
** tab, on its x (or a middle click) closes it, + opens a new one; a tab
** dragged along the bar takes a new place */
static int tabbar_mouse (int type, int button, int x, int y) {
  int close, hit;
  if (A.tab_drag >= 0) {	/* held: the whole window follows it */
    if (type == TMS_MOVE) tab_drag_to(x);
    else if (type == TMS_UP && button == 1) {
      A.tab_drag = -1;
      A.tab_moving = 0;
    }
    return 1;
  }
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
      if (!A.renaming) {	/* it may be dragged from here */
        A.tab_drag = A.cur;
        A.tab_drag_x = x;
        A.tab_moving = 0;
      }
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
  int col = (x - A.pad) / font_cell_w() - CP->x;
  int row = (y - TOP - A.pad) / font_cell_h() - CP->y;
  if (x < A.pad || col < 0) col = 0;
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


/*
** {==================================================================
** Find in the screen and the scrollback (Ctrl+Shift+F), and the links
** under the mouse (OSC 8 ones, and addresses in the text)
** ===================================================================
*/

/* a logical line: the rows the terminal wrapped, as code points with their cells */
typedef struct Run {
  uint32_t *cp;
  int *x, *y;
  int n, cap;
} Run;


static void run_add (Run *r, uint32_t cp, int x, int y) {
  if (r->n == r->cap) {
    r->cap = r->cap ? r->cap * 2 : 256;
    r->cp = (uint32_t *)xrealloc(r->cp, (size_t)r->cap * sizeof(uint32_t));
    r->x = (int *)xrealloc(r->x, (size_t)r->cap * sizeof(int));
    r->y = (int *)xrealloc(r->y, (size_t)r->cap * sizeof(int));
  }
  r->cp[r->n] = cp;
  r->x[r->n] = x;
  r->y[r->n] = y;
  r->n++;
}


static void run_free (Run *r) {
  free(r->cp);
  free(r->x);
  free(r->y);
  memset(r, 0, sizeof(*r));
}


/* the first row of the logical line that has row y */
static int run_start (const Grid *g, int y) {
  while (y > -g->sb_len) {
    const Line *up = grid_line(g, y - 1);
    if (up == NULL || !up->wrapped) break;
    y--;
  }
  return y;
}


/* fills r with the logical line starting at row y; returns the row after it */
static int run_build (Run *r, const Grid *g, int y) {
  r->n = 0;
  for (; y < g->rows; y++) {
    const Line *l = grid_line(g, y);
    int x, last;
    if (l == NULL) break;
    for (last = l->n - 1; !l->wrapped && last >= 0 && l->c[last].ch == 0; last--)
      ;	/* no blanks after the text */
    for (x = 0; x <= last; x++) {
      if (l->c[x].attr & A_WCONT) continue;
      run_add(r, l->c[x].ch ? grid_base(g, &l->c[x]) : ' ', x, y);
    }
    if (!l->wrapped) return y + 1;
  }
  return y;
}


static uint32_t fold (uint32_t c) {
  return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}


/* every place the search text is, oldest first */
static void find_all (void) {
  uint32_t q[128];
  int nq = 0, y;
  const char *p = A.find;
  Run r;
  memset(&r, 0, sizeof(r));
  A.fcount = 0;
  while (*p && nq < 128) {	/* the search text as code points */
    const unsigned char *u = (const unsigned char *)p;
    uint32_t c = *u;
    int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0, k;
    if (extra) c &= (0x3Fu >> extra);
    for (k = 1; k <= extra && (u[k] & 0xC0) == 0x80; k++) c = (c << 6) | (u[k] & 0x3F);
    p += k;
    q[nq++] = fold(c);
  }
  if (nq == 0) return;
  for (y = -A.g->sb_len; y < A.g->rows;) {
    int i, next = run_build(&r, A.g, y);
    for (i = 0; i + nq <= r.n; i++) {
      int k;
      for (k = 0; k < nq && fold(r.cp[i + k]) == q[k]; k++)
        ;
      if (k < nq) continue;
      if (A.fcount == A.fcap) {
        A.fcap = A.fcap ? A.fcap * 2 : 64;
        A.fm = (int *)xrealloc(A.fm, (size_t)A.fcap * 4 * sizeof(int));
      }
      A.fm[A.fcount * 4] = r.x[i];
      A.fm[A.fcount * 4 + 1] = r.y[i];
      A.fm[A.fcount * 4 + 2] = r.x[i + nq - 1];
      A.fm[A.fcount * 4 + 3] = r.y[i + nq - 1];
      A.fcount++;
      i += nq - 1;
    }
    y = next > y ? next : y + 1;
  }
  run_free(&r);
}


/* shows match i: selected, and scrolled to if it is not on screen */
static void find_show (int i) {
  int y, view;
  A.fcur = i;
  if (i < 0 || i >= A.fcount) {
    A.has_sel = 0;
    return;
  }
  A.ax = A.fm[i * 4];
  A.ay = A.fm[i * 4 + 1];
  A.bx = A.fm[i * 4 + 2];
  A.by = A.fm[i * 4 + 3];
  A.has_sel = 1;
  A.sel_mode = 0;
  y = A.ay;
  if (y + A.g->view < 0 || A.by + A.g->view >= A.g->rows) {	/* off screen */
    view = A.g->rows / 2 - y;
    if (view < 0) view = 0;
    grid_set_view(A.g, view);
  }
}


/* the search text changed: stay on the match shown, or the next one up */
static void find_update (void) {
  int ox = A.has_sel ? A.ax : A.g->cols, oy = A.has_sel ? A.ay : A.g->rows, i, pick = -1;
  find_all();
  for (i = A.fcount - 1; i >= 0; i--) {
    int x = A.fm[i * 4], y = A.fm[i * 4 + 1];
    if (y < oy || (y == oy && x <= ox)) {
      pick = i;
      break;
    }
  }
  if (pick < 0 && A.fcount > 0) pick = A.fcount - 1;
  find_show(pick);
  touch();
}


static void find_open (void) {
  if (A.finding) {	/* again: the next match up */
    if (A.fcount > 0) find_show((A.fcur + A.fcount - 1) % A.fcount);
    touch();
    return;
  }
  A.menu.open = 0;
  rename_end(1);
  A.finding = 1;
  A.find[0] = '\0';
  A.fcount = 0;
  A.fcur = -1;
  touch();
}


static void find_close (void) {
  A.finding = 0;
  A.has_sel = 0;
  touch();
}


static void find_add (const char *utf8) {
  size_t have = strlen(A.find);
  for (; *utf8 != '\0'; utf8++) {
    unsigned char c = (unsigned char)*utf8;
    size_t n = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
    if (c < 0x20 || c == 0x7F) continue;
    if (have + n >= sizeof(A.find)) break;
    memcpy(A.find + have, utf8, n);
    have += n;
    utf8 += n - 1;
  }
  A.find[have] = '\0';
  find_update();
}


/* Enter or Up: the next match up (older); Shift+Enter or Down: down */
static void find_key (int key, int mods, uint32_t cp) {
  int cs = (mods & TM_CTRL) && (mods & TM_SHIFT);
  if (key == TK_ESCAPE) find_close();
  else if (key == TK_CHAR && cs && (cp == 'F' || cp == 'f')) find_open();
  else if (key == TK_CHAR && (mods & TM_CTRL) && (cp == 'V' || cp == 'v')) win_request_paste();
  else if ((key == TK_ENTER && !(mods & TM_SHIFT)) || key == TK_UP) {
    find_all();	/* new output moves things: count again */
    if (A.fcount > 0) find_show(A.fcur < 0 ? A.fcount - 1 : (A.fcur + A.fcount - 1) % A.fcount);
    touch();
  }
  else if (key == TK_ENTER || key == TK_DOWN) {
    find_all();
    if (A.fcount > 0) find_show((A.fcur + 1) % A.fcount);
    touch();
  }
  else if (key == TK_BACKSPACE) {
    size_t n = strlen(A.find);
    while (n > 0 && ((unsigned char)A.find[n - 1] & 0xC0) == 0x80) n--;
    if (n > 0) n--;
    if (mods & TM_CTRL) n = 0;
    A.find[n] = '\0';
    find_update();
  }
}


/*
** Only web and mail addresses are opened. A program can print any link,
** and a file:// one or a bare path could start a program on this machine.
*/
static int url_safe (const char *u) {
  static const char *const ok[] = {"https://", "http://", "ftp://", "mailto:", NULL};
  const char *p;
  int i;
  for (p = u; *p; p++)
    if ((unsigned char)*p < 0x20 || *p == 0x7F) return 0;
  for (i = 0; ok[i] != NULL; i++) {
    size_t n = strlen(ok[i]), k;
    for (k = 0; k < n && tolower((unsigned char)u[k]) == ok[i][k]; k++)
      ;
    if (k == n && u[n] != '\0') return 1;
  }
  return 0;
}


/* is c part of a web address? */
static int url_char (uint32_t c) {
  return c < 128 && (isalnum((int)c) || strchr("-._~:/?#[]@!$&'()*+,;=%", (int)c) != NULL);
}


/*
** The link under cell (cx, cy), y as in grid_line(): an OSC 8 one, or an
** address written in the text. Sets A.hot_*; returns 1 when there is one.
*/
static int link_at (int cx, int cy) {
  const Line *l = grid_line(A.g, cy);
  Run r;
  int i, at = -1, found = 0;
  static const char *const schemes[] = {"https://", "http://", "ftp://", "mailto:", "www.", NULL};
  A.has_hot = 0;
  if (l == NULL || cx < 0 || cx >= l->n) return 0;
  memset(&r, 0, sizeof(r));
  run_build(&r, A.g, run_start(A.g, cy));
  for (i = 0; i < r.n; i++)
    if (r.y[i] == cy && r.x[i] <= cx) at = i;	/* the character under the mouse */
  if (at >= 0 && (l->c[cx].attr & A_WCONT) == 0 && r.x[at] != cx) at = -1;
  if (at >= 0 && l->c[r.x[at]].link != 0) {	/* OSC 8: the cells with the same link */
    int id = l->c[r.x[at]].link, a = at, b = at;
    const char *uri = grid_link(A.g, id);
    while (a > 0 && grid_line(A.g, r.y[a - 1])->c[r.x[a - 1]].link == id) a--;
    while (b + 1 < r.n && grid_line(A.g, r.y[b + 1])->c[r.x[b + 1]].link == id) b++;
    if (uri != NULL) {
      strncpy(A.hot_url, uri, sizeof(A.hot_url) - 1);
      A.hot_url[sizeof(A.hot_url) - 1] = '\0';
      A.hx0 = r.x[a];
      A.hy0 = r.y[a];
      A.hx1 = r.x[b];
      A.hy1 = r.y[b];
      found = 1;
    }
  }
  else if (at >= 0 && url_char(r.cp[at])) {	/* an address in the text? */
    int a = at, b = at, k, s;
    char text[2048];
    while (a > 0 && url_char(r.cp[a - 1])) a--;
    while (b + 1 < r.n && url_char(r.cp[b + 1])) b++;
    for (s = a; s <= at && !found; s++) {	/* where does an address start? */
      size_t n = 0, j;
      for (k = 0; schemes[k] != NULL; k++) {
        n = strlen(schemes[k]);
        if ((size_t)(b - s + 1) <= n) continue;
        for (j = 0; j < n && fold(r.cp[s + (int)j]) == (uint32_t)schemes[k][j]; j++)
          ;
        if (j == n) break;
      }
      if (schemes[k] == NULL) continue;
      {	/* its end: no dot, comma or bracket that closes nothing */
        int e = b, open = 0, m;
        for (m = s; m <= e; m++) open += (r.cp[m] == '(') - (r.cp[m] == ')');
        while (e > s && (strchr(".,;:!?'\"", (int)r.cp[e]) != NULL ||
                         (r.cp[e] == ')' && open < 0))) {
          if (r.cp[e] == ')') open++;
          e--;
        }
        if (at > e || e - s + 1 >= (int)sizeof(text) - 16) break;
        n = 0;
        if (schemes[k][0] == 'w') {
          strcpy(text, "https://");
          n = 8;
        }
        for (m = s; m <= e; m++) text[n++] = (char)r.cp[m];
        text[n] = '\0';
        strcpy(A.hot_url, text);
        A.hx0 = r.x[s];
        A.hy0 = r.y[s];
        A.hx1 = r.x[e];
        A.hy1 = r.y[e];
        found = 1;
      }
    }
  }
  run_free(&r);
  A.has_hot = found;
  return found;
}


/* the mouse moved over the terminal: underline the link it is on */
static void hover (int x, int y) {
  int cx, cy, had = A.has_hot, ox0 = A.hx0, oy0 = A.hy0;
  if (x < A.pad || y < TOP) {
    A.has_hot = 0;
  }
  else {
    cell_at(x, y, &cx, &cy);
    link_at(cx, cy);
  }
  if (had != A.has_hot || (A.has_hot && (ox0 != A.hx0 || oy0 != A.hy0))) touch();
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
  if (type == TMS_DOWN && button == 1 && (mods & TM_CTRL) && y >= TOP) {
    hover(x, y);
    if (A.has_hot) {	/* Ctrl+click on a link opens it */
      if (url_safe(A.hot_url)) win_open_url(A.hot_url);
      return;
    }
  }
  if (div_mouse(type, button, x, y)) return;	/* dragging the line between panes */
  if (type == TMS_DOWN && CUR->npanes > 1 && y >= TOP) {	/* a click picks the pane */
    Pane *p = pane_at(x, y);
    if (p != NULL && p != CP) focus_pane(p);
  }
  if (type == TMS_MOVE && !A.selecting && !A.bar_drag) {
    if (CUR->npanes > 1 && pane_at(x, y) != CP) {
      if (A.has_hot) {
        A.has_hot = 0;
        touch();
      }
    }
    else hover(x, y);
  }
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
  return CUR->label[0] ? CUR->label : CP->title[0] ? CP->title : TERM_NAME;
}


static void on_reply (void *ud, const char *s, size_t n) {
  Pane *t = (Pane *)ud;
  if (t->pty != NULL) pty_write(t->pty, s, n);
}


static void on_title (void *ud, const char *utf8) {
  Pane *t = (Pane *)ud;
  strncpy(t->title, utf8, sizeof(t->title) - 1);
  if (t == CP) win_set_title(window_title());
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


static void osc_reply_color (Pane *t, int code, int index, uint32_t rgb) {
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
static void osc_set_cwd (Pane *t, const char *text) {
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
  Pane *t = (Pane *)ud;
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
  if (A.g != NULL && A.g->focus_events && !CP->done && CP->pty != NULL)
    pty_write(CP->pty, on ? "\033[I" : "\033[O", 3);
}


/* after CUR or its focus changed: the window shows and types into that pane */
static void show_tab (void) {
  A.g = CP->g;
  A.g->all_dirty = 1;
  CUR->news = 0;
  A.has_sel = A.selecting = A.bar_drag = A.mouse_held = 0;
  A.has_hot = A.finding = 0;
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


static void pane_free (Pane *p) {
  if (p->pty != NULL) pty_close(p->pty);
  vt_free(&p->vt);
  grid_free(p->g);
  free(p);
}


static void split_free (Split *s) {
  if (s == NULL) return;
  split_free(s->a);
  split_free(s->b);
  free(s);
}


static void tab_free (Tab *t) {
  int i;
  for (i = 0; i < t->npanes; i++) pane_free(t->panes[i]);
  if (A.div_drag != NULL && t == CUR) A.div_drag = NULL;
  split_free(t->root);
  free(t);
}


/*
** {==================================================================
** Panes: a tab split side by side or one above the other, each part
** with a shell of its own. The parts are a tree; between two parts is
** a gap of one cell with a line in it.
** ===================================================================
*/

/* gives every pane under s its place, in cells */
static void layout_split (Split *s, int x, int y, int cols, int rows) {
  int a;
  if (s->pane != NULL) {
    s->pane->x = x;
    s->pane->y = y;
    s->pane->cols = cols < 2 ? 2 : cols;
    s->pane->rows = rows < 1 ? 1 : rows;
    return;
  }
  if (s->vertical) {
    a = (int)((long)(cols - 1) * s->permille / 1000);
    if (a < 2) a = 2;
    if (cols - 1 - a < 2) a = cols - 3;
    layout_split(s->a, x, y, a, rows);
    layout_split(s->b, x + a + 1, y, cols - a - 1, rows);
  }
  else {
    a = (int)((long)(rows - 1) * s->permille / 1000);
    if (a < 1) a = 1;
    if (rows - 1 - a < 1) a = rows - 2;
    layout_split(s->a, x, y, cols, a);
    layout_split(s->b, x, y + a + 1, cols, rows - a - 1);
  }
}


/* the panes of t get their places, their grids and programs the size */
static void layout_tab (Tab *t) {
  int i;
  layout_split(t->root, 0, 0, A.cols, A.rows);
  for (i = 0; i < t->npanes; i++) {
    Pane *p = t->panes[i];
    if (p->g->cols == p->cols && p->g->rows == p->rows) continue;
    grid_resize(p->g, p->cols, p->rows);
    if (p->pty != NULL) pty_resize(p->pty, p->cols, p->rows);
  }
}


/* the pane under a point of the window, or NULL */
static Pane *pane_at (int x, int y) {
  int i, cx = (x - A.pad) / font_cell_w(), cy = (y - TOP - A.pad) / font_cell_h();
  if (x < A.pad || y < TOP + A.pad) return NULL;
  for (i = 0; i < CUR->npanes; i++) {
    Pane *p = CUR->panes[i];
    if (cx >= p->x && cx < p->x + p->cols && cy >= p->y && cy < p->y + p->rows) return p;
  }
  return NULL;
}


static void focus_pane (Pane *p) {
  if (p == NULL || p == CP) return;
  if (A.focused) focus_report(0);
  CUR->focus = p;
  show_tab();
  if (A.focused) focus_report(1);
}


/* Alt+arrow: the pane next to this one in that direction */
static void focus_toward (int dx, int dy) {
  Pane *me = CP, *best = NULL;
  int i, bestd = 1 << 30;
  int mx = me->x + me->cols / 2, my = me->y + me->rows / 2;
  for (i = 0; i < CUR->npanes; i++) {
    Pane *p = CUR->panes[i];
    int px = p->x + p->cols / 2, py = p->y + p->rows / 2, d;
    if (p == me) continue;
    if (dx > 0 && p->x < me->x + me->cols) continue;
    if (dx < 0 && p->x + p->cols > me->x) continue;
    if (dy > 0 && p->y < me->y + me->rows) continue;
    if (dy < 0 && p->y + p->rows > me->y) continue;
    d = (px - mx) * (px - mx) + (py - my) * (py - my);
    if (d < bestd) {
      bestd = d;
      best = p;
    }
  }
  focus_pane(best);
}


static Pane *pane_make (int cols, int rows);
static int pane_spawn (Pane *p, char **cmd);
static int split_has (const Split *s, const Pane *p);
static void close_pane (Pane *p);


/* splits the focused pane: side by side (vertical), or one above the other */
static void split_pane (int vertical) {
  Tab *t = CUR;
  Pane *old = CP, *p;
  Split *leaf, *s;
  char *back;
  int i;
  if (t->npanes >= PANE_MAX || (vertical ? old->cols < 8 : old->rows < 4)) {
    win_flash();
    return;
  }
  for (leaf = t->root; leaf != NULL && leaf->pane != old;)	/* find the leaf of the focus */
    leaf = (leaf->a != NULL && split_has(leaf->a, old)) ? leaf->a : leaf->b;
  if (leaf == NULL) return;
  p = pane_make(2, 2);
  p->tab = t;
  strcpy(p->cwd, old->cwd);
  s = (Split *)xmalloc(sizeof(Split));	/* the leaf becomes a split of old and new */
  memset(s, 0, sizeof(*s));
  s->pane = old;
  s->up = leaf;
  leaf->a = s;
  s = (Split *)xmalloc(sizeof(Split));
  memset(s, 0, sizeof(*s));
  s->pane = p;
  s->up = leaf;
  leaf->b = s;
  leaf->pane = NULL;
  leaf->vertical = vertical;
  leaf->permille = 500;
  t->panes[t->npanes++] = p;
  layout_tab(t);
  back = enter_cwd();	/* the new shell starts where the old one is */
  i = pane_spawn(p, NULL);
  leave_cwd(back);
  if (i != 0) {
    close_pane(p);
    return;
  }
  focus_pane(p);
  touch();
}


/* a pane goes; the one it shared its part with takes the room. The last
** pane takes the tab with it */
static void close_pane (Pane *p) {
  Tab *t = p->tab;
  Split *leaf, *up, *other;
  int i, ti;
  for (ti = 0; ti < A.ntabs && A.tabs[ti] != t; ti++)
    ;
  if (t->npanes == 1) {
    close_tab(ti);
    return;
  }
  for (leaf = t->root; leaf != NULL && leaf->pane != p;)
    leaf = (leaf->a != NULL && split_has(leaf->a, p)) ? leaf->a : leaf->b;
  if (leaf == NULL) return;
  up = leaf->up;
  A.div_drag = NULL;	/* the tree changes under it */
  other = (up->a == leaf) ? up->b : up->a;
  *up = (Split){other->pane, other->vertical, other->permille, other->a, other->b, up->up};
  if (up->a) up->a->up = up;
  if (up->b) up->b->up = up;
  free(other);
  free(leaf);
  for (i = 0; i < t->npanes && t->panes[i] != p; i++)
    ;
  memmove(&t->panes[i], &t->panes[i + 1], (size_t)(t->npanes - i - 1) * sizeof(Pane *));
  t->npanes--;
  if (t->focus == p) t->focus = t->panes[i < t->npanes ? i : t->npanes - 1];
  pane_free(p);
  layout_tab(t);
  if (t == CUR) show_tab();
  touch();
}


/* is pane p somewhere under s? */
static int split_has (const Split *s, const Pane *p) {
  if (s == NULL) return 0;
  if (s->pane != NULL) return s->pane == p;
  return split_has(s->a, p) || split_has(s->b, p);
}


/* the cells a part covers: x0, y0 inclusive, x1, y1 not */
static void split_box (const Split *s, int *x0, int *y0, int *x1, int *y1) {
  if (s->pane != NULL) {
    *x0 = s->pane->x;
    *y0 = s->pane->y;
    *x1 = s->pane->x + s->pane->cols;
    *y1 = s->pane->y + s->pane->rows;
    return;
  }
  {
    int a0, b0, a1, b1, c0, d0, c1, d1;
    split_box(s->a, &a0, &b0, &a1, &b1);
    split_box(s->b, &c0, &d0, &c1, &d1);
    *x0 = a0 < c0 ? a0 : c0;
    *y0 = b0 < d0 ? b0 : d0;
    *x1 = a1 > c1 ? a1 : c1;
    *y1 = b1 > d1 ? b1 : d1;
  }
}


/* the split whose line (in the middle of the gap between its two parts)
** is within a cell of this point of the window */
static Split *div_at (Split *s, int x, int y) {
  int x0, y0, x1, y1, bx0, by0, bx1, by1, cw = font_cell_w(), ch = font_cell_h();
  Split *in;
  if (s == NULL || s->pane != NULL) return NULL;
  split_box(s, &x0, &y0, &x1, &y1);
  split_box(s->b, &bx0, &by0, &bx1, &by1);
  if (s->vertical) {
    int lx = A.pad + (bx0 - 1) * cw + cw / 2;
    if (x >= lx - cw && x <= lx + cw && y >= TOP + A.pad + y0 * ch && y < TOP + A.pad + y1 * ch)
      return s;
  }
  else {
    int ly = TOP + A.pad + (by0 - 1) * ch + ch / 2;
    if (y >= ly - ch / 2 - 2 && y <= ly + ch / 2 + 2 && x >= A.pad + x0 * cw && x < A.pad + x1 * cw)
      return s;
  }
  if ((in = div_at(s->a, x, y)) != NULL) return in;
  return div_at(s->b, x, y);
}


/* press on the line between two panes, drag it, let go: the parts get
** their new shares. Returns 1 when the event was that. */
static int div_mouse (int type, int button, int x, int y) {
  int cx, cy;
  if (CUR->npanes < 2 || x < A.pad || y < TOP + A.pad) {
    if (type == TMS_UP) A.div_drag = NULL;
    return A.div_drag != NULL && type != TMS_UP;
  }
  cx = (x - A.pad) / font_cell_w();
  cy = (y - TOP - A.pad) / font_cell_h();
  if (type == TMS_DOWN && button == 1) {
    A.div_drag = div_at(CUR->root, x, y);
    return A.div_drag != NULL;
  }
  if (A.div_drag == NULL) return 0;
  if (type == TMS_UP) {
    A.div_drag = NULL;
    return 1;
  }
  if (type == TMS_MOVE) {
    Split *s = A.div_drag;
    int x0, y0, x1, y1, span, at, pm;
    split_box(s, &x0, &y0, &x1, &y1);
    span = s->vertical ? x1 - x0 - 1 : y1 - y0 - 1;
    at = s->vertical ? cx - x0 : cy - y0;
    if (span < 2) return 1;
    pm = at * 1000 / span;
    if (pm < 50) pm = 50;
    if (pm > 950) pm = 950;
    if (pm != s->permille) {
      s->permille = pm;
      layout_tab(CUR);
      A.frame_ok = 0;
      touch();
    }
    return 1;
  }
  return 1;
}


/* the gaps between the panes of the tab on screen, for the renderer */
static void split_divs (const Split *s, Scene *sc) {
  int x, y, w, h, x0, y0, x1, y1, bx0, by0, bx1, by1;
  if (s == NULL || s->pane != NULL || sc->ndivs >= PANE_MAX) return;
  split_box(s, &x0, &y0, &x1, &y1);
  split_box(s->b, &bx0, &by0, &bx1, &by1);	/* the gap lies just before the second part */
  if (s->vertical) {
    x = A.pad + (bx0 - 1) * font_cell_w();
    y = TOP + A.pad + y0 * font_cell_h();
    w = font_cell_w();
    h = (y1 - y0) * font_cell_h();
  }
  else {
    x = A.pad + x0 * font_cell_w();
    y = TOP + A.pad + (by0 - 1) * font_cell_h();
    w = (x1 - x0) * font_cell_w();
    h = font_cell_h();
  }
  sc->div[sc->ndivs][0] = x;
  sc->div[sc->ndivs][1] = y;
  sc->div[sc->ndivs][2] = w;
  sc->div[sc->ndivs][3] = h;
  sc->ndivs++;
  split_divs(s->a, sc);
  split_divs(s->b, sc);
}

/* }================================================================== */


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
  A.tab_drag = -1;
  memmove(&A.tabs[i], &A.tabs[i + 1], (size_t)(A.ntabs - i - 1) * sizeof(Tab *));
  A.ntabs--;
  if (i < A.cur || A.cur == A.ntabs) A.cur--;
  tab_free(t);
  show_tab();
  if (A.focused) focus_report(1);
  apply_bar();
}


/* the program of pane p ended */
static void finish (Pane *p) {
  char note[64];
  if (p->done) return;
  p->done = 1;
  pty_exited(p->pty, &A.exit_code);
  if (!p->hold) {
    close_pane(p);
    return;
  }
  sprintf(note, "\r\n\033[0;2m[process ended with code %d]\033[0m", A.exit_code);
  vt_feed(&p->vt, note, strlen(note));
  p->g->cursor_on = 0;
  touch();
}


/* every pane of every tab, for loops: returns the count */
static int all_panes (Pane **out) {
  int i, k, n = 0;
  for (i = 0; i < A.ntabs; i++)
    for (k = 0; k < A.tabs[i]->npanes; k++) out[n++] = A.tabs[i]->panes[k];
  return n;
}


static int pane_alive (Pane *p) {
  Pane *all[TAB_MAX * PANE_MAX];
  int i, n = all_panes(all);
  for (i = 0; i < n; i++)
    if (all[i] == p) return 1;
  return 0;
}


void app_on_wake (void) {
  static char buf[65536];
  Pane *all[TAB_MAX * PANE_MAX];
  int i, n = all_panes(all);
  for (i = 0; i < n; i++) {
    Pane *p = all[i];
    size_t total = 0;
    long got = 0;
    if (!pane_alive(p) || p->pty == NULL) continue;	/* closed by one before it */
    /* with --hold keep reading after the end: late output still arrives */
    while ((got = pty_read(p->pty, buf, sizeof(buf))) > 0) {
      vt_feed(&p->vt, buf, (size_t)got);
      total += (size_t)got;
      if (total > ((size_t)2 << 20)) {	/* stay responsive under a flood */
        win_wake();
        break;
      }
    }
    if (total > 0 && p == CP) {
      if (A.has_sel && !A.selecting && A.g->view == 0 && !A.finding) A.has_sel = 0;
      A.has_hot = 0;
      touch();
    }
    else if (total > 0 && p->tab == CUR) touch();	/* another pane on screen */
    else if (total > 0 && !p->tab->news && A.now > A.quiet_until) {
      p->tab->news = 1;	/* not for the repaint every shell does on a resize */
      touch();
    }
    if (got < 0 || (got == 0 && pty_exited(p->pty, NULL))) finish(p);
  }
}


int app_fds (int *fds, int max) {
  Pane *all[TAB_MAX * PANE_MAX];
  int i, n = 0, np = all_panes(all);
  for (i = 0; i < np && n < max; i++)
    if (all[i]->pty != NULL && pty_fd(all[i]->pty) >= 0) fds[n++] = pty_fd(all[i]->pty);
  return n;
}


/* the window closes: every shell ends; the screens stay until exit */
void app_close_all (void) {
  Pane *all[TAB_MAX * PANE_MAX];
  int i, n = all_panes(all);
  for (i = 0; i < n; i++) {
    if (all[i]->pty == NULL) continue;
    pty_close(all[i]->pty);
    all[i]->pty = NULL;
    all[i]->done = 1;
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
    A.tab_drag = -1;
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
    if (A.g->cy < A.g->rows) A.g->screen[A.g->cy].dirty = 1;	/* only its row */
    touch();
  }
  if (now - A.tblink_at >= 500) {	/* SGR 5: blinking text */
    int y, x, any = 0;
    A.tblink_at = now;
    for (y = 0; y < A.g->rows && !any; y++) {
      const Line *l = grid_view_line(A.g, y);
      for (x = 0; l != NULL && x < l->n && !any; x++) any = (l->c[x].attr & A_BLINK) != 0;
    }
    if (any || !A.tblink) {
      A.tblink = any ? !A.tblink : 1;
      touch();
    }
  }
  if (A.selecting) {	/* dragging beyond the edge scrolls */
    if (A.mouse_y < TOP + A.pad) scroll_view(1);
    else if (A.mouse_y > A.win_h - A.pad) scroll_view(-1);
  }
  if (A.bar_until != 0 && now > A.bar_until && !A.bar_hover && !A.bar_drag) {
    A.bar_until = 0;
    touch();
  }
  if (A.dirty && A.sync_since != 0) win_redraw();	/* a held screen: try again */
  if (A.pill_until != 0 && now > A.pill_until && !A.resizing) {
    A.pill_until = 0;
    touch();
  }
  {
    Pane *all[TAB_MAX * PANE_MAX];
    int n = all_panes(all);
    for (i = 0; i < n; i++)
      if (!all[i]->done && all[i]->pty != NULL && pty_exited(all[i]->pty, NULL)) {
        app_on_wake();
        break;
      }
  }
}


static void build_scene (void) {
  Scene *s = &A.scene;
  int i;
  memset(s, 0, sizeof(*s));
  s->g = A.g;
  s->t = &CP->theme;
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
    s->tab_title[i] = t->label[0] ? t->label : t->focus->title[0] ? t->focus->title : t->focus->name;
    s->tab_news[i] = (unsigned char)t->news;
  }
  s->hot_button = A.hot_button;
  s->maximized = A.head > 0 && win_is_maximized();
  s->cw = font_cell_w();
  s->ch = font_cell_h();
  s->ascent = font_ascent();
  s->focused = A.focused;
  s->blink_on = A.blink_on || !A.cfg.cursor_blink;
  s->text_blink_on = A.tblink;
  s->cursor_style = A.cfg.cursor;
  s->no_bold = A.cfg.no_bold;
  s->has_sel = A.has_sel;
  if (A.has_sel) sel_range(&s->sx0, &s->sy0, &s->sx1, &s->sy1);
  s->has_hot = A.has_hot;
  s->hx0 = A.hx0;
  s->hy0 = A.hy0;
  s->hx1 = A.hx1;
  s->hy1 = A.hy1;
  if (A.finding) {
    if (A.find[0] == '\0') sprintf(A.find_box, "find: \xe2\x96\x8f");
    else if (A.fcount == 0) sprintf(A.find_box, "find: %s\xe2\x96\x8f  no match", A.find);
    else sprintf(A.find_box, "find: %s\xe2\x96\x8f  %d/%d", A.find, A.fcur + 1, A.fcount);
    s->find = A.find_box;
  }
  s->bar_alpha = (A.bar_drag || A.bar_hover) ? 230 :
                 (A.g->view > 0 || A.bar_until != 0) ? 150 : 0;
  s->pill = (A.pill_until != 0) ? A.pill : NULL;
  if (A.renaming && A.bar > 0) s->edit = A.edit;	/* typed in the tab itself */
  else if (A.renaming) {	/* no tab bar: in a pill in the middle */
    sprintf(A.edit_pill, "tab name: %s\xe2\x96\x8f", A.edit);	/* U+258F bar */
    s->pill = A.edit_pill;
  }
  s->menu = &A.menu;
  if (CUR->npanes > 1) {	/* a split tab: every pane where it is */
    for (i = 0; i < CUR->npanes; i++) {
      const Pane *p = CUR->panes[i];
      s->pane[i].g = p->g;
      s->pane[i].t = &p->theme;
      s->pane[i].x = A.pad + p->x * s->cw;
      s->pane[i].y = TOP + A.pad + p->y * s->ch;
      s->pane[i].w = p->cols * s->cw;
      s->pane[i].h = p->rows * s->ch;
      s->pane[i].focused = (p == CP);
    }
    s->npanes = CUR->npanes;
    s->ox = A.pad + CP->x * s->cw;
    s->oy = TOP + A.pad + CP->y * s->ch;
    split_divs(CUR->root, s);
  }
}


static uint32_t mix_in (uint32_t h, const void *p, size_t n) {
  const unsigned char *b = (const unsigned char *)p;
  size_t i;
  for (i = 0; i < n; i++) {
    h ^= b[i];
    h *= 16777619u;
  }
  return h;
}


static uint32_t mix_str (uint32_t h, const char *s) {
  return s ? mix_in(h, s, strlen(s) + 1) : mix_in(h, "", 1);
}


/* a fingerprint of all the scene shows besides the rows of the grids:
** when it is the same as last time, only dirty rows need drawing */
static uint32_t look_of (const Scene *s) {
  uint32_t h = 2166136261u;
  int v[40], i, n = 0;
  v[n++] = s->head; v[n++] = s->bar; v[n++] = s->strip; v[n++] = s->pad;
  v[n++] = s->ntabs; v[n++] = s->cur_tab; v[n++] = s->hot_tab; v[n++] = s->hot_close;
  v[n++] = s->hot_button; v[n++] = s->maximized; v[n++] = s->cw; v[n++] = s->ch;
  v[n++] = s->focused; v[n++] = s->cursor_style; v[n++] = s->no_bold;
  v[n++] = s->has_sel; v[n++] = s->sx0; v[n++] = s->sy0; v[n++] = s->sx1; v[n++] = s->sy1;
  v[n++] = s->has_hot; v[n++] = s->hx0; v[n++] = s->hy0; v[n++] = s->hx1; v[n++] = s->hy1;
  v[n++] = s->bar_alpha; v[n++] = s->text_blink_on; v[n++] = s->npanes;
  v[n++] = s->ox; v[n++] = s->oy; v[n++] = s->ndivs; v[n++] = s->g->view;
  v[n++] = s->g->cols; v[n++] = s->g->rows; v[n++] = A.frame.w; v[n++] = A.frame.h;
  h = mix_in(h, v, (size_t)n * sizeof(int));
  h = mix_in(h, s->t, sizeof(Theme));
  h = mix_str(h, s->title);
  h = mix_str(h, s->edit);
  h = mix_str(h, s->pill);
  h = mix_str(h, s->find);
  for (i = 0; i < s->ntabs; i++) {
    h = mix_str(h, s->tab_title[i]);
    h = mix_in(h, &s->tab_news[i], 1);
  }
  for (i = 0; i < s->npanes; i++) {
    h = mix_in(h, &s->pane[i].x, 5 * sizeof(int));
    h = mix_in(h, s->pane[i].t, sizeof(Theme));
  }
  if (s->menu != NULL) h = mix_in(h, s->menu, sizeof(Menu));
  return h;
}


/* what was drawn is clean now */
static void clean_grid (Grid *g) {
  int r;
  g->all_dirty = 0;
  for (r = 0; r < g->rows; r++) {
    const Line *l = grid_view_line(g, r);
    if (l != NULL) ((Line *)l)->dirty = 0;
    g->screen[r].dirty = 0;
  }
}


const Frame *app_render (void) {
  if (!A.dirty || A.g == NULL || A.frame.px == NULL) return NULL;
  if (A.g->sync) {	/* 2026: the program is still drawing */
    if (A.sync_since == 0) A.sync_since = A.now ? A.now : 1;
    if (A.now - A.sync_since < 200) return NULL;
  }
  A.sync_since = 0;
  A.dirty = 0;
  build_scene();
  {
    uint32_t look = look_of(&A.scene);
    const Scene *s = &A.scene;
    int i, full = !A.frame_ok || look != A.look;
    /* what floats over the rows (menu, find box, pill, scrollbar) is not
    ** drawn again with them: while it shows, the whole frame is */
    if ((s->menu != NULL && s->menu->open) || s->find != NULL || s->pill != NULL ||
        (s->bar_alpha > 0 && s->g->sb_len > 0 && !s->g->alt))
      full = 1;
    for (i = 0; i < CUR->npanes && !full; i++) full = CUR->panes[i]->g->all_dirty;
    if (full) draw_scene(&A.frame, &A.scene);
    else if (!draw_scene_rows(&A.frame, &A.scene)) return NULL;	/* nothing changed */
    A.look = look;
    A.frame_ok = 1;
    for (i = 0; i < CUR->npanes; i++) clean_grid(CUR->panes[i]->g);
  }
  return &A.frame;
}

/* }================================================================== */


/*
** {==================================================================
** Start
** ===================================================================
*/

/* a tab of one pane */
static Tab *tab_new (Pane *p) {
  Tab *t = (Tab *)xmalloc(sizeof(Tab));
  memset(t, 0, sizeof(*t));
  t->root = (Split *)xmalloc(sizeof(Split));
  memset(t->root, 0, sizeof(Split));
  t->root->pane = p;
  t->panes[0] = p;
  t->npanes = 1;
  t->focus = p;
  p->tab = t;
  p->x = p->y = 0;
  p->cols = p->g->cols;
  p->rows = p->g->rows;
  return t;
}


/* a pane with its screen and parser, nothing running in it yet */
static Pane *pane_make (int cols, int rows) {
  Pane *t = (Pane *)xmalloc(sizeof(Pane));
  memset(t, 0, sizeof(*t));
  t->g = grid_new(cols, rows, A.cfg.scrollback);
  t->g->cell_w = font_cell_w();
  t->g->cell_h = font_cell_h();
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
  A.hot_tab = A.pressed_close = A.tab_drag = -1;
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
  A.tabs[0] = tab_new(pane_make(A.cfg.cols, A.cfg.rows));
  A.ntabs = 1;
  A.cols = A.cfg.cols;
  A.rows = A.cfg.rows;
  A.g = CP->g;
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


/* starts 'cmd' (NULL: the shell) in pane t; 0 if it runs */
static int pane_spawn (Pane *t, char **cmd) {
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
  CP->hold = A.args.hold;
  ok = pane_spawn(CP, A.args.cmd);
  set_theme(A.cfg.theme, 0);
  win_set_opacity(A.cfg.opacity);
  return ok;
}


/* a new tab with the shell, in the folder of the tab on screen, at the end */
static void new_tab (void) {
  Tab *t;
  Pane *p;
  char *back;
  int ok;
  if (A.ntabs >= TAB_MAX) {
    win_flash();
    return;
  }
  p = pane_make(A.cols, A.rows);
  strcpy(p->cwd, CP->cwd);
  back = enter_cwd();
  ok = pane_spawn(p, NULL);
  leave_cwd(back);
  if (ok != 0) {
    pane_free(p);
    return;
  }
  t = tab_new(p);
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
  A.cols = 86;
  A.rows = 11;
  layout_tab(CUR);
  /* two more tabs show the tab bar: one with a title, one with news */
  A.tabs[1] = tab_new(pane_make(86, 11));
  strcpy(A.tabs[1]->focus->title, "nvim notes.txt");
  A.tabs[2] = tab_new(pane_make(86, 11));
  strcpy(A.tabs[2]->focus->name, "build");
  A.tabs[2]->news = 1;
  A.ntabs = 3;
  A.bar = bar_height();
  size_for(A.cols, A.rows, &w, &h);
  frame_resize(&A.frame, w, h);
  A.win_w = w;
  A.win_h = h;
  vt_feed(&CP->vt, sample, strlen(sample));
  A.dirty = 1;
  build_scene();
  draw_scene(&A.frame, &A.scene);
  return frame_save_bmp(&A.frame, native);
}

/* }================================================================== */
