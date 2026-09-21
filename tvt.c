/*
** tvt.c - escape sequence parser of mmc-term
**
** Bytes from the program go in; calls on the Grid come out. Speaks the
** xterm dialect that ConPTY, ncurses programs and editors use.
*/

#include "mterm.h"

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
      case 0: pen->attr = 0; pen->fg = pen->bg = COL_DEFAULT; break;
      case 1: pen->attr |= A_BOLD; break;
      case 2: pen->attr |= A_DIM; break;
      case 3: pen->attr |= A_ITALIC; break;
      case 4:
        if (i + 1 < vt->nparams && vt->sub[i + 1] && vt->params[i + 1] == 0)
          pen->attr &= (uint16_t)~A_UNDER;
        else pen->attr |= A_UNDER;
        break;
      case 7: pen->attr |= A_REVERSE; break;
      case 8: pen->attr |= A_HIDDEN; break;
      case 9: pen->attr |= A_STRIKE; break;
      case 21: pen->attr |= A_UNDER; break;
      case 22: pen->attr &= (uint16_t)~(A_BOLD | A_DIM); break;
      case 23: pen->attr &= (uint16_t)~A_ITALIC; break;
      case 24: pen->attr &= (uint16_t)~A_UNDER; break;
      case 27: pen->attr &= (uint16_t)~A_REVERSE; break;
      case 28: pen->attr &= (uint16_t)~A_HIDDEN; break;
      case 29: pen->attr &= (uint16_t)~A_STRIKE; break;
      case 38: i = extended_color(vt, i, &pen->fg); break;
      case 48: i = extended_color(vt, i, &pen->bg); break;
      case 58: {	/* underline color: parsed, not used */
        uint32_t unused = 0;
        i = extended_color(vt, i, &unused);
        break;
      }
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
        default: break;	/* 9001 (win32 input) ...: not supported */
      }
    }
    else if (vt->priv == 0 && p == 4) g->insert = on;
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
  if (vt->inter != 0) return;
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
    case 'H': case 'f': grid_move(g, param(vt, 1, 1) - 1, n - 1); break;
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
      else if (param(vt, 0, 0) == 0) reply(vt, "\033[?1;2c");
      break;
    case 'd': grid_move(g, g->cx, n - 1); break;
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
        sprintf(buf, "\033[%d;%dR", g->cy + 1, g->cx + 1);
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
    case 'P': case 'X': case '^': case '_': vt->state = S_STRING; break;
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
        else if (c == 0x07) vt->state = S_GROUND;
        continue;
      case S_STRING_ESC:
        vt->state = (c == '\\') ? S_GROUND : S_STRING;
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
