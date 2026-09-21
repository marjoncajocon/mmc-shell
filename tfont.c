/*
** tfont.c - fonts of mmc-term
**
** Finds a monospace font file on the system and caches its glyphs.
** Missing glyphs come from fallback fonts; bold and italic are
** synthesized when the font has no file for them.
**
** Two rasterizers:
**   stb_truetype   every system; unhinted, the same picture everywhere
**   GDI            Windows (font_smoothing=cleartype|gray, the default):
**                  hinted and ClearType filtered like every other Windows
**                  program - this is what makes git-bash's mintty sharp
**                  and smooth at small sizes. stb_truetype still decides
**                  which font file has a glyph, GDI only draws it.
**
** Small text gets two helps on top (both fade out as the size grows):
**   x-height snap  stb only: the vertical scale is nudged so the tops of
**                  the lowercase letters land on a pixel edge instead of
**                  smearing over two rows - FreeType's "light" hinting
**   darkening      thin strokes are made a little heavier, the way macOS
**                  and DirectWrite do it, so they don't fade into gray
*/

#include "mterm.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif


#define ST_BOLD		1
#define ST_ITALIC	2

typedef struct Face {
  unsigned char *data;	/* the font file; shared by faces of one .ttc */
  size_t len;	/* bytes in data */
  stbtt_fontinfo info;
  OtFace ot;	/* ligatures, colors: what stb_truetype does not read */
  float scale;	/* horizontal: the em size, untouched */
  float scale_y;	/* vertical: x-height snapped to whole pixels */
  int ok;
#ifdef _WIN32
  wchar_t family[LF_FACESIZE];	/* name GDI knows the font by */
  HFONT hf[4];	/* GDI font per ST_* style, for the current size */
#endif
} Face;

typedef struct Known {
  const char *name, *regular, *bold, *italic;
  int ttc_bold, ttc_italic;	/* face index inside a .ttc, 0 = none */
} Known;

/* a name may appear twice: the first entry whose file exists wins */
static const Known known[] = {
  {"JetBrains Mono", "JetBrainsMonoNerdFontMono-Regular.ttf",
   "JetBrainsMonoNerdFontMono-Bold.ttf", "JetBrainsMonoNerdFontMono-Italic.ttf", 0, 0},
  {"JetBrains Mono Nerd Font Mono", "JetBrainsMonoNerdFontMono-Regular.ttf",
   "JetBrainsMonoNerdFontMono-Bold.ttf", "JetBrainsMonoNerdFontMono-Italic.ttf", 0, 0},
  {"Hack", "HackNerdFontMono-Regular.ttf", "HackNerdFontMono-Bold.ttf",
   "HackNerdFontMono-Italic.ttf", 0, 0},
  {"Hack", "Hack-Regular.ttf", "Hack-Bold.ttf", "Hack-Italic.ttf", 0, 0},
  {"Hack Nerd Font Mono", "HackNerdFontMono-Regular.ttf",
   "HackNerdFontMono-Bold.ttf", "HackNerdFontMono-Italic.ttf", 0, 0},
  {"Cascadia Mono", "CascadiaMono.ttf", NULL, NULL, 0, 0},
  {"Cascadia Code", "CascadiaCode.ttf", NULL, NULL, 0, 0},
  {"Consolas", "consola.ttf", "consolab.ttf", "consolai.ttf", 0, 0},
  {"JetBrains Mono", "JetBrainsMono-Regular.ttf", "JetBrainsMono-Bold.ttf",
   "JetBrainsMono-Italic.ttf", 0, 0},
  {"DejaVu Sans Mono", "DejaVuSansMono.ttf", "DejaVuSansMono-Bold.ttf",
   "DejaVuSansMono-Oblique.ttf", 0, 0},
  {"Liberation Mono", "LiberationMono-Regular.ttf", "LiberationMono-Bold.ttf",
   "LiberationMono-Italic.ttf", 0, 0},
  {"Noto Sans Mono", "NotoSansMono-Regular.ttf", "NotoSansMono-Bold.ttf",
   NULL, 0, 0},
  {"Ubuntu Mono", "UbuntuMono-R.ttf", "UbuntuMono-B.ttf", "UbuntuMono-RI.ttf",
   0, 0},
  {"Menlo", "Menlo.ttc", NULL, NULL, 1, 2},
  {"SF Mono", "SFNSMono.ttf", NULL, "SFNSMonoItalic.ttf", 0, 0},
  {"Monaco", "Monaco.ttf", NULL, NULL, 0, 0},
  {"Courier New", "cour.ttf", "courbd.ttf", "couri.ttf", 0, 0},
  {"Lucida Console", "lucon.ttf", NULL, NULL, 0, 0},
  {NULL, NULL, NULL, NULL, 0, 0}
};

/* looked at in this order when the config names no font; JetBrains Mono
** (the Nerd Font version, with ligatures and the Powerline and icon
** glyphs) comes with mmc in usr/share/fonts, the others are what each
** system has */
static const char *const preferred[] = {
  "JetBrains Mono", "Hack",
#if defined(_WIN32)
  "Cascadia Mono", "Cascadia Code", "Consolas", "Lucida Console",
#elif defined(__APPLE__)
  "Menlo", "SF Mono", "Monaco",
#else
  "DejaVu Sans Mono", "Liberation Mono", "Noto Sans Mono", "Ubuntu Mono",
#endif
  "Courier New", NULL
};

/* fonts asked for glyphs the main font does not have; the Nerd Font
** first, so the icons work whatever the main font is */
static const char *const fallback_files[] = {
  "JetBrainsMonoNerdFontMono-Regular.ttf",
#if defined(_WIN32)
  "seguisym.ttf", "segoeui.ttf", "msgothic.ttc", "malgun.ttf", "msyh.ttc",
  "seguiemj.ttf",
#elif defined(__APPLE__)
  "Apple Symbols.ttf", "PingFang.ttc", "Hiragino Sans GB.ttc",
  "Arial Unicode.ttf",
#else
  "DejaVuSans.ttf", "NotoSansSymbols2-Regular.ttf", "NotoSansCJK-Regular.ttc",
  "unifont.ttf",
#endif
  NULL
};

#define MAX_FALLBACK	8

/* color emoji: COLR layers (Windows), CBDT pictures (Linux), sbix
** (macOS); the first one there with colors is used */
static const char *const emoji_files[] = {
#if defined(_WIN32)
  "seguiemj.ttf",
#elif defined(__APPLE__)
  "Apple Color Emoji.ttc",
#else
  "NotoColorEmoji.ttf", "NotoColorEmoji-Regular.ttf", "Twemoji.Mozilla.ttf",
#endif
  NULL
};

