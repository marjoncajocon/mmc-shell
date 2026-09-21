/*
** tvt.c - escape sequence parser of mmc-term
**
** Bytes from the program go in; calls on the Grid come out. Speaks the
** xterm dialect that ConPTY, ncurses programs and editors use.
*/

#include "mterm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


enum { S_GROUND, S_ESC, S_ESC_SKIP, S_CHARSET, S_CSI, S_OSC, S_OSC_ESC,
       S_STRING, S_STRING_ESC };


void vt_init (Vt *vt, Grid *g) {
  memset(vt, 0, sizeof(*vt));
  vt->g = g;
  buf_init(&vt->osc);
}


void vt_free (Vt *vt) {
  buf_free(&vt->osc);
  buf_free(&vt->dcs);
}


static void reply (Vt *vt, const char *s) {
  if (vt->reply) vt->reply(vt->ud, s, strlen(s));
}


/* DEC special graphics: what 'q' means after ESC ( 0 */
static uint32_t line_drawing (uint32_t c) {
  static const uint16_t map[] = {	/* 0x60 .. 0x7E */
    0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1,
    0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA,
    0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, 0x252C,
    0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7
  };
  return (c >= 0x60 && c <= 0x7E) ? map[c - 0x60] : c;
}


static void print (Vt *vt, uint32_t cp) {
  if (vt->charset[vt->gl]) cp = line_drawing(cp);
  vt->last = cp;
  grid_putc(vt->g, cp);
}


static void control (Vt *vt, int c) {
  Grid *g = vt->g;
  switch (c) {
    case 0x07: if (vt->bell) vt->bell(vt->ud); break;
    case 0x08: grid_bs(g); break;
    case 0x09: grid_tab(g, 1); break;
    case 0x0A: case 0x0B: case 0x0C: grid_lf(g); break;
    case 0x0D: grid_cr(g); break;
    case 0x0E: vt->gl = 1; break;
    case 0x0F: vt->gl = 0; break;
    default: break;
  }
}


/*
** {==================================================================
** CSI
** ===================================================================
*/

static int param (const Vt *vt, int i, int def) {
  return (i < vt->nparams && vt->params[i] > 0) ? vt->params[i] : def;
}


/* 38;5;n  38;2;r;g;b  38:2::r:g:b -> color; returns the last index used */
static int extended_color (const Vt *vt, int i, uint32_t *out) {
  int kind = (i + 1 < vt->nparams) ? vt->params[i + 1] : -1;
  if (kind == 5 && i + 2 < vt->nparams) {
    *out = COL_IDX(vt->params[i + 2] & 0xFF);
    return i + 2;
  }
  if (kind == 2) {
    int j = i + 2, subs = 0, k;
    for (k = i + 1; k < vt->nparams && vt->sub[k]; k++) subs++;
    if (subs >= 5) j++;	/* colon form carries a color space id first */
    if (j + 2 < vt->nparams) {
      *out = COL_RGB(((uint32_t)(vt->params[j] & 0xFF) << 16) |
                     ((uint32_t)(vt->params[j + 1] & 0xFF) << 8) |
                     (uint32_t)(vt->params[j + 2] & 0xFF));
    }
    return j + 2;
  }
  return vt->nparams;	/* something else: skip the rest */
}


