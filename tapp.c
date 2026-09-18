/*
** tapp.c - behaviour of mmc-term
**
** Everything between the backend (keys, mouse, window) and the core
** (grid, parser, renderer): key mapping, selection and clipboard,
** scrolling, zoom, themes, the popup menu, the configuration.
*/

#include "mterm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


enum { M_COPY = 1, M_PASTE, M_SELECT_ALL, M_THEME, M_BIGGER, M_SMALLER,
       M_MORE_CLEAR, M_LESS_CLEAR, M_NEW_WINDOW, M_ABOUT };

static struct {
  AppArgs args;
  Config cfg;
  Theme theme;
  char *exe, *exe_dir, *root, *conf;
  Grid *g;
  Vt vt;
  Frame frame;
  Scene scene;
  Menu menu;
  float scale;
  int zoom;	/* points added to the configured font size */
  int pad, strip;
  int custom, head;	/* our own title bar and its height (0: none) */
  char title[256];
  int hot_button, pressed_button;
  int win_w, win_h;
  int dirty, focused, fullscreen, resizing;
  int blink_on;
  unsigned now, blink_at, bar_until, pill_until;
  char pill[32];
  int bar_hover, bar_drag, bar_grab;
  /* selection: anchor and moving end, y as in grid_line() */
  int selecting, sel_mode, has_sel;
  int ax, ay, bx, by;
  unsigned click_at;
  int clicks, click_x, click_y;
  int mouse_x, mouse_y;
  int done, exit_code;
} A;

/* where the terminal starts: below our title bar and the gradient strip */
#define TOP	(A.head + A.strip)


static void build_scene (void);


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

static void apply_font (void) {
  float px = (float)(A.cfg.font_size + A.zoom) * A.scale * 96.0f / 72.0f;
  font_set_px(px);
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
    grid_resize(A.g, cols, rows);
    pty_resize(cols, rows);
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
  strncpy(A.cfg.theme, name, sizeof(A.cfg.theme) - 1);
  theme_apply(&A.theme, theme_find(name), &A.cfg);
  /* a quiet frame: the header color with only a hint of the logo blue */
  win_set_chrome(A.theme.bg, blend(A.theme.ui, A.theme.accent1, A.theme.dark ? 70 : 110),
                 A.theme.fg, A.theme.dark);
  if (save && A.conf != NULL) config_set_key(A.conf, "theme", A.cfg.theme);
  touch();
}