static Vec font_files;	/* every font file found on the system */
static int files_listed = 0;
static char *extra_dir = NULL;
static Face f_regular, f_bold, f_italic;
static Face f_fallback[MAX_FALLBACK];
static int fallback_state[MAX_FALLBACK];	/* 0 not tried, 1 loaded, -1 none */
static Face f_emoji;
static int emoji_state = 0;	/* 0 not tried, 1 loaded, -1 none */
static char name_buf[160];
static float cur_px = 15.0f;
static int cell_w = 8, cell_h = 16, ascent = 12;
static int smoothing = SMOOTH_STB;
static int use_ligatures = 1;
static float emoji_scale = 0.0f;	/* COLR emoji, for this cell size; 0: not yet */


/*
** {==================================================================
** Finding font files
** ===================================================================
*/

static int has_font_ext (const char *name) {
  size_t n = strlen(name);
  return n > 4 && (m_stricmp(name + n - 4, ".ttf") == 0 ||
                   m_stricmp(name + n - 4, ".ttc") == 0 ||
                   m_stricmp(name + n - 4, ".otf") == 0);
}


static void list_dir (const char *dir, int depth) {
  Vec names;
  size_t i;
  vec_init(&names);
  if (os_listdir(dir, &names) == 0) {
    for (i = 0; i < names.n; i++) {
      char *full = path_join(dir, names.v[i]);
      OsStat st;
      if (has_font_ext(names.v[i])) vec_push(&font_files, full);
      else {
        if (depth < 4 && os_stat(full, &st) == 0 && st.is_dir)
          list_dir(full, depth + 1);
        free(full);
      }
    }
  }
  vec_free(&names);
}


static void list_env_dir (const char *var, const char *sub) {
  char *base = os_getenv(var);
  if (base != NULL) {
    char *dir = path_join(base, sub);
    list_dir(dir, 0);
    free(dir);
    free(base);
  }
}


static void list_fonts (void) {
  if (files_listed) return;
  files_listed = 1;
  vec_init(&font_files);
  if (extra_dir) list_dir(extra_dir, 0);	/* fonts carried with mmc win */
#if defined(_WIN32)
  list_env_dir("LOCALAPPDATA", "Microsoft\\Windows\\Fonts");
  list_env_dir("WINDIR", "Fonts");
#elif defined(__APPLE__)
  list_env_dir("HOME", "Library/Fonts");
  list_dir("/Library/Fonts", 0);
  list_dir("/System/Library/Fonts", 0);
#else
  list_env_dir("HOME", ".local/share/fonts");
  list_env_dir("HOME", ".fonts");
  list_dir("/usr/local/share/fonts", 0);
  list_dir("/usr/share/fonts", 0);
#endif
}


static const char *find_file (const char *basename) {
  size_t i;
  if (basename == NULL) return NULL;
  list_fonts();
  for (i = 0; i < font_files.n; i++)
    if (m_stricmp(path_basename(font_files.v[i]), basename) == 0)
      return font_files.v[i];
  return NULL;
}


void font_add_dir (const char *native) {
  free(extra_dir);
  extra_dir = xstrdup(native);
}

/* }================================================================== */


#ifdef _WIN32

/*
** {==================================================================
** GDI: hinted, ClearType (or gray) glyphs, the way Windows draws text
** ===================================================================
*/

static HDC gdc = NULL;
static HBITMAP gbmp = NULL;
static uint32_t *gbits = NULL;
static int gw = 0, gh = 0;


static int use_gdi (void) {
  return smoothing != SMOOTH_STB;
}


/* the family name from the font's own 'name' table (UTF-16 big endian) */
static void face_family (Face *f) {
  int len = 0, i, n;
  const char *s;
  f->family[0] = L'\0';
  if (!f->ok) return;
  s = stbtt_GetFontNameString(&f->info, &len, STBTT_PLATFORM_ID_MICROSOFT,
                              STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH, 1);
  if (s == NULL) return;
  n = len / 2;
  if (n > LF_FACESIZE - 1) n = LF_FACESIZE - 1;
  for (i = 0; i < n; i++)
    f->family[i] = (wchar_t)(((unsigned char)s[2 * i] << 8) | (unsigned char)s[2 * i + 1]);
  f->family[n] = L'\0';
}


/* the font file becomes usable by name, for this program only */
static void face_register (Face *f, const char *file) {
  wchar_t w[1024];
  if (MultiByteToWideChar(CP_UTF8, 0, file, -1, w, 1024) > 0)
    AddFontResourceExW(w, FR_PRIVATE, 0);
  face_family(f);
}


static void face_drop_gdi (Face *f) {
  int i;
  for (i = 0; i < 4; i++) {
    if (f->hf[i] != NULL) DeleteObject(f->hf[i]);
    f->hf[i] = NULL;
  }
}


static HFONT face_hfont (Face *f, int style) {
  LOGFONTW lf;
  if (f->hf[style] != NULL) return f->hf[style];
  memset(&lf, 0, sizeof(lf));
  lf.lfHeight = -(LONG)floor(cur_px + 0.5f);	/* negative: the em size */
  lf.lfWeight = (style & ST_BOLD) ? FW_BOLD : FW_NORMAL;
  lf.lfItalic = (style & ST_ITALIC) ? TRUE : FALSE;
  lf.lfCharSet = DEFAULT_CHARSET;
  lf.lfOutPrecision = OUT_TT_ONLY_PRECIS;
  lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
  lf.lfQuality = (smoothing == SMOOTH_GRAY) ? ANTIALIASED_QUALITY : CLEARTYPE_QUALITY;
  lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
  wcsncpy(lf.lfFaceName, f->family, LF_FACESIZE - 1);
  f->hf[style] = CreateFontIndirectW(&lf);
  return f->hf[style];
}