static void sgr (Vt *vt) {
  Cell *pen = &vt->g->pen;
  int i;
  if (vt->nparams == 0) {
    vt->params[0] = 0;
    vt->nparams = 1;
  }
  for (i = 0; i < vt->nparams; i++) {
    int p = vt->params[i];
    if (vt->sub[i]) continue;	/* 4:3 and friends: the main value is enough */
    switch (p) {
      case 0: pen->attr = 0; pen->fg = pen->bg = pen->ul = COL_DEFAULT; break;
      case 1: pen->attr |= A_BOLD; break;
      case 2: pen->attr |= A_DIM; break;
      case 3: pen->attr |= A_ITALIC; break;
      case 4: {	/* 4:0 none, 4:1 single, 4:2 double, 4:3 curly, 4:4 dotted, 4:5 dashed */
        int st = (i + 1 < vt->nparams && vt->sub[i + 1]) ? vt->params[i + 1] : 1;
        pen->attr &= (uint16_t)~(A_UNDER | A_ULSTYLE);
        if (st >= 1 && st <= 5) pen->attr |= (uint16_t)(A_UNDER | ((st - 1) << UL_SHIFT));
        break;
      }
      case 5: case 6: pen->attr |= A_BLINK; break;
      case 7: pen->attr |= A_REVERSE; break;
      case 8: pen->attr |= A_HIDDEN; break;
      case 9: pen->attr |= A_STRIKE; break;
      case 21:	/* double underline */
        pen->attr &= (uint16_t)~A_ULSTYLE;
        pen->attr |= (uint16_t)(A_UNDER | (1 << UL_SHIFT));
        break;
      case 22: pen->attr &= (uint16_t)~(A_BOLD | A_DIM); break;
      case 23: pen->attr &= (uint16_t)~A_ITALIC; break;
      case 24: pen->attr &= (uint16_t)~(A_UNDER | A_ULSTYLE); break;
      case 25: pen->attr &= (uint16_t)~A_BLINK; break;
      case 53: pen->attr |= A_OVER; break;
      case 55: pen->attr &= (uint16_t)~A_OVER; break;
      case 27: pen->attr &= (uint16_t)~A_REVERSE; break;
      case 28: pen->attr &= (uint16_t)~A_HIDDEN; break;
      case 29: pen->attr &= (uint16_t)~A_STRIKE; break;
      case 38: i = extended_color(vt, i, &pen->fg); break;
      case 48: i = extended_color(vt, i, &pen->bg); break;
      case 58: i = extended_color(vt, i, &pen->ul); break;	/* underline color */
      case 59: pen->ul = COL_DEFAULT; break;
      case 39: pen->fg = COL_DEFAULT; break;
      case 49: pen->bg = COL_DEFAULT; break;
      default:
        if (p >= 30 && p <= 37) pen->fg = COL_IDX(p - 30);
        else if (p >= 40 && p <= 47) pen->bg = COL_IDX(p - 40);
        else if (p >= 90 && p <= 97) pen->fg = COL_IDX(p - 90 + 8);
        else if (p >= 100 && p <= 107) pen->bg = COL_IDX(p - 100 + 8);
        break;
    }
  }
}


static void set_mode (Vt *vt, int on) {
  Grid *g = vt->g;
  int i;
  for (i = 0; i < vt->nparams; i++) {
    int p = vt->params[i];
    if (vt->priv == '?') {
      switch (p) {
        case 1: g->app_cursor = on; break;
        case 7: g->autowrap = on; break;
        case 25:
          g->cursor_on = on;
          g->screen[g->cy].dirty = 1;
          break;
        case 47: case 1047: grid_set_alt(g, on, on); break;
        case 1048:
          if (on) grid_save_cursor(g);
          else grid_restore_cursor(g);
          break;
        case 1049:
          if (on) {
            grid_save_cursor(g);
            grid_set_alt(g, 1, 1);
          }
          else {
            grid_set_alt(g, 0, 0);
            grid_restore_cursor(g);
          }
          break;
        case 1004: g->focus_events = on; break;
        case 2004: g->bracketed = on; break;
        /* the mouse: the program asks for clicks (1000), drags (1002),
        ** every move (1003), and 1006 for reports it can read as text */
        case 9: case 1000: case 1002: case 1003:
          g->mouse = on ? p : 0;
          break;
        case 1005: case 1015: break;	/* other encodings: SGR is enough */
        case 1006: g->mouse_sgr = on; break;
        case 2026: g->sync = on; break;	/* synchronized output */
        case 6:	/* DECOM: positions inside the scroll region */
          g->origin = on;
          grid_move(g, 0, on ? g->top : 0);
          break;
        default: break;	/* 9001 (win32 input) ...: not supported */
      }
    }
    else if (vt->priv == 0 && p == 4) g->insert = on;
  }
}


/* DECRQM: is mode p set? 1 yes, 2 no, 0 not known here */
static int mode_state (const Vt *vt, int p) {
  const Grid *g = vt->g;
  if (vt->priv != '?') return (p == 4) ? (g->insert ? 1 : 2) : 0;
  switch (p) {
    case 1: return g->app_cursor ? 1 : 2;
    case 7: return g->autowrap ? 1 : 2;
    case 25: return g->cursor_on ? 1 : 2;
    case 47: case 1047: case 1049: return g->alt ? 1 : 2;
    case 1004: return g->focus_events ? 1 : 2;
    case 2004: return g->bracketed ? 1 : 2;
    case 9: case 1000: case 1002: case 1003: return g->mouse == p ? 1 : 2;
    case 1006: return g->mouse_sgr ? 1 : 2;
    case 2026: return g->sync ? 1 : 2;
    case 6: return g->origin ? 1 : 2;
    default: return 0;
  }
}


