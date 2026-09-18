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
  stbtt_fontinfo info;
  float scale;
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

/* looked at in this order when the config names no font; Hack (the Nerd
** Font version, with the Powerline and icon glyphs) comes with mmc in
** usr/share/fonts, the others are what each system has */
static const char *const preferred[] = {
  "Hack",
#if defined(_WIN32)
  "Cascadia Mono", "Cascadia Code", "Consolas", "Lucida Console",
#elif defined(__APPLE__)
  "Menlo", "SF Mono", "Monaco",
#else
  "DejaVu Sans Mono", "Liberation Mono", "Noto Sans Mono", "Ubuntu Mono",
#endif
  "JetBrains Mono", "Courier New", NULL
};

/* fonts asked for glyphs the main font does not have; the Nerd Font
** first, so the icons work whatever the main font is */
static const char *const fallback_files[] = {
  "HackNerdFontMono-Regular.ttf",
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

static Vec font_files;	/* every font file found on the system */
static int files_listed = 0;
static char *extra_dir = NULL;
static Face f_regular, f_bold, f_italic;
static Face f_fallback[MAX_FALLBACK];
static int fallback_state[MAX_FALLBACK];	/* 0 not tried, 1 loaded, -1 none */
static char name_buf[160];
static float cur_px = 15.0f;
static int cell_w = 8, cell_h = 16, ascent = 12;
static int smoothing = SMOOTH_STB;


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
** tunes ClearType for the colors, so both come out right.
*/
static int gdi_glyph (Face *f, uint32_t cp, int style, int dark, int span,
                      Glyph *g) {
  wchar_t wc[2];
  int n = 1, pad = cell_h, w = cell_w * 2 + 2 * pad, h = cell_h + 2 * pad;
  int x0 = w, y0 = h, x1 = -1, y1 = -1, x, y, x_at;
  uint32_t paper = dark ? 0xFFFFFFu : 0u;
  HFONT hf = face_hfont(f, style);
  if (hf == NULL || !canvas(w, h)) return 0;
  if (cp >= 0x10000) {	/* UTF-16 surrogate pair */
    wc[0] = (wchar_t)(0xD800 + ((cp - 0x10000) >> 10));
    wc[1] = (wchar_t)(0xDC00 + ((cp - 0x10000) & 0x3FF));
    n = 2;
  }
  else wc[0] = (wchar_t)cp;
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++) gbits[(size_t)y * (size_t)gw + (size_t)x] = paper;
  SelectObject(gdc, hf);
  SetTextColor(gdc, dark ? RGB(0, 0, 0) : RGB(255, 255, 255));
  x_at = pad;
  if (span > 0) {	/* a fallback font is not monospace: center it */
    SIZE sz;
    if (GetTextExtentPoint32W(gdc, wc, n, &sz)) x_at += (span - sz.cx) / 2;
  }
  ExtTextOutW(gdc, x_at, pad + ascent, 0, NULL, wc, (UINT)n, NULL);
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
  g->xoff = x0 - pad;
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

/* }================================================================== */

#else

static int use_gdi (void) { return 0; }

#endif


static int face_open (Face *f, unsigned char *data, int index) {
  int off = stbtt_GetFontOffsetForIndex(data, index);
  memset(f, 0, sizeof(*f));
  if (off < 0 || !stbtt_InitFont(&f->info, data, off)) return 0;
  f->data = data;
  f->ok = 1;
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
  if (!face_open(f, data, index)) {
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
  if (k->ttc_bold) face_open(&f_bold, f_regular.data, k->ttc_bold);
  else face_load(&f_bold, find_file(k->bold), 0);
  if (k->ttc_italic) face_open(&f_italic, f_regular.data, k->ttc_italic);
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


int font_init (const Config *c) {
  size_t i;
  name_buf[0] = '\0';
#ifdef _WIN32
  smoothing = c->smoothing;
#else
  smoothing = SMOOTH_STB;
#endif
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


static void cache_clear (void) {
  size_t i;
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


static void stb_metrics (void) {
  int asc, desc, gap, adv, lsb;
  float s = f_regular.scale, height;
  stbtt_GetFontVMetrics(&f_regular.info, &asc, &desc, &gap);
  stbtt_GetCodepointHMetrics(&f_regular.info, 'M', &adv, &lsb);
  height = (float)(asc - desc + gap) * s;
  cell_h = (int)ceil(height * 1.08f);	/* a little air between lines */
  cell_w = (int)floor((float)adv * s + 0.5f);
  if (cell_w < 1) cell_w = 1;
  ascent = (int)floor((float)asc * s + ((float)cell_h - height) * 0.5f + 0.5f);
}


void font_set_px (float px) {
  int i;
  if (px < 6.0f) px = 6.0f;
  cur_px = px;
  cache_clear();
  if (!f_regular.ok) return;
  f_regular.scale = stbtt_ScaleForMappingEmToPixels(&f_regular.info, px);
  if (f_bold.ok) f_bold.scale = stbtt_ScaleForMappingEmToPixels(&f_bold.info, px);
  if (f_italic.ok)
    f_italic.scale = stbtt_ScaleForMappingEmToPixels(&f_italic.info, px);
  for (i = 0; i < MAX_FALLBACK; i++)
    if (f_fallback[i].ok)
      f_fallback[i].scale = stbtt_ScaleForMappingEmToPixels(&f_fallback[i].info, px);
#ifdef _WIN32
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
      if (f->ok) f->scale = stbtt_ScaleForMappingEmToPixels(&f->info, cur_px);
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


const Glyph *font_glyph (uint32_t cp, int bold, int italic, int dark) {
  uint32_t key = (cp & 0x1FFFFF) | (bold ? 1u << 24 : 0) |
                 (italic ? 1u << 25 : 0) | (1u << 31);
  Slot *s;
  Face *f = &f_regular;
  int glyph, fake_bold = bold, fake_italic = italic, fallback = 0;
  int x0, y0, x1, y1, adv, lsb, span;
  Glyph *g;
  if (use_gdi() && dark) key |= 1u << 26;	/* GDI tunes by color */
  s = cache_find(key);
  if (s != NULL && s->key == key) return &s->g;
  s = cache_insert(key);
  g = &s->g;
  memset(g, 0, sizeof(*g));
  if (!f_regular.ok) return g;
  if (bold && f_bold.ok && stbtt_FindGlyphIndex(&f_bold.info, (int)cp) != 0) {
    f = &f_bold;
    fake_bold = 0;
  }
  else if (italic && f_italic.ok &&
           stbtt_FindGlyphIndex(&f_italic.info, (int)cp) != 0) {
    f = &f_italic;
    fake_italic = 0;
  }
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
  span = cell_w * (grid_wcwidth(cp) == 2 ? 2 : 1);
#ifdef _WIN32
  if (use_gdi()) {
    /* GDI picks the bold/italic file of the family itself, or makes it */
    Face *gf = (f == &f_bold || f == &f_italic) ? &f_regular : f;
    int style = (bold ? ST_BOLD : 0) | (italic ? ST_ITALIC : 0);
    if (gf->family[0] != L'\0' &&
        gdi_glyph(gf, cp, style, dark, fallback ? span : 0, g))
      return g;
  }
#endif
  stbtt_GetGlyphBitmapBox(&f->info, glyph, f->scale, f->scale, &x0, &y0, &x1, &y1);
  g->w = x1 - x0;
  g->h = y1 - y0;
  g->xoff = x0;
  g->yoff = y0;
  if (g->w <= 0 || g->h <= 0) {
    g->w = g->h = 0;
    return g;
  }
  g->bm = (unsigned char *)xmalloc((size_t)g->w * (size_t)g->h);
  stbtt_MakeGlyphBitmap(&f->info, g->bm, g->w, g->h, g->w, f->scale, f->scale,
                        glyph);
  if (fallback) {	/* a fallback font is not monospace: center it in its cell(s) */
    stbtt_GetGlyphHMetrics(&f->info, glyph, &adv, &lsb);
    g->xoff += (span - (int)((float)adv * f->scale)) / 2;
  }
  if (fake_bold) embolden(g);
  if (fake_italic) slant(g);
  return g;
}