static int canvas (int w, int h) {
  BITMAPINFO bi;
  HBITMAP b;
  void *bits = NULL;
  if (gdc == NULL && (gdc = CreateCompatibleDC(NULL)) == NULL) return 0;
  if (w <= gw && h <= gh) return 1;
  if (w < gw) w = gw;
  if (h < gh) h = gh;
  memset(&bi, 0, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;	/* top row first */
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  b = CreateDIBSection(gdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
  if (b == NULL) return 0;
  SelectObject(gdc, b);
  if (gbmp != NULL) DeleteObject(gbmp);
  gbmp = b;
  gbits = (uint32_t *)bits;
  gw = w;
  gh = h;
  SetBkMode(gdc, TRANSPARENT);
  SetTextAlign(gdc, TA_BASELINE | TA_LEFT | TA_NOUPDATECP);
  return 1;
}


/* cell size from the hinted metrics, so the grid matches what GDI draws */
static int gdi_metrics (void) {
  TEXTMETRICW tm;
  SIZE sz;
  HFONT hf;
  int extra;
  if (!canvas(8, 8) || (hf = face_hfont(&f_regular, 0)) == NULL) return 0;
  SelectObject(gdc, hf);
  if (!GetTextMetricsW(gdc, &tm) || !GetTextExtentPoint32W(gdc, L"M", 1, &sz))
    return 0;
  extra = (tm.tmHeight + 6) / 12;	/* a little air between lines */
  cell_w = sz.cx > 0 ? sz.cx : 1;
  cell_h = tm.tmHeight + extra;
  ascent = tm.tmAscent + extra / 2;
  return 1;
}


/*
** Draws one glyph with GDI and reads the coverage back. Light text is
** drawn white on black, dark text black on white (and inverted): GDI
** tunes ClearType for the colors, so both come out right. gid >= 0: that
** glyph of the font, not a character (a ligature; it may reach several
** cells to the left).
*/
static int gdi_glyph (Face *f, uint32_t cp, int gid, int style, int dark, int span,
                      Glyph *g) {
  wchar_t wc[2];
  int n = 1, pad = cell_h, padx = gid >= 0 ? cell_w * 6 : cell_h;
  int w = cell_w * 2 + 2 * padx, h = cell_h + 2 * pad;
  int x0 = w, y0 = h, x1 = -1, y1 = -1, x, y, x_at;
  uint32_t paper = dark ? 0xFFFFFFu : 0u;
  HFONT hf = face_hfont(f, style);
  if (hf == NULL || !canvas(w, h)) return 0;
  if (gid >= 0) wc[0] = (wchar_t)gid;
  else if (cp >= 0x10000) {	/* UTF-16 surrogate pair */
    wc[0] = (wchar_t)(0xD800 + ((cp - 0x10000) >> 10));
    wc[1] = (wchar_t)(0xDC00 + ((cp - 0x10000) & 0x3FF));
    n = 2;
  }
  else wc[0] = (wchar_t)cp;
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++) gbits[(size_t)y * (size_t)gw + (size_t)x] = paper;
  SelectObject(gdc, hf);
  SetTextColor(gdc, dark ? RGB(0, 0, 0) : RGB(255, 255, 255));
  x_at = padx;
  if (span > 0) {	/* a fallback font is not monospace: center it */
    SIZE sz;
    if (GetTextExtentPoint32W(gdc, wc, n, &sz)) x_at += (span - sz.cx) / 2;
  }
  ExtTextOutW(gdc, x_at, pad + ascent, gid >= 0 ? ETO_GLYPH_INDEX : 0, NULL, wc,
              (UINT)n, NULL);
  GdiFlush();
  for (y = 0; y < h; y++) {
    const uint32_t *row = gbits + (size_t)y * (size_t)gw;
    for (x = 0; x < w; x++) {
      if ((row[x] & 0xFFFFFFu) == paper) continue;
      if (x < x0) x0 = x;
      if (x > x1) x1 = x;
      if (y < y0) y0 = y;
      if (y > y1) y1 = y;
    }
  }
  memset(g, 0, sizeof(*g));
  if (x1 < 0) return 1;	/* a space */
  g->w = x1 - x0 + 1;
  g->h = y1 - y0 + 1;
  g->xoff = x0 - padx;
  g->yoff = y0 - (pad + ascent);
  g->lcd = (smoothing == SMOOTH_CLEARTYPE) ? 1 : 2;
  g->bm = (unsigned char *)xmalloc((size_t)g->w * (size_t)g->h * (g->lcd == 1 ? 3u : 1u));
  for (y = 0; y < g->h; y++) {
    const uint32_t *row = gbits + (size_t)(y0 + y) * (size_t)gw + (size_t)x0;
    for (x = 0; x < g->w; x++) {
      uint32_t p = dark ? ~row[x] : row[x];
      int r = (int)((p >> 16) & 0xFF), gg = (int)((p >> 8) & 0xFF), b = (int)(p & 0xFF);
      size_t at = (size_t)y * (size_t)g->w + (size_t)x;
      if (g->lcd == 1) {
        g->bm[at * 3] = (unsigned char)r;
        g->bm[at * 3 + 1] = (unsigned char)gg;
        g->bm[at * 3 + 2] = (unsigned char)b;
      }
      else g->bm[at] = (unsigned char)((r + gg + gg + b) / 4);
    }
  }
  return 1;
}

/*
** Does GDI draw this style from the very file f was read from? Only then
** are its glyph numbers ours (a family may have an italic file of its
** own that we did not find, with other glyphs). -1: not asked yet.
*/
static int same_file[4] = {-1, -1, -1, -1};

static int gdi_same_file (Face *f, int style) {
  HFONT hf;
  DWORD n;
  if (same_file[style] >= 0) return same_file[style];
  same_file[style] = 0;
  if ((hf = face_hfont(&f_regular, style)) == NULL || !canvas(8, 8)) return 0;
  SelectObject(gdc, hf);
  n = GetFontData(gdc, 0, 0, NULL, 0);
  same_file[style] = (n != GDI_ERROR && (size_t)n == f->len);
  return same_file[style];
}

/* }================================================================== */

#else

static int use_gdi (void) { return 0; }

#endif


/*
** A font of pictures only (Noto Color Emoji has no outlines, which
** stb_truetype wants): enough of it for the cmap and the metrics. Its
** outlines read as empty (a loca format that does not exist).
*/
static int picture_font_init (stbtt_fontinfo *info, unsigned char *data, int off) {
  stbtt_uint32 cmap, maxp;
  int i, n;
  memset(info, 0, sizeof(*info));
  info->data = data;
  info->fontstart = off;
  cmap = stbtt__find_table(data, (stbtt_uint32)off, "cmap");
  info->head = (int)stbtt__find_table(data, (stbtt_uint32)off, "head");
  info->hhea = (int)stbtt__find_table(data, (stbtt_uint32)off, "hhea");
  info->hmtx = (int)stbtt__find_table(data, (stbtt_uint32)off, "hmtx");
  if (!cmap || !info->head || !info->hhea || !info->hmtx) return 0;
  if (!stbtt__find_table(data, (stbtt_uint32)off, "CBDT") &&
      !stbtt__find_table(data, (stbtt_uint32)off, "sbix"))
    return 0;
  maxp = stbtt__find_table(data, (stbtt_uint32)off, "maxp");
  info->numGlyphs = maxp ? ttUSHORT(data + maxp + 4) : 0xFFFF;
  info->svg = -1;
  info->indexToLocFormat = 2;
  n = ttUSHORT(data + cmap + 2);
  for (i = 0; i < n; i++) {	/* Unicode, the full one when there is one */
    stbtt_uint32 rec = cmap + 4 + 8 * (stbtt_uint32)i;
    int pid = ttUSHORT(data + rec), eid = ttUSHORT(data + rec + 2);
    if (pid == 0 || (pid == 3 && (eid == 1 || eid == 10))) {
      info->index_map = (int)(cmap + ttULONG(data + rec + 4));
      if ((pid == 3 && eid == 10) || (pid == 0 && eid >= 4)) break;
    }
  }
  return info->index_map != 0;
}


static int face_open (Face *f, unsigned char *data, size_t len, int index) {
  int off = stbtt_GetFontOffsetForIndex(data, index);
  memset(f, 0, sizeof(*f));
  if (off < 0) return 0;
  if (!stbtt_InitFont(&f->info, data, off) && !picture_font_init(&f->info, data, off))
    return 0;
  f->data = data;
  f->len = len;
  f->ok = 1;
  ot_open(&f->ot, data, len, off);
  return 1;
}


static int face_load (Face *f, const char *file, int index) {
  size_t len = 0;
  unsigned char *data;
  memset(f, 0, sizeof(*f));
  if (file == NULL) return 0;
  data = (unsigned char *)read_file(file, &len);
  if (data == NULL || len < 64) {
    free(data);
    return 0;
  }
  if (!face_open(f, data, len, index)) {
    free(data);
    return 0;
  }
#ifdef _WIN32
  face_register(f, file);
#endif
  return 1;
}


static int load_known (const Known *k) {
  const char *file = find_file(k->regular);
  if (!face_load(&f_regular, file, 0)) return 0;
  if (k->ttc_bold) face_open(&f_bold, f_regular.data, f_regular.len, k->ttc_bold);
  else face_load(&f_bold, find_file(k->bold), 0);
  if (k->ttc_italic) face_open(&f_italic, f_regular.data, f_regular.len, k->ttc_italic);
  else face_load(&f_italic, find_file(k->italic), 0);
#ifdef _WIN32
  if (k->ttc_bold) face_family(&f_bold);	/* faces inside a .ttc */
  if (k->ttc_italic) face_family(&f_italic);
#endif
  strncpy(name_buf, k->name, sizeof(name_buf) - 1);
  return 1;
}


/* every entry with that name, until one loads */
static int load_by_name (const char *name) {
  const Known *k;
  for (k = known; k->name; k++)
    if (m_stricmp(k->name, name) == 0 && load_known(k)) return 1;
  return 0;
}


static int is_known_name (const char *name) {
  const Known *k;
  for (k = known; k->name; k++)
    if (m_stricmp(k->name, name) == 0) return 1;
  return 0;
}


static void cache_clear (void);


/* the fonts of an earlier font_init go */
static void faces_free (void) {
  Face *faces[3];
  int i;
  faces[0] = &f_regular;
  faces[1] = &f_bold;
  faces[2] = &f_italic;
  for (i = 2; i >= 0; i--) {	/* the regular one last: a .ttc shares its data */
    Face *f = faces[i];
    if (!f->ok) continue;
#ifdef _WIN32
    face_drop_gdi(f);
#endif
    ot_close(&f->ot);
    if (i == 0 || f->data != f_regular.data) free(f->data);
  }
  for (i = 0; i < 3; i++) memset(faces[i], 0, sizeof(Face));
  cache_clear();
}


int font_init (const Config *c) {
  size_t i;
  faces_free();
  name_buf[0] = '\0';
#ifdef _WIN32
  smoothing = c->smoothing;
#else
  smoothing = SMOOTH_STB;
#endif
  use_ligatures = c->ligatures;
  if (c->font_file[0] != '\0') {	/* an explicit file wins */
    char *native = path_to_native(c->font_file);
    int ok = face_load(&f_regular, native, 0);
    free(native);
    if (ok) strncpy(name_buf, path_basename(c->font_file), sizeof(name_buf) - 1);
  }
  if (!f_regular.ok && c->font[0] != '\0') {
    if (is_known_name(c->font)) load_by_name(c->font);
    else {	/* maybe it is a file name, with or without ".ttf" */
      char *guess = xstrcat3(c->font, ".ttf", "");
      const char *file = find_file(c->font);
      if (file == NULL) file = find_file(guess);
      if (face_load(&f_regular, file, 0))
        strncpy(name_buf, c->font, sizeof(name_buf) - 1);
      free(guess);
    }
  }
  for (i = 0; !f_regular.ok && preferred[i] != NULL; i++)
    load_by_name(preferred[i]);
  if (!f_regular.ok) {	/* last resort: anything that says "mono" */
    list_fonts();
    for (i = 0; i < font_files.n && !f_regular.ok; i++) {
      const char *base = path_basename(font_files.v[i]);
      char low[64];
      size_t j;
      for (j = 0; j < sizeof(low) - 1 && base[j]; j++)
        low[j] = (char)((base[j] >= 'A' && base[j] <= 'Z') ? base[j] + 32 : base[j]);
      low[j] = '\0';
      if (strstr(low, "mono") != NULL && strstr(low, "bold") == NULL &&
          strstr(low, "italic") == NULL && strstr(low, "oblique") == NULL &&
          face_load(&f_regular, font_files.v[i], 0))
        strncpy(name_buf, base, sizeof(name_buf) - 1);
    }
  }
  if (!f_regular.ok) return -1;
#ifdef _WIN32
  if (f_regular.family[0] == L'\0') smoothing = SMOOTH_STB;	/* GDI cannot name it */
#endif
  font_set_px(cur_px);
  return 0;
}


const char *font_name (void) { return name_buf; }
int font_cell_w (void) { return cell_w; }
int font_cell_h (void) { return cell_h; }
int font_ascent (void) { return ascent; }


/*
** {==================================================================
** Glyph cache (open addressing, keyed by code point + style)
** ===================================================================
*/

typedef struct Slot {
  uint32_t key;	/* 0 = empty */
  Glyph g;
} Slot;

static Slot *slots = NULL;
static size_t nslots = 0, nused = 0;


static void clusters_clear (void);


static void cache_clear (void) {
  size_t i;
  clusters_clear();
  for (i = 0; i < nslots; i++) free(slots[i].g.bm);
  free(slots);
  slots = NULL;
  nslots = nused = 0;
}


static Slot *cache_find (uint32_t key) {
  size_t i;
  if (nslots == 0) return NULL;
  for (i = (key * 2654435761u) & (nslots - 1); ; i = (i + 1) & (nslots - 1))
    if (slots[i].key == key || slots[i].key == 0) return &slots[i];
}


static Slot *cache_insert (uint32_t key) {
  Slot *s;
  if ((nused + 1) * 10 >= nslots * 7) {	/* grow at 70% */
    Slot *old = slots;
    size_t oldn = nslots, i;
    nslots = nslots ? nslots * 2 : 512;
    slots = (Slot *)xmalloc(nslots * sizeof(Slot));
    memset(slots, 0, nslots * sizeof(Slot));
    for (i = 0; i < oldn; i++)
      if (old[i].key != 0) *cache_find(old[i].key) = old[i];
    free(old);
  }
  s = cache_find(key);
  s->key = key;
  nused++;
  return s;
}

/* }================================================================== */


/*
** The scales of a face at 'px' pixels per em. With 'snap' the vertical
** scale moves (by at most half a pixel of x-height) so the x-height is a
** whole number of pixels: with the baseline on a pixel edge too, the
** flat tops and bottoms of most lowercase letters come out sharp. Only
** below 32 px; above, the blur of half a pixel does not show.
*/
static void face_scale (Face *f, float px, int snap) {
  int x0, y0, x1, y1;
  f->scale = f->scale_y = stbtt_ScaleForMappingEmToPixels(&f->info, px);
  if (snap && px < 32.0f &&
      stbtt_GetCodepointBox(&f->info, 'x', &x0, &y0, &x1, &y1) && y1 > 0) {
    float xh = (float)y1 * f->scale;
    float want = (float)floor(xh + 0.5f);
    if (want < 1.0f) want = 1.0f;
    f->scale_y = f->scale * want / xh;
  }
}


/*
** How much heavier small text gets: full strength at 11 px per em (8 pt)
** and below, none from 22 px (16 pt) up. GDI's hinted strokes are already
** firm, so they get half.
*/
static float darkening (void) {
  float k = (22.0f - cur_px) / 11.0f;
  if (k <= 0.0f) return 0.0f;
  if (k > 1.0f) k = 1.0f;
  return use_gdi() ? 0.5f * k : 0.8f * k;
}


/*
** Raises the coverage of partly covered pixels (full and empty ones stay):
** c' = c (1 + k) / (1 + k c), the "enhanced contrast" curve of
** DirectWrite. A one pixel stem at 60% becomes ~75%.
*/
static void darken (Glyph *g) {
  unsigned char lut[256];
  float k = darkening();
  size_t n, i;
  if (g->bm == NULL || k <= 0.0f) return;
  for (i = 0; i < 256; i++) {
    float c = (float)i / 255.0f;
    lut[i] = (unsigned char)(c * (1.0f + k) / (1.0f + k * c) * 255.0f + 0.5f);
  }
  n = (size_t)g->w * (size_t)g->h * (g->lcd == 1 ? 3u : 1u);
  for (i = 0; i < n; i++) g->bm[i] = lut[g->bm[i]];
}


static void stb_metrics (void) {
  int asc, desc, gap, adv, lsb;
  float s = f_regular.scale_y, height;
  stbtt_GetFontVMetrics(&f_regular.info, &asc, &desc, &gap);
  stbtt_GetCodepointHMetrics(&f_regular.info, 'M', &adv, &lsb);
  height = (float)(asc - desc + gap) * s;
  s = f_regular.scale;
  cell_h = (int)ceil(height * 1.08f);	/* a little air between lines */
  cell_w = (int)floor((float)adv * s + 0.5f);
  if (cell_w < 1) cell_w = 1;
  ascent = (int)floor((float)asc * f_regular.scale_y +
                      ((float)cell_h - height) * 0.5f + 0.5f);
}


void font_set_px (float px) {
  int i;
  if (px < 6.0f) px = 6.0f;
  cur_px = px;
  cache_clear();
  emoji_scale = 0.0f;
  if (!f_regular.ok) return;
  face_scale(&f_regular, px, 1);
  if (f_bold.ok) face_scale(&f_bold, px, 1);
  if (f_italic.ok) face_scale(&f_italic, px, 1);
  for (i = 0; i < MAX_FALLBACK; i++)
    if (f_fallback[i].ok) face_scale(&f_fallback[i], px, 0);
#ifdef _WIN32
  for (i = 0; i < 4; i++) same_file[i] = -1;
  face_drop_gdi(&f_regular);
  face_drop_gdi(&f_bold);
  face_drop_gdi(&f_italic);
  for (i = 0; i < MAX_FALLBACK; i++) face_drop_gdi(&f_fallback[i]);
  if (use_gdi()) {
    if (gdi_metrics()) return;
    smoothing = SMOOTH_STB;	/* GDI failed: stay with stb_truetype */
  }
#endif
  stb_metrics();
}


static Face *fallback_for (uint32_t cp, int *glyph) {
  int i;
  for (i = 0; i < MAX_FALLBACK && fallback_files[i] != NULL; i++) {
    Face *f = &f_fallback[i];
    if (fallback_state[i] == 0) {	/* loaded the first time they are needed */
      fallback_state[i] = face_load(f, find_file(fallback_files[i]), 0) ? 1 : -1;
      if (f->ok) face_scale(f, cur_px, 0);
    }
    if (f->ok && (*glyph = stbtt_FindGlyphIndex(&f->info, (int)cp)) != 0)
      return f;
  }
  return NULL;
}


/* one pixel fatter to the right */
static void embolden (Glyph *g) {
  int w = g->w + 1, x, y;
  unsigned char *out = (unsigned char *)xmalloc((size_t)w * (size_t)g->h);
  for (y = 0; y < g->h; y++) {
    const unsigned char *in = g->bm + (size_t)y * (size_t)g->w;
    for (x = 0; x < w; x++) {
      int a = (x < g->w) ? in[x] : 0;
      int b = (x > 0) ? in[x - 1] : 0;
      out[(size_t)y * (size_t)w + (size_t)x] = (unsigned char)(a > b ? a : b);
    }
  }
  free(g->bm);
  g->bm = out;
  g->w = w;
}


/* leans the glyph to the right around the baseline */
static void slant (Glyph *g) {
  const float k = 0.2f;
  float lo = k * (float)(-(g->yoff + g->h - 1));	/* bottom row */
  float hi = k * (float)(-g->yoff);	/* top row */
  int base = (int)floor(lo);
  int w = g->w + (int)ceil(hi - (float)base) + 1, x, y;
  unsigned char *out = (unsigned char *)xmalloc((size_t)w * (size_t)g->h);
  memset(out, 0, (size_t)w * (size_t)g->h);
  for (y = 0; y < g->h; y++) {
    float shift = k * (float)(-(g->yoff + y)) - (float)base;
    int is = (int)shift;
    float f = shift - (float)is;
    const unsigned char *in = g->bm + (size_t)y * (size_t)g->w;
    unsigned char *row = out + (size_t)y * (size_t)w;
    for (x = 0; x < g->w; x++) {
      int a = row[x + is] + (int)((float)in[x] * (1.0f - f));
      int b = row[x + is + 1] + (int)((float)in[x] * f);
      row[x + is] = (unsigned char)(a > 255 ? 255 : a);
      row[x + is + 1] = (unsigned char)(b > 255 ? 255 : b);
    }
  }
  free(g->bm);
  g->bm = out;
  g->w = w;
  g->xoff += base;
}


/*
** Glyph 'glyph' of face f into g. GDI draws it when it is on (the code
** point cp, or the glyph itself when by_id), else stb_truetype.
** fallback: f is not the main font, center it in 'span'.
*/
static void render (Glyph *g, Face *f, int glyph, uint32_t cp, int by_id, int bold,
                    int italic, int fake_bold, int fake_italic, int fallback,
                    int dark, int span) {
  int x0, y0, x1, y1, adv, lsb;
#ifdef _WIN32
  if (use_gdi()) {
    /* GDI picks the bold/italic file of the family itself, or makes it */
    Face *gf = (f == &f_bold || f == &f_italic) ? &f_regular : f;
    int style = (bold ? ST_BOLD : 0) | (italic ? ST_ITALIC : 0);
    if (gf->family[0] != L'\0' &&
        gdi_glyph(gf, cp, by_id ? glyph : -1, style, dark, fallback ? span : 0, g)) {
      darken(g);
      return;
    }
  }
#else
  (void)cp; (void)by_id; (void)bold; (void)italic; (void)dark;
#endif
  stbtt_GetGlyphBitmapBox(&f->info, glyph, f->scale, f->scale_y, &x0, &y0, &x1, &y1);
  g->w = x1 - x0;
  g->h = y1 - y0;
  g->xoff = x0;
  g->yoff = y0;
  if (g->w <= 0 || g->h <= 0) {
    g->w = g->h = 0;
    return;
  }
  g->bm = (unsigned char *)xmalloc((size_t)g->w * (size_t)g->h);
  stbtt_MakeGlyphBitmap(&f->info, g->bm, g->w, g->h, g->w, f->scale, f->scale_y,
                        glyph);
  if (fallback) {	/* a fallback font is not monospace: center it in its cell(s) */
    stbtt_GetGlyphHMetrics(&f->info, glyph, &adv, &lsb);
    g->xoff += (span - (int)((float)adv * f->scale)) / 2;
  }
  if (fake_bold) embolden(g);
  if (fake_italic) slant(g);
  darken(g);
}


/*
** {==================================================================
** Color emoji
** ===================================================================
*/

static Face *emoji_face (void) {
  int i;
  if (emoji_state != 0) return emoji_state > 0 ? &f_emoji : NULL;
  emoji_state = -1;
  for (i = 0; emoji_files[i] != NULL && emoji_state < 0; i++) {
    if (!face_load(&f_emoji, find_file(emoji_files[i]), 0)) continue;
    if ((f_emoji.ot.colr != 0 && f_emoji.ot.cpal != 0) || f_emoji.ot.cbdt != 0 ||
        f_emoji.ot.sbix != 0)
      emoji_state = 1;
  }
  return emoji_state > 0 ? &f_emoji : NULL;
}


/* shown as a color picture by itself: the wide pictographs */
static int emoji_presentation (uint32_t cp) {
  return grid_wcwidth(cp) == 2 &&
         ((cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2300 && cp <= 0x2BFF));
}


/* premultiplied 0xAARRGGBB back to plain */
static uint32_t unpremultiply (uint32_t a, uint32_t r, uint32_t g, uint32_t b) {
  if (a == 0) return 0;
  r = r * 255 / a;
  g = g * 255 / a;
  b = b * 255 / a;
  return (a << 24) | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) |
         (b > 255 ? 255 : b);
}