/* DECSTR: the modes back to their start, the screen stays as it is */
static void soft_reset (Vt *vt) {
  Grid *g = vt->g;
  g->cursor_on = g->autowrap = 1;
  g->app_cursor = g->insert = g->sync = g->origin = 0;
  g->top = 0;
  g->bot = g->rows - 1;
  g->pen.attr = 0;
  g->pen.fg = g->pen.bg = COL_DEFAULT;
  g->pen.link = 0;
  g->pen.ul = COL_DEFAULT;
  g->saved_cx[g->alt] = g->saved_cy[g->alt] = 0;
  g->saved_pen[g->alt] = g->pen;
  vt->charset[0] = vt->charset[1] = vt->gl = 0;
  g->screen[g->cy].dirty = 1;
}


/* row n (from 1) of CUP and VPA: in origin mode inside the scroll region */
static int row_of (const Grid *g, int n) {
  int y = n - 1;
  if (!g->origin) return y;
  y += g->top;
  return y > g->bot ? g->bot : y;
}


/*
** The kitty keyboard protocol: CSI ? u asks for the flags, CSI > f u
** pushes, CSI < n u pops, CSI = f ; m u sets (m: 1 set, 2 add, 3 take
** away). Each screen has its own stack.
*/
static void kitty_keys (Vt *vt) {
  Grid *g = vt->g;
  int *st = g->kitty[g->alt], *n = &g->kitty_n[g->alt], cur = grid_kitty(g);
  char buf[32];
  switch (vt->priv) {
    case '?':
      sprintf(buf, "\033[?%du", cur);
      reply(vt, buf);
      break;
    case '>':
      if (*n == 8) {	/* full: the oldest goes */
        memmove(st, st + 1, 7 * sizeof(int));
        (*n)--;
      }
      st[(*n)++] = (vt->nparams > 0 ? vt->params[0] : 0) & 31;
      break;
    case '<': {
      int k = param(vt, 0, 1);
      *n = (k >= *n) ? 0 : *n - k;
      break;
    }
    case '=': {
      int f = (vt->nparams > 0 ? vt->params[0] : 0) & 31, m = param(vt, 1, 1);
      cur = (m == 2) ? (cur | f) : (m == 3) ? (cur & ~f) : f;
      if (*n == 0) st[(*n)++] = cur;
      else st[*n - 1] = cur;
      break;
    }
    default: break;
  }
}