static void toggle_theme (void) {
  set_theme(A.theme.dark ? "light" : "dark", 1);
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
  pty_write(s, n);
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


static void new_window (void) {
  char *argv[2];
  OsProc proc;
  long pid;
  argv[0] = A.exe;
  argv[1] = NULL;
  if (os_spawn(A.exe, argv, 0, 1, 2, &proc, &pid) == 0) os_detach(proc);
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
  Menu *m = &A.menu;
  int i, widest = 0, row = font_cell_h() + 10;
  memset(m, 0, sizeof(*m));
  menu_add("Copy", "Ctrl+Shift+C", M_COPY);
  menu_add("Paste", "Ctrl+Shift+V", M_PASTE);
  menu_add("Select all", NULL, M_SELECT_ALL);
  menu_add(NULL, NULL, 0);
  menu_add(A.theme.dark ? "Light theme" : "Dark theme", "Ctrl+Shift+T", M_THEME);
  menu_add("Bigger text", "Ctrl +", M_BIGGER);
  menu_add("Smaller text", "Ctrl -", M_SMALLER);
  menu_add("More transparent", "Ctrl+Shift+wheel", M_MORE_CLEAR);
  menu_add("Less transparent", NULL, M_LESS_CLEAR);
  menu_add(NULL, NULL, 0);
  menu_add("New window", "Ctrl+Shift+N", M_NEW_WINDOW);
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
    case M_THEME: toggle_theme(); break;
    case M_BIGGER: zoom(1); break;
    case M_SMALLER: zoom(-1); break;
    case M_MORE_CLEAR: change_opacity(-5); break;
    case M_LESS_CLEAR: change_opacity(5); break;
    case M_NEW_WINDOW: new_window(); break;
    case M_ABOUT: about(); break;
    default: break;
  }
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
    else if (cs && cp == 'T') toggle_theme();
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


int app_hit_test (int x, int y) {
  if (A.menu.open && x >= A.menu.x && x < A.menu.x + A.menu.w &&
      y >= A.menu.y && y < A.menu.y + A.menu.h)
    return HIT_UI;
  if (y >= A.head && x >= A.win_w - 14 && A.g->sb_len > 0 && !A.g->alt)
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
  if (type == TMS_WHEEL) {
    if ((mods & TM_CTRL) && (mods & TM_SHIFT)) change_opacity(arg > 0 ? 5 : -5);
    else if (mods & TM_CTRL) zoom(arg > 0 ? 1 : -1);
    else if (A.g->alt) {	/* full screen programs get arrow keys */
      int n = arg > 0 ? arg : -arg;
      for (; n > 0; n--) send_csi(0, 0, arg > 0 ? 'A' : 'B');
    }
    else scroll_view(arg);
    return;
  }
  if (type == TMS_LEAVE) {
    if (A.hot_button != -1) {
      A.hot_button = -1;
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

static void on_reply (void *ud, const char *s, size_t n) {
  (void)ud;
  pty_write(s, n);
}


static void on_title (void *ud, const char *utf8) {
  (void)ud;
  strncpy(A.title, utf8[0] ? utf8 : TERM_NAME, sizeof(A.title) - 1);
  win_set_title(A.title);
  touch();
}


static void on_bell (void *ud) {
  (void)ud;
  win_flash();
}


static void finish (void) {
  char note[64];
  if (A.done) return;
  A.done = 1;
  pty_exited(&A.exit_code);
  if (!A.args.hold) {
    win_close();
    return;
  }
  sprintf(note, "\r\n\033[0;2m[process ended with code %d]\033[0m", A.exit_code);
  vt_feed(&A.vt, note, strlen(note));
  A.g->cursor_on = 0;
  touch();
}


void app_on_wake (void) {
  static char buf[65536];
  size_t total = 0;
  long n;
  /* with --hold keep reading after the end: late output still arrives */
  while ((n = pty_read(buf, sizeof(buf))) > 0) {
    vt_feed(&A.vt, buf, (size_t)n);
    total += (size_t)n;
    if (total > ((size_t)2 << 20)) {	/* stay responsive under a flood */
      win_wake();
      break;
    }
  }
  if (total > 0) {
    if (A.has_sel && !A.selecting && A.g->view == 0) A.has_sel = 0;
    touch();
  }
  if (n < 0 || (n == 0 && pty_exited(NULL))) finish();
}


void app_on_focus (int on) {
  A.focused = on;
  A.blink_on = 1;
  A.blink_at = A.now;
  if (A.g != NULL && A.g->focus_events && !A.done)
    pty_write(on ? "\033[I" : "\033[O", 3);
  if (!on) {
    A.menu.open = 0;
    A.selecting = A.bar_drag = 0;
  }
  touch();
}


void app_on_tick (unsigned now) {
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
  if (!A.done && pty_exited(NULL)) app_on_wake();
}


static void build_scene (void) {
  Scene *s = &A.scene;
  memset(s, 0, sizeof(*s));
  s->g = A.g;
  s->t = &A.theme;
  s->pad = A.pad;
  s->strip = A.strip;
  s->head = A.head;
  s->title = A.title;
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
  A.custom = win_custom_chrome(!A.cfg.native_titlebar);
  A.hot_button = A.pressed_button = -1;
  strcpy(A.title, TERM_NAME);
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
  A.g = grid_new(A.cfg.cols, A.cfg.rows, A.cfg.scrollback);
  vt_init(&A.vt, A.g);
  A.vt.reply = on_reply;
  A.vt.title = on_title;
  A.vt.bell = on_bell;
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


int app_start (void) {
  char *one[2];
  char **argv = A.args.cmd;
  char *exe;
  int ok;
  if (argv == NULL) {
    one[0] = (char *)(A.cfg.shell[0] ? A.cfg.shell : MMC_NAME);
    one[1] = NULL;
    argv = one;
  }
  exe = find_program(argv[0]);
#ifndef _WIN32
  if (exe == NULL && A.args.cmd == NULL) {	/* no mmc here: the user's shell */
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
  ok = pty_spawn(exe, argv, A.g->cols, A.g->rows);
  if (ok != 0) {
    char text[700];
    sprintf(text, "Cannot start: %.500s", exe);
    win_message(TERM_NAME, text);
  }
  free(exe);
  set_theme(A.cfg.theme, 0);
  win_set_opacity(A.cfg.opacity);
  return ok;
}


/* draws a sample without any window: used to check the look */
int app_render_test (const char *native) {
  static const char *const sample =
    "\033[32mmmc-term\033[0m render test\r\n"
    "\033[7;34m MMC \033[0m \033[1mbold\033[0m \033[3mitalic\033[0m "
    "\033[4munder\033[0m \033[31mred \033[33myellow \033[36mcyan\033[0m\r\n$ ";
  int w, h;
  size_for(A.g->cols, A.g->rows, &w, &h);
  frame_resize(&A.frame, w, h);
  A.win_w = w;
  A.win_h = h;
  vt_feed(&A.vt, sample, strlen(sample));
  A.dirty = 1;
  build_scene();
  draw_scene(&A.frame, &A.scene);
  return frame_save_bmp(&A.frame, native);
}

/* }================================================================== */