/* the box of a glyph's layers in font units, placed at pen x; 0: none */
static int layers_box (Face *f, int gid, int pen, int *x0, int *y0, int *x1, int *y1) {
  int first, n = ot_color_layers(&f->ot, gid, &first), k, any = 0;
  for (k = 0; k < n; k++) {
    int lg, pal, a0, b0, a1, b1;
    ot_layer(&f->ot, first + k, &lg, &pal);
    if (!stbtt_GetGlyphBox(&f->info, lg, &a0, &b0, &a1, &b1) || a1 <= a0) continue;
    if (a0 + pen < *x0) *x0 = a0 + pen;
    if (b0 < *y0) *y0 = b0;
    if (a1 + pen > *x1) *x1 = a1 + pen;
    if (b1 > *y1) *y1 = b1;
    any = 1;
  }
  return any;
}


/*
** COLR: the layers of the glyphs painted one over the other, each an
** outline in a color of the palette; the glyphs side by side at their
** advances (a family is people standing close). Every emoji gets the
** same size - the size that fits 😀 into two cells - unless it would not
** fit; the drawing is centered in its cells.
*/
static int layers_glyph (Face *f, const uint16_t *gl, int m, int span, Glyph *g) {
  int x0 = 1 << 20, y0 = 1 << 20, x1 = -(1 << 20), y1 = -(1 << 20), k, pen, w, h;
  int ox, oy, adv, lsb;
  float s;
  uint32_t *acc, *out;
  if (emoji_scale == 0.0f) {	/* the size of all of them, from 😀 */
    int r0 = 1 << 20, q0 = 1 << 20, r1 = -(1 << 20), q1 = -(1 << 20);
    int ref = stbtt_FindGlyphIndex(&f->info, 0x1F600);
    emoji_scale = stbtt_ScaleForMappingEmToPixels(&f->info, (float)cell_h);
    if (ref != 0 && layers_box(f, ref, 0, &r0, &q0, &r1, &q1)) {
      float sw = (float)(cell_w * 2) * 0.92f / (float)(r1 - r0);
      float sh = (float)cell_h * 0.86f / (float)(q1 - q0);
      emoji_scale = sw < sh ? sw : sh;
    }
  }
  for (k = 0, pen = 0; k < m; k++) {
    layers_box(f, gl[k], pen, &x0, &y0, &x1, &y1);
    stbtt_GetGlyphHMetrics(&f->info, gl[k], &adv, &lsb);
    pen += adv;
  }
  if (x1 <= x0 || y1 <= y0) return 0;
  s = emoji_scale;
  if ((float)(x1 - x0) * s > (float)span) s = (float)span / (float)(x1 - x0);
  if ((float)(y1 - y0) * s > (float)cell_h) s = (float)cell_h / (float)(y1 - y0);
  ox = (int)floor((float)x0 * s);	/* the picture's corner in pixels */
  oy = (int)floor((float)-y1 * s);
  w = (int)ceil((float)x1 * s) - ox + 1;
  h = (int)ceil((float)-y0 * s) - oy + 1;
  acc = (uint32_t *)xmalloc((size_t)w * (size_t)h * sizeof(uint32_t) * 4);
  memset(acc, 0, (size_t)w * (size_t)h * sizeof(uint32_t) * 4);
  for (k = 0, pen = 0; k < m; k++) {
    int first, n = ot_color_layers(&f->ot, gl[k], &first), j;
    float shift = (float)pen * s;
    for (j = 0; j < n; j++) {	/* over: premultiplied a, r, g, b per pixel */
      int lg, pal, a0, b0, a1, b1, x, y, bw, bh;
      uint32_t color, ca, cr, cg, cb;
      unsigned char *cov;
      ot_layer(&f->ot, first + j, &lg, &pal);
      stbtt_GetGlyphBitmapBoxSubpixel(&f->info, lg, s, s, shift - (float)floor(shift), 0.0f,
                                      &a0, &b0, &a1, &b1);
      bw = a1 - a0;
      bh = b1 - b0;
      if (bw <= 0 || bh <= 0) continue;
      cov = (unsigned char *)xmalloc((size_t)bw * (size_t)bh);
      stbtt_MakeGlyphBitmapSubpixel(&f->info, cov, bw, bh, bw, s, s,
                                    shift - (float)floor(shift), 0.0f, lg);
      a0 += (int)floor(shift) - ox;
      b0 -= oy;
      color = ot_palette(&f->ot, pal, 0xFFFFFFu);
      ca = color >> 24;
      cr = (color >> 16) & 0xFF;
      cg = (color >> 8) & 0xFF;
      cb = color & 0xFF;
      for (y = 0; y < bh; y++)
        for (x = 0; x < bw; x++) {
          uint32_t c = cov[(size_t)y * (size_t)bw + (size_t)x], a, *p;
          if (c == 0 || x + a0 < 0 || x + a0 >= w || y + b0 < 0 || y + b0 >= h) continue;
          a = ca * c / 255;
          p = acc + ((size_t)(y + b0) * (size_t)w + (size_t)(x + a0)) * 4;
          p[0] = a + p[0] * (255 - a) / 255;
          p[1] = cr * a / 255 + p[1] * (255 - a) / 255;
          p[2] = cg * a / 255 + p[2] * (255 - a) / 255;
          p[3] = cb * a / 255 + p[3] * (255 - a) / 255;
        }
      free(cov);
    }
    stbtt_GetGlyphHMetrics(&f->info, gl[k], &adv, &lsb);
    pen += adv;
  }
  out = (uint32_t *)xmalloc((size_t)w * (size_t)h * sizeof(uint32_t));
  for (k = 0; k < w * h; k++)
    out[k] = unpremultiply(acc[k * 4], acc[k * 4 + 1], acc[k * 4 + 2], acc[k * 4 + 3]);
  free(acc);
  g->w = w;
  g->h = h;
  g->xoff = (span - w) / 2;
  g->yoff = (cell_h - h) / 2 - ascent;
  g->lcd = 3;
  g->bm = (unsigned char *)out;
  return 1;
}