static void csi (Vt *vt, int final) {
  Grid *g = vt->g;
  char buf[64];
  int n = param(vt, 0, 1);
  int lo, hi;
  if (vt->inter == ' ' && final == 'q') {	/* DECSCUSR: cursor shape */
    g->cursor_shape = param(vt, 0, 0);
    g->screen[g->cy].dirty = 1;
    return;
  }
  if (vt->inter == '$' && final == 'p') {	/* DECRQM: a program asks about a mode */
    int p = param(vt, 0, 0);
    sprintf(buf, "\033[%s%d;%d$y", vt->priv == '?' ? "?" : "", p, mode_state(vt, p));
    reply(vt, buf);
    return;
  }
  if (vt->inter == '!' && final == 'p') {
    soft_reset(vt);
    return;
  }
  if (vt->inter != 0) return;
  if (final == 'S' && vt->priv == '?') {	/* XTSMGRAPHICS: what images can be */
    int what = param(vt, 0, 0);
    if (what == 1) reply(vt, "\033[?1;0;256S");	/* color registers */
    else if (what == 2) {	/* the most pixels: the screen */
      sprintf(buf, "\033[?2;0;%d;%dS", g->cols * g->cell_w, g->rows * g->cell_h);
      reply(vt, buf);
    }
    else {
      sprintf(buf, "\033[?%d;1;0S", what);
      reply(vt, buf);
    }
    return;
  }
  if (final == 'u' && vt->priv != 0) {	/* the kitty keyboard protocol */
    kitty_keys(vt);
    return;
  }
  if (vt->priv == '>' && final == 'q') {	/* XTVERSION: which terminal is this? */
    reply(vt, "\033P>|" TERM_NAME " " MMC_VERSION "\033\\");
    return;
  }
  if (vt->priv != 0 && strchr("hlcnJK", final) == NULL) return;
  lo = (g->cy >= g->top) ? g->top : 0;	/* margins stop the cursor */
  hi = (g->cy <= g->bot) ? g->bot : g->rows - 1;
  switch (final) {
    case '@': grid_insert_chars(g, n); break;
    case 'A': grid_move(g, g->cx, (g->cy - n < lo) ? lo : g->cy - n); break;
    case 'B': case 'e':
      grid_move(g, g->cx, (g->cy + n > hi) ? hi : g->cy + n);
      break;
    case 'C': case 'a': grid_move(g, g->cx + n, g->cy); break;
    case 'D': grid_move(g, g->cx - n, g->cy); break;
    case 'E': grid_move(g, 0, (g->cy + n > hi) ? hi : g->cy + n); break;
    case 'F': grid_move(g, 0, (g->cy - n < lo) ? lo : g->cy - n); break;
    case 'G': case '`': grid_move(g, n - 1, g->cy); break;
    case 'H': case 'f': grid_move(g, param(vt, 1, 1) - 1, row_of(g, n)); break;
    case 'I': grid_tab(g, n); break;
    case 'J': grid_erase_display(g, param(vt, 0, 0)); break;
    case 'K': grid_erase_line(g, param(vt, 0, 0)); break;
    case 'L': grid_insert_lines(g, n); break;
    case 'M': grid_delete_lines(g, n); break;
    case 'P': grid_delete_chars(g, n); break;
    case 'S': grid_scroll_up(g, n); break;
    case 'T': grid_scroll_down(g, n); break;
    case 'X': grid_erase_chars(g, n); break;
    case 'Z': grid_tab(g, -n); break;
    case 'b':
      if (vt->last != 0)
        for (; n > 0; n--) grid_putc(g, vt->last);
      break;
    case 'c':
      if (vt->priv == '>') reply(vt, "\033[>0;10;1c");
      else if (param(vt, 0, 0) == 0) reply(vt, "\033[?62;4;22c");	/* VT220, sixel, color */
      break;
    case 'd': grid_move(g, g->cx, row_of(g, n)); break;
    case 'g':
      if (param(vt, 0, 0) == 0) g->tabs[g->cx] = 0;
      else if (param(vt, 0, 0) == 3) memset(g->tabs, 0, (size_t)g->cols);
      break;
    case 'h': set_mode(vt, 1); break;
    case 'l': set_mode(vt, 0); break;
    case 'm': sgr(vt); break;
    case 'n':
      if (param(vt, 0, 0) == 5) reply(vt, "\033[0n");
      else if (param(vt, 0, 0) == 6) {
        sprintf(buf, "\033[%d;%dR", g->cy + 1 - (g->origin ? g->top : 0), g->cx + 1);
        reply(vt, buf);
      }
      break;
    case 'r': grid_set_region(g, n - 1, param(vt, 1, g->rows) - 1); break;
    case 's': grid_save_cursor(g); break;
    case 'u': grid_restore_cursor(g); break;
    default: break;
  }
}

/* }================================================================== */


static void osc_end (Vt *vt) {
  const char *s = vt->osc.s ? vt->osc.s : "";
  const char *semi = strchr(s, ';');
  int code = atoi(s);
  if ((s[0] == '0' || s[0] == '1' || s[0] == '2') && s[1] == ';') {
    if (s[0] != '1' && vt->title) vt->title(vt->ud, s + 2);	/* 1 is the icon name */
  }
  else if (code == 8 && semi != NULL) {	/* 8;params;uri: a link starts, "" ends it */
    const char *uri = strchr(semi + 1, ';');
    vt->g->pen.link = 0;
    if (uri != NULL && uri[1] != '\0') vt->g->pen.link = (uint16_t)grid_link_add(vt->g, uri + 1);
  }
  else if (semi != NULL && vt->on_osc != NULL &&
           (code == 4 || code == 7 || code == 10 || code == 11 || code == 12 || code == 52))
    vt->on_osc(vt->ud, code, semi + 1);
  buf_free(&vt->osc);
}


static void escape (Vt *vt, int c) {
  Grid *g = vt->g;
  vt->state = S_GROUND;
  switch (c) {
    case '[':
      vt->state = S_CSI;
      vt->nparams = vt->has_digit = 0;
      vt->priv = vt->inter = 0;
      memset(vt->params, 0, sizeof(vt->params));
      memset(vt->sub, 0, sizeof(vt->sub));
      break;
    case ']':
      vt->state = S_OSC;
      buf_free(&vt->osc);
      break;
    case 'P':	/* DCS: kept, it may be a sixel image */
      vt->state = S_STRING;
      vt->in_dcs = 1;
      buf_free(&vt->dcs);
      break;
    case 'X': case '^': case '_':
      vt->state = S_STRING;
      vt->in_dcs = 0;
      break;
    case '(': case ')': vt->state = S_CHARSET; vt->inter = (char)c; break;
    case '*': case '+': case '#': case '%': case ' ':
      vt->state = S_ESC_SKIP;
      break;
    case '7': grid_save_cursor(g); break;
    case '8': grid_restore_cursor(g); break;
    case 'D': grid_lf(g); break;
    case 'E': grid_cr(g); grid_lf(g); break;
    case 'H': g->tabs[g->cx] = 1; break;
    case 'M': grid_ri(g); break;
    case 'c':
      grid_reset(g);
      vt->charset[0] = vt->charset[1] = vt->gl = 0;
      break;
    default: break;	/* = > \ and the rest: nothing to do */
  }
}


static void csi_byte (Vt *vt, int c) {
  if (c >= '0' && c <= '9') {
    if (vt->nparams == 0) vt->nparams = 1;
    if (vt->nparams <= VT_MAX_PARAMS) {
      int *p = &vt->params[vt->nparams - 1];
      if (*p < 100000) *p = *p * 10 + (c - '0');
    }
    vt->has_digit = 1;
  }
  else if (c == ';' || c == ':') {
    if (vt->nparams == 0) vt->nparams = 1;
    if (vt->nparams < VT_MAX_PARAMS) {
      vt->sub[vt->nparams] = (c == ':');
      vt->nparams++;
    }
  }
  else if (c >= 0x3C && c <= 0x3F) {
    if (vt->nparams == 0 && !vt->has_digit) vt->priv = (char)c;
  }
  else if (c >= 0x20 && c <= 0x2F) vt->inter = (char)c;
  else if (c >= 0x40 && c <= 0x7E) {
    vt->state = S_GROUND;
    csi(vt, c);
  }
  else vt->state = S_GROUND;	/* garbage: give up on this sequence */
}


static void ground_byte (Vt *vt, unsigned char c) {
  if (vt->u8need > 0) {
    if ((c & 0xC0) == 0x80) {
      vt->u8cp = (vt->u8cp << 6) | (c & 0x3F);
      if (--vt->u8need == 0) print(vt, vt->u8cp);
      return;
    }
    vt->u8need = 0;	/* broken sequence */
    print(vt, 0xFFFD);
  }
  if (c < 0x80) print(vt, c);
  else if ((c & 0xE0) == 0xC0) { vt->u8cp = c & 0x1F; vt->u8need = 1; }
  else if ((c & 0xF0) == 0xE0) { vt->u8cp = c & 0x0F; vt->u8need = 2; }
  else if ((c & 0xF8) == 0xF0) { vt->u8cp = c & 0x07; vt->u8need = 3; }
  else print(vt, 0xFFFD);
}


/*
** {==================================================================
** Sixel: bands six pixels high; a character '?'..'~' is one column of
** a band, #n picks (or defines) a color, !n repeats, $ goes back to the
** band's start, - to the next band
** ===================================================================
*/

#define SIXEL_MAX	4096	/* pixels, each way */

typedef struct Canvas {
  uint32_t *px;
  int w, h, cap_w, cap_h;	/* w, h: as far as pixels were set */
} Canvas;