/* CBDT, sbix: the picture made small enough for the cells (each pixel
** the average of the ones it covers) */
static int picture_glyph (Face *f, int gid, int span, Glyph *g) {
  int iw, ih, w, h, x, y;
  uint32_t *img = ot_bitmap(&f->ot, gid, cell_h, &iw, &ih), *out;
  if (img == NULL) return 0;
  if ((float)span / (float)iw < (float)cell_h / (float)ih) {
    w = span;
    h = ih * span / iw;
  }
  else {
    h = cell_h;
    w = iw * cell_h / ih;
  }
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  out = (uint32_t *)xmalloc((size_t)w * (size_t)h * sizeof(uint32_t));
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++) {
      int sx0 = x * iw / w, sx1 = (x + 1) * iw / w, sy0 = y * ih / h, sy1 = (y + 1) * ih / h;
      uint32_t a = 0, r = 0, gg = 0, b = 0, n = 0;
      int i, j;
      if (sx1 <= sx0) sx1 = sx0 + 1;
      if (sy1 <= sy0) sy1 = sy0 + 1;
      for (j = sy0; j < sy1 && j < ih; j++)
        for (i = sx0; i < sx1 && i < iw; i++) {
          uint32_t p = img[(size_t)j * (size_t)iw + (size_t)i], pa = p >> 24;
          a += pa;
          r += ((p >> 16) & 0xFF) * pa / 255;
          gg += ((p >> 8) & 0xFF) * pa / 255;
          b += (p & 0xFF) * pa / 255;
          n++;
        }
      out[(size_t)y * (size_t)w + (size_t)x] =
        n ? unpremultiply(a / n, r / n, gg / n, b / n) : 0;
    }
  free(img);
  g->w = w;
  g->h = h;
  g->xoff = (span - w) / 2;
  g->yoff = (cell_h - h) / 2 - ascent;
  g->lcd = 3;
  g->bm = (unsigned char *)out;
  return 1;
}