static int canvas_room (Canvas *c, int w, int h) {
  if (w > SIXEL_MAX || h > SIXEL_MAX) return 0;
  if (w > c->cap_w || h > c->cap_h) {
    int nw = c->cap_w, nh = c->cap_h, y;
    uint32_t *n;
    while (nw < w) nw = nw ? nw * 2 : 256;
    while (nh < h) nh = nh ? nh * 2 : 96;
    if (nw > SIXEL_MAX) nw = SIXEL_MAX;
    if (nh > SIXEL_MAX) nh = SIXEL_MAX;
    n = (uint32_t *)xmalloc((size_t)nw * (size_t)nh * sizeof(uint32_t));
    memset(n, 0, (size_t)nw * (size_t)nh * sizeof(uint32_t));
    for (y = 0; y < c->cap_h; y++)
      memcpy(n + (size_t)y * (size_t)nw, c->px + (size_t)y * (size_t)c->cap_w,
             (size_t)c->cap_w * sizeof(uint32_t));
    free(c->px);
    c->px = n;
    c->cap_w = nw;
    c->cap_h = nh;
  }
  return 1;
}


static uint32_t hls_rgb (int h, int l, int s) {	/* sixel HLS: 0 degrees is blue */
  double hh = fmod((double)h + 240.0, 360.0) / 60.0, ll = l / 100.0, ss = s / 100.0;
  double c = (1.0 - fabs(2.0 * ll - 1.0)) * ss, x = c * (1.0 - fabs(fmod(hh, 2.0) - 1.0));
  double m = ll - c / 2.0, r = 0, g = 0, b = 0;
  if (hh < 1) { r = c; g = x; }
  else if (hh < 2) { r = x; g = c; }
  else if (hh < 3) { g = c; b = x; }
  else if (hh < 4) { g = x; b = c; }
  else if (hh < 5) { r = x; b = c; }
  else { r = c; b = x; }
  return ((uint32_t)((r + m) * 255.0 + 0.5) << 16) | ((uint32_t)((g + m) * 255.0 + 0.5) << 8) |
         (uint32_t)((b + m) * 255.0 + 0.5);
}


/* numbers after an introducer: up to 'max' of them, ';' between */
static size_t sixel_nums (const char *d, size_t i, size_t n, int *out, int max, int *count) {
  int k = 0;
  *count = 0;
  out[0] = 0;
  while (i < n && ((d[i] >= '0' && d[i] <= '9') || d[i] == ';')) {
    if (d[i] == ';') {
      if (k + 1 < max) out[++k] = 0;
    }
    else if (out[k] < 100000) out[k] = out[k] * 10 + (d[i] - '0');
    *count = k + 1;
    i++;
  }
  return i;
}


static void sixel (Vt *vt, const char *d, size_t n) {
  static const uint32_t vt340[16] = {
    0x000000, 0x3333CC, 0xCC2121, 0x33CC33, 0xCC33CC, 0x33CCCC, 0xCCCC33, 0x878787,
    0x424242, 0x545499, 0x994242, 0x549954, 0x995499, 0x549999, 0x999954, 0xCCCCCC
  };
  uint32_t pal[256];
  Canvas cv;
  int x = 0, y = 0, color = 0, rep = 1, i, raster_w = 0, raster_h = 0;
  size_t p = 0;
  memset(&cv, 0, sizeof(cv));
  for (i = 0; i < 256; i++) pal[i] = i < 16 ? vt340[i] : 0;
  while (p < n) {
    char c = d[p];
    if (c >= '?' && c <= '~') {
      int bits = c - '?', k, b;
      if (canvas_room(&cv, x + rep, y + 6)) {
        for (k = 0; k < rep; k++)
          for (b = 0; b < 6; b++)
            if (bits & (1 << b)) {
              cv.px[(size_t)(y + b) * (size_t)cv.cap_w + (size_t)(x + k)] = 0xFF000000u | pal[color];
              if (x + k + 1 > cv.w) cv.w = x + k + 1;
              if (y + b + 1 > cv.h) cv.h = y + b + 1;
            }
      }
      x += rep;
      rep = 1;
      p++;
    }
    else if (c == '!') {
      int v[1], cnt;
      p = sixel_nums(d, p + 1, n, v, 1, &cnt);
      rep = (cnt > 0 && v[0] > 0) ? v[0] : 1;
    }
    else if (c == '#') {
      int v[5], cnt;
      p = sixel_nums(d, p + 1, n, v, 5, &cnt);
      if (cnt < 1) continue;
      color = v[0] & 255;
      if (cnt >= 5 && v[1] == 2)	/* RGB in percent */
        pal[color] = ((uint32_t)(v[2] * 255 / 100) << 16) | ((uint32_t)(v[3] * 255 / 100) << 8) |
                     (uint32_t)(v[4] * 255 / 100);
      else if (cnt >= 5 && v[1] == 1) pal[color] = hls_rgb(v[2], v[3], v[4]);
    }
    else if (c == '"') {	/* raster attributes: aspect, then the size */
      int v[4], cnt;
      p = sixel_nums(d, p + 1, n, v, 4, &cnt);
      if (cnt >= 4) {
        raster_w = v[2] > SIXEL_MAX ? SIXEL_MAX : v[2];
        raster_h = v[3] > SIXEL_MAX ? SIXEL_MAX : v[3];
        canvas_room(&cv, raster_w, raster_h);
      }
    }
    else if (c == '$') {
      x = 0;
      p++;
    }
    else if (c == '-') {
      x = 0;
      y += 6;
      p++;
    }
    else p++;
  }
  if (raster_w > cv.w && raster_w <= cv.cap_w) cv.w = raster_w;
  if (raster_h > cv.h && raster_h <= cv.cap_h) cv.h = raster_h;
  if (cv.w > 0 && cv.h > 0) {	/* the image, exactly its size */
    uint32_t *img = (uint32_t *)xmalloc((size_t)cv.w * (size_t)cv.h * sizeof(uint32_t));
    int row;
    for (row = 0; row < cv.h; row++)
      memcpy(img + (size_t)row * (size_t)cv.w, cv.px + (size_t)row * (size_t)cv.cap_w,
             (size_t)cv.w * sizeof(uint32_t));
    grid_put_image(vt->g, img, cv.w, cv.h);
  }
  free(cv.px);
}


/* a DCS string ended: sixel (P...q) is the one we know */
static void dcs_end (Vt *vt) {
  const char *s = vt->dcs.s ? vt->dcs.s : "";
  size_t i = 0, n = vt->dcs.len;
  while (i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == ';')) i++;
  if (i < n && s[i] == 'q') sixel(vt, s + i + 1, n - i - 1);
  buf_free(&vt->dcs);
}

/* }================================================================== */


void vt_feed (Vt *vt, const char *buf, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)buf[i];
    switch (vt->state) {
      case S_OSC:
        if (c == 0x07) { osc_end(vt); vt->state = S_GROUND; }
        else if (c == 0x1B) vt->state = S_OSC_ESC;
        else if (vt->osc.len < 4096) buf_putc(&vt->osc, (char)c);
        continue;
      case S_OSC_ESC:
        osc_end(vt);
        vt->state = S_GROUND;
        if (c != '\\') escape(vt, c);
        continue;
      case S_STRING:
        if (c == 0x1B) vt->state = S_STRING_ESC;
        else if (c == 0x07) {
          vt->state = S_GROUND;
          if (vt->in_dcs) dcs_end(vt);
        }
        else if (vt->in_dcs && vt->dcs.len < ((size_t)32 << 20)) buf_putc(&vt->dcs, (char)c);
        continue;
      case S_STRING_ESC:
        vt->state = (c == '\\') ? S_GROUND : S_STRING;
        if (vt->state == S_GROUND && vt->in_dcs) dcs_end(vt);
        continue;
      default: break;
    }
    if (c == 0x1B) {	/* ESC restarts whatever was going on */
      vt->state = S_ESC;
      vt->u8need = 0;
      continue;
    }
    if (c < 0x20 || c == 0x7F) {
      if (c == 0x18 || c == 0x1A) vt->state = S_GROUND;	/* CAN, SUB */
      else control(vt, c);
      continue;
    }
    switch (vt->state) {
      case S_GROUND: ground_byte(vt, c); break;
      case S_ESC: escape(vt, c); break;
      case S_ESC_SKIP: vt->state = S_GROUND; break;
      case S_CHARSET:
        vt->charset[vt->inter == ')'] = (c == '0');
        vt->state = S_GROUND;
        break;
      case S_CSI: csi_byte(vt, c); break;
      default: vt->state = S_GROUND; break;
    }
  }
}