#define NCLUSTERS	256
#define CLUSTER_CPS	16	/* code points of one kept */

/* glyphs of the emoji font in color, 'cells' wide; 0: they have none */
static int color_glyph (Face *f, const uint16_t *gl, int m, int cells, Glyph *g) {
  uint16_t shown[CLUSTER_CPS * 2];
  int first, k, n = 0;
  memset(g, 0, sizeof(*g));
  for (k = 0; k < m && n < CLUSTER_CPS * 2; k++) {	/* all in color, or no emoji */
    if (ot_color_layers(&f->ot, gl[k], &first) > 0) shown[n++] = gl[k];
    else if (!stbtt_IsGlyphEmpty(&f->info, gl[k])) break;
  }	/* (empty ones - a variation selector, a joiner - are not drawn) */
  if (n > 0 && k == m) return layers_glyph(f, shown, n, cell_w * cells, g);
  if (m > 0 && (f->ot.cbdt != 0 || f->ot.sbix != 0))	/* pictures: the first */
    return picture_glyph(f, gl[0], cell_w * cells, g);
  return 0;
}


/* emoji of several code points, the last few kept */

typedef struct Cluster {
  uint32_t cps[CLUSTER_CPS];
  int n, cells, ok;	/* n 0: an empty slot */
  Glyph g;
} Cluster;

static Cluster clusters[NCLUSTERS];


static void clusters_clear (void) {
  int i;
  for (i = 0; i < NCLUSTERS; i++) free(clusters[i].g.bm);
  memset(clusters, 0, sizeof(clusters));
}


const Glyph *font_emoji (const uint32_t *cps, int n, int cells) {
  Face *ef;
  Cluster *c;
  uint16_t gl[CLUSTER_CPS * 2];
  int cl[CLUSTER_CPS * 2], k, m = 0;
  uint32_t h = 2166136261u;
  if (n <= 0 || n > CLUSTER_CPS || (ef = emoji_face()) == NULL) return NULL;
  for (k = 0; k < n; k++) h = (h ^ cps[k]) * 16777619u;
  c = &clusters[(h ^ (uint32_t)cells) % NCLUSTERS];
  if (c->n == n && c->cells == cells && memcmp(c->cps, cps, (size_t)n * sizeof(uint32_t)) == 0)
    return c->ok ? &c->g : NULL;
  free(c->g.bm);
  memset(c, 0, sizeof(*c));
  memcpy(c->cps, cps, (size_t)n * sizeof(uint32_t));
  c->n = n;
  c->cells = cells;
  for (k = 0; k < n; k++) {	/* a variation selector the font lacks: skipped */
    int gi = stbtt_FindGlyphIndex(&ef->info, (int)cps[k]);
    if (gi == 0 && k == 0) return NULL;
    if (gi == 0) continue;
    gl[m] = (uint16_t)gi;
    cl[m++] = k;
  }
  m = ot_shape(&ef->ot, OT_SEQ, gl, cl, m, CLUSTER_CPS * 2);	/* ZWJ, skin tone */
  c->ok = color_glyph(ef, gl, m, cells, &c->g);
  return c->ok ? &c->g : NULL;
}

/* }================================================================== */


/* the face text of this style is drawn with; *fake_*: what it lacks */
static Face *style_face (uint32_t cp, int bold, int italic, int *fake_bold,
                         int *fake_italic) {
  *fake_bold = bold;
  *fake_italic = italic;
  if (bold && f_bold.ok && (cp == 0 || stbtt_FindGlyphIndex(&f_bold.info, (int)cp) != 0)) {
    *fake_bold = 0;
    return &f_bold;
  }
  if (italic && f_italic.ok &&
      (cp == 0 || stbtt_FindGlyphIndex(&f_italic.info, (int)cp) != 0)) {
    *fake_italic = 0;
    return &f_italic;
  }
  return &f_regular;
}


const Glyph *font_glyph (uint32_t cp, int bold, int italic, int dark) {
  uint32_t key = (cp & 0x1FFFFF) | (bold ? 1u << 24 : 0) |
                 (italic ? 1u << 25 : 0) | (1u << 31);
  Slot *s;
  Face *f, *ef;
  int glyph, fake_bold, fake_italic, fallback = 0;
  Glyph *g;
  if (use_gdi() && dark) key |= 1u << 26;	/* GDI tunes by color */
  s = cache_find(key);
  if (s != NULL && s->key == key) return &s->g;
  s = cache_insert(key);
  g = &s->g;
  memset(g, 0, sizeof(*g));
  if (!f_regular.ok) return g;
  if (emoji_presentation(cp) && (ef = emoji_face()) != NULL &&
      (glyph = stbtt_FindGlyphIndex(&ef->info, (int)cp)) != 0) {
    uint16_t one = (uint16_t)glyph;
    if (color_glyph(ef, &one, 1, 2, g)) return g;
  }
  f = style_face(cp, bold, italic, &fake_bold, &fake_italic);
  glyph = stbtt_FindGlyphIndex(&f->info, (int)cp);
  if (glyph == 0) {
    Face *fb = fallback_for(cp, &glyph);
    if (fb != NULL) {
      f = fb;
      fallback = 1;
    }
    else {	/* nobody has it: show the replacement character */
      f = &f_regular;
      cp = stbtt_FindGlyphIndex(&f->info, 0xFFFD) ? 0xFFFD : '?';
      glyph = stbtt_FindGlyphIndex(&f->info, (int)cp);
    }
  }
  render(g, f, glyph, cp, 0, bold, italic, fake_bold, fake_italic, fallback, dark,
         cell_w * (grid_wcwidth(cp) == 2 ? 2 : 1));
  return g;
}


/*
** {==================================================================
** Ligatures
** ===================================================================
*/

int font_has_ligatures (int bold, int italic) {
  int fb, fi;
  Face *f;
  if (!use_ligatures || !f_regular.ok) return 0;
  f = style_face(0, bold, italic, &fb, &fi);
  if (!ot_can_shape(&f->ot, OT_TEXT)) return 0;
#ifdef _WIN32
  if (use_gdi() && !gdi_same_file(f, (bold ? ST_BOLD : 0) | (italic ? ST_ITALIC : 0)))
    return 0;	/* GDI draws another font: its glyphs are not these */
#endif
  return 1;
}


int font_shape (const uint32_t *cps, int n, int bold, int italic, uint16_t *plain,
                uint16_t *out, int *cells, int cap) {
  int k, fb, fi;
  Face *f = style_face(0, bold, italic, &fb, &fi);
  if (n > cap) return -1;
  for (k = 0; k < n; k++) {
    int gi = stbtt_FindGlyphIndex(&f->info, (int)cps[k]);
    if (gi == 0) return -1;	/* from a fallback font: no ligature */
    plain[k] = out[k] = (uint16_t)gi;
    cells[k] = k;
  }
  return ot_shape(&f->ot, OT_TEXT, out, cells, n, cap);
}


const Glyph *font_glyph_id (int gid, int bold, int italic, int dark) {
  uint32_t key = ((uint32_t)gid & 0xFFFF) | (bold ? 1u << 24 : 0) |
                 (italic ? 1u << 25 : 0) | (1u << 30) | (1u << 31);
  Slot *s;
  Face *f;
  int fake_bold, fake_italic;
  Glyph *g;
  if (use_gdi() && dark) key |= 1u << 26;
  s = cache_find(key);
  if (s != NULL && s->key == key) return &s->g;
  s = cache_insert(key);
  g = &s->g;
  memset(g, 0, sizeof(*g));
  if (!f_regular.ok) return g;
  f = style_face(0, bold, italic, &fake_bold, &fake_italic);
  render(g, f, gid, 0, 1, bold, italic, fake_bold, fake_italic, 0, dark, cell_w);
  return g;
}

/* }================================================================== */
