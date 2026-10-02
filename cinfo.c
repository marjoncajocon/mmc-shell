/*
** cinfo.c - files, time and processes: stat date nproc printenv tty
** mktemp truncate yes expr sync nohup timeout xargs
**
** stat and date share the time code here: our own calendar arithmetic
** and time zones, so every OS prints the same. TZ is read the way glibc
** reads it: a zoneinfo file name (Europe/Paris, looked for in $TZDIR,
** /usr/share/zoneinfo ...), a POSIX rule (EST5EDT, <+08>-8, CET-1CEST,
** M3.5.0,M10.5.0/3), a bare name like UTC (offset 0), or, unset, the
** system's own zone. Only an exported TZ counts, as for a program.
**
** stat on Windows shows what git-bash's stat shows: the volume serial
** number as the device, the file id as the inode, 1 KiB blocks, 64 KiB
** IO blocks, the change and birth times. Owner ids are mmc's (1000).
**
** nohup, timeout and xargs run commands. One after the other they run in
** this mmc through sh_eval_argv, like env and find -exec, so `xargs rm`
** finds our rm when there is no rm program. When they must run side by
** side or be stopped (timeout, xargs -P), a program is started directly
** and a builtin in a child mmc ("mmc --stage", as a pipeline stage).
** Shell functions are not commands for them, as with the GNU tools.
*/

#include "mmc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OPT_VERSION	(-3)


static int tool_version (int out, const char *tool) {
  fd_printf(out, "%s (mmc) %s\n", tool, MMC_VERSION);
  return 0;
}


/* 'x' quoted for messages, the way GNU's quote() does it */
static const char *q (const char *s) {
  static char bufs[4][1024];
  static int k = 0;
  char *b = bufs[k = (k + 1) & 3];
  if (strchr(s, '\'') == NULL) snprintf(b, 1024, "'%s'", s);
  else snprintf(b, 1024, "\"%s\"", s);
  return b;
}


static int try_help (int err, const char *tool, int status) {
  fd_printf(err, "Try '%s --help' for more information.\n", tool);
  return status;
}


/* tools that take only --help and --version before their operands (yes,
** nohup): -1 and *first the first operand, or the status to return */
static int std_opts (int argc, char **argv, const char *tool, int out, int err, int bad,
                     int *first) {
  const char *a = argc > 1 ? argv[1] : "";
  *first = 1;
  if (strcmp(a, "--") == 0) *first = 2;
  else if (strcmp(a, "--help") == 0) return tool_help(out, tool);
  else if (strcmp(a, "--version") == 0) return tool_version(out, tool);
  else if (a[0] == '-' && a[1] == '-') {
    tool_err(err, tool, "unrecognized option %s", q(a));
    return try_help(err, tool, bad);
  }
  else if (a[0] == '-' && a[1] != '\0') {
    tool_err(err, tool, "invalid option -- '%c'", a[1]);
    return try_help(err, tool, bad);
  }
  return -1;
}


/*
** {==================================================================
** Calendar and time zones
** ===================================================================
*/

typedef struct DT {	/* a moment, broken down in some zone */
  long long t;	/* seconds since 1970-01-01 UTC */
  long nsec;
  long long year;
  int mon, mday, hour, min, sec, wday, yday;	/* mon 1..12, wday 0 = Sunday, yday 0.. */
  int isdst;
  long gmtoff;	/* seconds east of UTC */
  char zone[32];
} DT;


static long long floor_div (long long a, long long b) {
  long long d = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) d--;
  return d;
}


static long long days_from_civil (long long y, int m, int d) {
  long long era, yoe, doy, doe;
  y -= m <= 2;
  era = floor_div(y, 400);
  yoe = y - era * 400;
  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}


static void civil_from_days (long long z, long long *y, int *m, int *d) {
  long long era, doe, yoe, doy, mp;
  z += 719468;
  era = floor_div(z, 146097);
  doe = z - era * 146097;
  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  mp = (5 * doy + 2) / 153;
  *d = (int)(doy - (153 * mp + 2) / 5 + 1);
  *m = (int)(mp < 10 ? mp + 3 : mp - 9);
  *y = yoe + era * 400 + (*m <= 2);
}


static int weekday_of (long long days) {	/* 0 = Sunday; 1970-01-01 was a Thursday */
  return (int)((days % 7 + 7 + 4) % 7);
}


static int is_leap (long long y) {
  return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}


static int month_days (long long y, int m) {
  static const int md[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && is_leap(y) ? 29 : md[m - 1];
}


/* a rule of a POSIX TZ: Jn (1..365, no Feb 29), n (0..365) or Mm.w.d */
typedef struct TzRule {
  char kind;
  int m, w, d;
  long secs;	/* local time of day it happens */
} TzRule;

typedef struct PosixTz {
  char std[16], dst[16];
  long stdoff, dstoff;	/* seconds east of UTC */
  int has_dst;
  TzRule start, end;
} PosixTz;

typedef struct TzType {
  long off;
  int isdst, abbr;
} TzType;

typedef struct Zone {
  char *spec;	/* the TZ it was made for */
  int kind;	/* 0 POSIX rule, 1 zoneinfo file, 2 the system's */
  PosixTz p;	/* kind 0, and a file's footer */
  int has_footer;
  long long *trans;
  unsigned char *tidx;
  int ntrans;
  TzType *types;
  int ntypes;
  char *abbrs;
} Zone;

static Zone g_zone;
static int g_zone_set = 0;
static const char *g_tz_force = NULL;	/* date -u: "UTC0" */


static const char *tz_name (const char *s, char *out, size_t n) {
  size_t k = 0;
  if (*s == '<') {
    for (s++; *s && *s != '>'; s++)
      if (k + 1 < n) out[k++] = *s;
    if (*s != '>') return NULL;
    s++;
  }
  else
    for (; isalpha((unsigned char)*s); s++)
      if (k + 1 < n) out[k++] = *s;
  out[k] = '\0';
  return k >= 3 ? s : NULL;
}


/* [+-]hh[:mm[:ss]] */
static const char *tz_time (const char *s, long *secs) {
  int neg = 0, part = 0;
  long v[3] = {0, 0, 0};
  if (*s == '+' || *s == '-') neg = *s++ == '-';
  if (!isdigit((unsigned char)*s)) return NULL;
  for (;;) {
    int digits = 0;
    while (isdigit((unsigned char)*s) && digits < 3) {
      v[part] = v[part] * 10 + (*s++ - '0');
      digits++;
    }
    if (part < 2 && *s == ':' && isdigit((unsigned char)s[1])) {
      part++;
      s++;
    }
    else break;
  }
  *secs = (v[0] * 3600 + v[1] * 60 + v[2]) * (neg ? -1 : 1);
  return s;
}


static const char *tz_rule (const char *s, TzRule *r) {
  char *end;
  memset(r, 0, sizeof(*r));
  if (*s == 'J') {
    r->kind = 'J';
    r->d = (int)strtol(s + 1, &end, 10);
    if (end == s + 1 || r->d < 1 || r->d > 365) return NULL;
  }
  else if (*s == 'M') {
    r->kind = 'M';
    r->m = (int)strtol(s + 1, &end, 10);
    if (*end != '.') return NULL;
    r->w = (int)strtol(end + 1, &end, 10);
    if (*end != '.') return NULL;
    r->d = (int)strtol(end + 1, &end, 10);
    if (r->m < 1 || r->m > 12 || r->w < 1 || r->w > 5 || r->d < 0 || r->d > 6) return NULL;
  }
  else if (isdigit((unsigned char)*s)) {
    r->kind = 'N';
    r->d = (int)strtol(s, &end, 10);
    if (r->d > 365) return NULL;
  }
  else return NULL;
  s = end;
  r->secs = 7200;
  if (*s == '/') s = tz_time(s + 1, &r->secs);
  return s;
}


static int parse_posix_tz (const char *s, PosixTz *p) {
  long off;
  memset(p, 0, sizeof(*p));
  if ((s = tz_name(s, p->std, sizeof(p->std))) == NULL) return -1;
  if ((s = tz_time(s, &off)) == NULL) return -1;
  p->stdoff = -off;
  if (*s == '\0') return 0;
  if ((s = tz_name(s, p->dst, sizeof(p->dst))) == NULL) return -1;
  p->has_dst = 1;
  p->dstoff = p->stdoff + 3600;
  if (*s && *s != ',') {
    if ((s = tz_time(s, &off)) == NULL) return -1;
    p->dstoff = -off;
  }
  if (*s == '\0') {	/* no rules: the US ones, as glibc has them */
    tz_rule("M3.2.0", &p->start);
    tz_rule("M11.1.0", &p->end);
    return 0;
  }
  if (*s != ',' || (s = tz_rule(s + 1, &p->start)) == NULL || *s != ',' ||
      (s = tz_rule(s + 1, &p->end)) == NULL || *s != '\0')
    return -1;
  return 0;
}


/* the day (since 1970) a rule falls on in year y */
static long long rule_day (long long y, const TzRule *r) {
  long long jan1 = days_from_civil(y, 1, 1), first, day;
  if (r->kind == 'J') return jan1 + r->d - 1 + (is_leap(y) && r->d >= 60);
  if (r->kind == 'N') return jan1 + r->d;
  first = days_from_civil(y, r->m, 1);
  day = first + (r->d - weekday_of(first) + 7) % 7 + 7 * (r->w - 1);
  while (day >= first + month_days(y, r->m)) day -= 7;
  return day;
}


static void posix_at (const PosixTz *p, long long t, long *off, int *isdst, const char **abbr) {
  long long y, start, end;
  int m, d, dst;
  if (!p->has_dst) {
    *off = p->stdoff;
    *isdst = 0;
    *abbr = p->std;
    return;
  }
  civil_from_days(floor_div(t + p->stdoff, 86400), &y, &m, &d);
  start = rule_day(y, &p->start) * 86400 + p->start.secs - p->stdoff;
  end = rule_day(y, &p->end) * 86400 + p->end.secs - p->dstoff;
  if (start < end) dst = t >= start && t < end;
  else dst = !(t >= end && t < start);
  *off = dst ? p->dstoff : p->stdoff;
  *isdst = dst;
  *abbr = dst ? p->dst : p->std;
}


static unsigned long be32 (const unsigned char *p) {
  return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | p[3];
}


static long sbe32 (const unsigned char *p) {
  unsigned long v = be32(p);
  return v >= 0x80000000UL ? (long)(v - 0x80000000UL) - 0x7FFFFFFFL - 1 : (long)v;
}


static long long sbe64 (const unsigned char *p) {
  unsigned long long v = ((unsigned long long)be32(p) << 32) | be32(p + 4);
  if (v >= 0x8000000000000000ULL) return (long long)(v - 0x8000000000000000ULL) - 0x7FFFFFFFFFFFFFFFLL - 1;
  return (long long)v;
}


static void zone_clear (Zone *z) {
  free(z->spec);
  free(z->trans);
  free(z->tidx);
  free(z->types);
  free(z->abbrs);
  memset(z, 0, sizeof(*z));
}


/* a TZif file (RFC 8536): version 1, or the 64 bit part of 2 and later */
static int tzif_load (Zone *z, const char *native) {
  size_t len, need, i;
  unsigned char *d = (unsigned char *)read_file(native, &len), *p;
  unsigned long cnt[6];
  int big = 0, k;
  if (d == NULL) return -1;
  p = d;
  if (len < 44 || memcmp(d, "TZif", 4) != 0) goto bad;
  for (k = 0; k < 6; k++) cnt[k] = be32(p + 20 + 4 * k);
  need = 44 + cnt[3] * 5 + cnt[4] * 6 + cnt[5] + cnt[2] * 8 + cnt[1] + cnt[0];
  if (need > len) goto bad;
  if (d[4] >= '2' && need + 44 <= len && memcmp(d + need, "TZif", 4) == 0) {
    p = d + need;
    for (k = 0; k < 6; k++) cnt[k] = be32(p + 20 + 4 * k);
    big = 1;
    if ((size_t)(p - d) + 44 + cnt[3] * 9 + cnt[4] * 6 + cnt[5] + cnt[2] * 12 + cnt[1] + cnt[0] > len)
      goto bad;
  }
  if (cnt[4] == 0 || cnt[4] > 256) goto bad;
  p += 44;
  z->ntrans = (int)cnt[3];
  z->trans = (long long *)xmalloc((cnt[3] + 1) * sizeof(long long));
  z->tidx = (unsigned char *)xmalloc(cnt[3] + 1);
  for (i = 0; i < cnt[3]; i++) {
    z->trans[i] = big ? sbe64(p) : (long long)sbe32(p);
    p += big ? 8 : 4;
  }
  for (i = 0; i < cnt[3]; i++) {
    z->tidx[i] = *p++;
    if (z->tidx[i] >= cnt[4]) z->tidx[i] = 0;
  }
  z->ntypes = (int)cnt[4];
  z->types = (TzType *)xmalloc(cnt[4] * sizeof(TzType));
  for (i = 0; i < cnt[4]; i++) {
    z->types[i].off = sbe32(p);
    z->types[i].isdst = p[4];
    z->types[i].abbr = p[5] < cnt[5] ? p[5] : 0;
    p += 6;
  }
  z->abbrs = (char *)xmalloc(cnt[5] + 1);
  memcpy(z->abbrs, p, cnt[5]);
  z->abbrs[cnt[5]] = '\0';
  p += cnt[5] + cnt[2] * (big ? 12 : 8) + cnt[1] + cnt[0];
  if (big && (size_t)(p - d) < len && *p == '\n') {	/* the footer: a POSIX rule */
    char *nl = (char *)memchr(p + 1, '\n', len - (size_t)(p + 1 - d));
    if (nl != NULL && nl > (char *)p + 1) {
      char *rule = xstrndup((char *)p + 1, (size_t)(nl - (char *)p - 1));
      z->has_footer = parse_posix_tz(rule, &z->p) == 0;
      free(rule);
    }
  }
  free(d);
  z->kind = 1;
  return 0;
bad:
  free(d);
  return -1;
}


static int zone_file (Zone *z, const char *name) {
  static const char *const dirs[] = {"/usr/share/zoneinfo", "/usr/lib/zoneinfo",
                                     "/usr/share/lib/zoneinfo"};
  const char *tzdir = var_get("TZDIR");
  int k;
  if (strstr(name, "..") != NULL || !*name) return -1;
  if (name[0] == '/') {
    char *native = path_to_native(name);
    int r = tzif_load(z, native);
    free(native);
    return r;
  }
  for (k = -1; k < 3; k++) {
    const char *dir = k < 0 ? tzdir : dirs[k];
    char *full, *native;
    int r;
    if (dir == NULL || !*dir) continue;
    full = xstrcat3(dir, "/", name);
    native = path_to_native(full);
    r = tzif_load(z, native);
    free(full);
    free(native);
    if (r == 0) return 0;
  }
  return -1;
}


/* the zone that TZ says now */
static Zone *zone_get (void) {
  const char *tz = g_tz_force, *s;
  Zone *z = &g_zone;
  size_t k = 0;
  if (tz == NULL) {
    int fl = var_flags("TZ");
    tz = (fl >= 0 && (fl & V_EXPORT)) ? var_get("TZ") : NULL;
  }
  if (g_zone_set) {
    if (tz == NULL && z->kind == 2) return z;
    if (tz != NULL && z->spec != NULL && strcmp(z->spec, tz) == 0) return z;
  }
  zone_clear(z);
  g_zone_set = 1;
  if (tz == NULL) {
    z->kind = 2;
    return z;
  }
  z->spec = xstrdup(tz);
  s = tz[0] == ':' ? tz + 1 : tz;
  if (*s == '\0') {
    strcpy(z->p.std, "UTC");
    return z;
  }
  if (zone_file(z, s) == 0) return z;
  z->kind = 0;
  if (tz[0] != ':' && parse_posix_tz(s, &z->p) == 0) return z;
  memset(&z->p, 0, sizeof(z->p));	/* a name alone (UTC, GMT ...): offset 0 */
  while (isalpha((unsigned char)s[k]) && k + 1 < sizeof(z->p.std)) {
    z->p.std[k] = s[k];
    k++;
  }
  z->p.std[k] = '\0';
  if (k < 3) strcpy(z->p.std, "UTC");
  return z;
}


static void zone_at (long long t, long *off, int *isdst, char *abbr, size_t n) {
  Zone *z = zone_get();
  const char *a = "";
  *off = 0;
  *isdst = 0;
  if (z->kind == 2) {
    os_localzone((time_t)t, off, isdst, abbr, n);
    return;
  }
  if (z->kind == 0) posix_at(&z->p, t, off, isdst, &a);
  else if (z->ntrans == 0 || t < z->trans[0]) {
    int k;
    for (k = 0; k < z->ntypes && z->types[k].isdst; k++) {}
    if (k == z->ntypes) k = 0;
    *off = z->types[k].off;
    *isdst = z->types[k].isdst;
    a = z->abbrs + z->types[k].abbr;
  }
  else if (t >= z->trans[z->ntrans - 1] && z->has_footer) posix_at(&z->p, t, off, isdst, &a);
  else {
    int lo = 0, hi = z->ntrans - 1;
    TzType *ty;
    while (lo < hi) {	/* the last transition at or before t */
      int mid = (lo + hi + 1) / 2;
      if (z->trans[mid] <= t) lo = mid;
      else hi = mid - 1;
    }
    ty = &z->types[z->tidx[lo]];
    *off = ty->off;
    *isdst = ty->isdst;
    a = z->abbrs + ty->abbr;
  }
  snprintf(abbr, n, "%s", a);
}


static void dt_make (DT *d, long long t, long nsec) {
  long long days, secs;
  memset(d, 0, sizeof(*d));
  d->t = t;
  d->nsec = nsec;
  zone_at(t, &d->gmtoff, &d->isdst, d->zone, sizeof(d->zone));
  secs = t + d->gmtoff;
  days = floor_div(secs, 86400);
  secs -= days * 86400;
  civil_from_days(days, &d->year, &d->mon, &d->mday);
  d->hour = (int)(secs / 3600);
  d->min = (int)(secs / 60 % 60);
  d->sec = (int)(secs % 60);
  d->wday = weekday_of(days);
  d->yday = (int)(days - days_from_civil(d->year, 1, 1));
}


static long zone_off (long long t) {
  long off;
  int dst;
  char abbr[32];
  zone_at(t, &off, &dst, abbr, sizeof(abbr));
  return off;
}


/* local wall clock seconds (counted as if UTC) -> the moment */
static long long local_to_t (long long wall) {
  long off1 = zone_off(wall), off2;
  long long t = wall - off1;
  off2 = zone_off(t);
  if (off2 != off1) {
    long long t2 = wall - off2;
    if (zone_off(t2) == off2) t = t2;
  }
  return t;
}


static void now_time (long long *t, long *ns) {
  long long us = os_now_us();
  *t = floor_div(us, 1000000);
  *ns = (long)(us - *t * 1000000) * 1000;
}

/* }================================================================== */


/*
** {==================================================================
** strftime, GNU style: flags - _ 0 ^ #, widths, %N %:z %::z %:::z %q %P
** ===================================================================
*/

static const char *const wday_names[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                         "Thursday", "Friday", "Saturday"};
static const char *const mon_names[] = {"January", "February", "March", "April", "May",
                                        "June", "July", "August", "September", "October",
                                        "November", "December"};


static void put_pad (Buf *b, int n, char c) {
  while (n-- > 0) buf_putc(b, c);
}


static void put_num (Buf *b, long long v, int defw, char defpad, int pad, int width) {
  char digits[32];
  int len, w = width >= 0 ? width : defw;
  char pc = pad == '_' ? ' ' : (pad == '0' || pad == '+') ? '0' : defpad;
  unsigned long long u = v < 0 ? (unsigned long long)(-(v + 1)) + 1 : (unsigned long long)v;
  sprintf(digits, "%llu", u);
  len = (int)strlen(digits) + (v < 0);
  if (pad == '-') w = 0;
  if (pc == ' ') {
    put_pad(b, w - len, ' ');
    if (v < 0) buf_putc(b, '-');
  }
  else {
    if (v < 0) buf_putc(b, '-');
    put_pad(b, w - len, '0');
  }
  buf_puts(b, digits);
}


/* cas: 1 upper, -1 lower */
static void put_str (Buf *b, const char *s, int pad, int width, int cas) {
  int len = (int)strlen(s);
  if (width > len && pad != '-') put_pad(b, width - len, pad == '0' ? '0' : ' ');
  for (; *s; s++)
    buf_putc(b, (char)(cas > 0 ? toupper((unsigned char)*s) : cas < 0 ? tolower((unsigned char)*s) : *s));
}


static int iso_weeks (long long y) {	/* 52 or 53 */
  int jan1 = weekday_of(days_from_civil(y, 1, 1));
  return (jan1 == 4 || (is_leap(y) && jan1 == 3)) ? 53 : 52;
}


static void iso_week (const DT *d, long long *year, int *week) {
  int wd = d->wday == 0 ? 7 : d->wday;
  int w = (d->yday + 1 - wd + 10) / 7;
  *year = d->year;
  if (w < 1) {
    *year = d->year - 1;
    w = iso_weeks(*year);
  }
  else if (w > iso_weeks(d->year)) {
    *year = d->year + 1;
    w = 1;
  }
  *week = w;
}


static void fmt_time (Buf *b, const char *f, const DT *d) {
  for (; *f; f++) {
    const char *start = f;
    int pad = 0, upper = 0, swap = 0, width = -1, colons = 0;
    int h12 = d->hour % 12 == 0 ? 12 : d->hour % 12;
    char sub[64];
    if (*f != '%') {
      buf_putc(b, *f);
      continue;
    }
    for (f++;; f++) {
      if (*f == '-' || *f == '_' || *f == '0' || *f == '+') pad = *f;
      else if (*f == '^') upper = 1;
      else if (*f == '#') swap = 1;
      else break;
    }
    if (isdigit((unsigned char)*f)) {
      width = 0;
      while (isdigit((unsigned char)*f)) width = width * 10 + (*f++ - '0');
    }
    if (*f == 'E' || *f == 'O') f++;
    while (*f == ':') {
      colons++;
      f++;
    }
    if (colons && *f != 'z') {	/* only %:z and friends take colons */
      buf_putn(b, start, (size_t)(f - start) + (*f != '\0'));
      if (*f == '\0') break;
      continue;
    }
    switch (*f) {
      case 'a': case 'A': case 'b': case 'h': case 'B': {
        char s[16];
        const char *full = (*f == 'a' || *f == 'A') ? wday_names[d->wday] : mon_names[d->mon - 1];
        snprintf(s, sizeof(s), "%s", full);
        if (*f != 'A' && *f != 'B') s[3] = '\0';
        put_str(b, s, pad, width, upper || swap ? 1 : 0);
        break;
      }
      case 'c': case 'D': case 'F': case 'r': case 'R': case 'T': case 'x': case 'X': {
        Buf t;
        const char *sf = *f == 'c' ? "%a %b %e %H:%M:%S %Y" : *f == 'D' || *f == 'x' ? "%m/%d/%y" :
                         *f == 'F' ? "-%m-%d" : *f == 'r' ? "%I:%M:%S %p" :
                         *f == 'R' ? "%H:%M" : "%H:%M:%S";
        buf_init(&t);
        if (*f == 'F') put_num(&t, d->year, 4, '0', 0, -1);	/* 4 digits at least */
        fmt_time(&t, sf, d);
        put_str(b, t.s ? t.s : "", pad, width, upper ? 1 : 0);
        buf_free(&t);
        break;
      }
      case 'C': put_num(b, floor_div(d->year, 100), 2, '0', pad, width); break;
      case 'd': put_num(b, d->mday, 2, '0', pad, width); break;
      case 'e': put_num(b, d->mday, 2, ' ', pad, width); break;
      case 'g': case 'G': case 'V': {
        long long iy;
        int iw;
        iso_week(d, &iy, &iw);
        if (*f == 'V') put_num(b, iw, 2, '0', pad, width);
        else if (*f == 'g') put_num(b, (iy % 100 + 100) % 100, 2, '0', pad, width);
        else put_num(b, iy, 1, '0', pad, width);
        break;
      }
      case 'H': put_num(b, d->hour, 2, '0', pad, width); break;
      case 'I': put_num(b, h12, 2, '0', pad, width); break;
      case 'j': put_num(b, d->yday + 1, 3, '0', pad, width); break;
      case 'k': put_num(b, d->hour, 2, ' ', pad, width); break;
      case 'l': put_num(b, h12, 2, ' ', pad, width); break;
      case 'm': put_num(b, d->mon, 2, '0', pad, width); break;
      case 'M': put_num(b, d->min, 2, '0', pad, width); break;
      case 'n': buf_putc(b, '\n'); break;
      case 't': buf_putc(b, '\t'); break;
      case '%': buf_putc(b, '%'); break;
      case 'N': {	/* a width cuts the digits: %3N milliseconds */
        char ns[16];
        int w = width > 0 ? width : 9;
        sprintf(ns, "%09ld", d->nsec);
        if (w <= 9) buf_putn(b, ns, (size_t)w);
        else {
          buf_puts(b, ns);
          put_pad(b, w - 9, '0');
        }
        break;
      }
      case 'p': put_str(b, d->hour < 12 ? "AM" : "PM", pad, width, swap ? -1 : 0); break;
      case 'P': put_str(b, d->hour < 12 ? "am" : "pm", pad, width, upper ? 1 : 0); break;
      case 'q': put_num(b, (d->mon - 1) / 3 + 1, 1, '0', pad, width); break;
      case 's': put_num(b, d->t, 1, '0', pad, width); break;
      case 'S': put_num(b, d->sec, 2, '0', pad, width); break;
      case 'u': put_num(b, d->wday == 0 ? 7 : d->wday, 1, '0', pad, width); break;
      case 'w': put_num(b, d->wday, 1, '0', pad, width); break;
      case 'U': put_num(b, (d->yday + 7 - d->wday) / 7, 2, '0', pad, width); break;
      case 'W': put_num(b, (d->yday + 7 - (d->wday + 6) % 7) / 7, 2, '0', pad, width); break;
      case 'y': put_num(b, (d->year % 100 + 100) % 100, 2, '0', pad, width); break;
      case 'Y': put_num(b, d->year, 1, '0', pad, width); break;
      case 'z': {
        long off = d->gmtoff, a = off < 0 ? -off : off;
        int hh = (int)(a / 3600), mm = (int)(a / 60 % 60), ss = (int)(a % 60);
        char sign = off < 0 ? '-' : '+';
        if (colons == 0) sprintf(sub, "%c%02d%02d", sign, hh, mm);
        else if (colons == 1) sprintf(sub, "%c%02d:%02d", sign, hh, mm);
        else if (colons == 2 || (colons == 3 && ss)) sprintf(sub, "%c%02d:%02d:%02d", sign, hh, mm, ss);
        else if (colons == 3 && mm) sprintf(sub, "%c%02d:%02d", sign, hh, mm);
        else if (colons == 3) sprintf(sub, "%c%02d", sign, hh);
        else {
          buf_putn(b, start, (size_t)(f - start) + 1);
          break;
        }
        put_str(b, sub, pad, width, 0);
        break;
      }
      case 'Z': put_str(b, d->zone, pad, width, upper ? 1 : swap ? -1 : 0); break;
      case '\0':	/* a % at the end */
        put_str(b, start, pad, width, 0);
        f--;
        break;
      default: {	/* not ours: as it is */
        char *lit = xstrndup(start, (size_t)(f - start) + 1);
        put_str(b, lit, pad, width, 0);
        free(lit);
        break;
      }
    }
  }
}

/* }================================================================== */


/*
** {==================================================================
** date -d: the inputs people use (the common part of GNU's grammar)
** ===================================================================
*/

typedef struct DTok {
  char type;	/* 'n' number, 'w' word, 'c' other character, 0 the end */
  long long v;
  int digits, sign;	/* sign: 0 none, 1 '+', -1 '-' */
  long ns;	/* a fraction after the number */
  int has_frac;
  char word[32];
  char c;
} DTok;

typedef struct DParse {
  DTok *tok;
  int n, i;
  int have_date, have_year, have_time, have_zone, have_day, have_rel;
  long long year;
  int mon, mday, hour, min, sec, meridian;	/* 0 none, 1 am, 2 pm */
  long ns, zone_off;
  int day_num, day_ord;
  long long rel[6];	/* years months days hours minutes seconds */
  int last_unit;
  long long last_amount;
} DParse;


static int lex_date (const char *s, DTok **out) {
  int n = 0, cap = 16;
  DTok *t = (DTok *)xmalloc((size_t)cap * sizeof(DTok));
  for (;;) {
    DTok *k;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '(') {	/* a comment */
      int depth = 0;
      for (; *s; s++) {
        if (*s == '(') depth++;
        else if (*s == ')' && --depth == 0) {
          s++;
          break;
        }
      }
      continue;
    }
    if (n + 1 >= cap) {
      cap *= 2;
      t = (DTok *)xrealloc(t, (size_t)cap * sizeof(DTok));
    }
    k = &t[n++];
    memset(k, 0, sizeof(*k));
    if (*s == '\0') break;
    if (*s == '+' || *s == '-') {	/* a sign, maybe apart from its number */
      const char *d = s + 1;
      while (isspace((unsigned char)*d)) d++;
      if (isdigit((unsigned char)*d)) {
        k->sign = *s == '-' ? -1 : 1;
        s = d;
      }
    }
    if (isdigit((unsigned char)*s)) {
      k->type = 'n';
      while (isdigit((unsigned char)*s)) {
        if (k->v < 100000000000000000LL) k->v = k->v * 10 + (*s - '0');
        k->digits++;
        s++;
      }
      if ((*s == '.' || *s == ',') && isdigit((unsigned char)s[1])) {
        int dd = 0;
        k->has_frac = 1;
        for (s++; isdigit((unsigned char)*s); s++, dd++)
          if (dd < 9) k->ns = k->ns * 10 + (*s - '0');
        for (; dd < 9; dd++) k->ns *= 10;
      }
      if (k->sign < 0) k->v = -k->v;
    }
    else if (isalpha((unsigned char)*s)) {
      size_t w = 0;
      k->type = 'w';
      for (; isalpha((unsigned char)*s) || (*s == '.' && isalpha((unsigned char)s[1])); s++)
        if (*s != '.' && w + 1 < sizeof(k->word)) k->word[w++] = (char)tolower((unsigned char)*s);
      if (*s == '.') s++;
      k->word[w] = '\0';
    }
    else {
      k->type = 'c';
      k->c = *s++;
    }
  }
  *out = t;
  return n - 1;
}


/* a name, its 3 letter form, or a listed short form */
static int name_index (const char *w, const char *const *names) {
  int k;
  for (k = 0; names[k]; k++)
    if (strcmp(w, names[k]) == 0 || (strlen(w) == 3 && strncmp(w, names[k], 3) == 0)) return k;
  return -1;
}


static int month_word (const char *w) {
  static const char *const m[] = {"january", "february", "march", "april", "may", "june", "july",
                                  "august", "september", "october", "november", "december", NULL};
  if (strcmp(w, "sept") == 0) return 9;
  return name_index(w, m) + 1;
}


static int day_word (const char *w) {
  static const char *const d[] = {"sunday", "monday", "tuesday", "wednesday", "thursday",
                                  "friday", "saturday", NULL};
  if (strcmp(w, "tues") == 0) return 2;
  if (strcmp(w, "wednes") == 0) return 3;
  if (strcmp(w, "thur") == 0 || strcmp(w, "thurs") == 0) return 4;
  return name_index(w, d);
}


/* unit words: the index into rel[] and how many of it */
static int unit_word (const char *w, int *unit, long long *mult) {
  static const struct { const char *name; int unit; long long mult; } u[] = {
    {"year", 0, 1}, {"years", 0, 1}, {"month", 1, 1}, {"months", 1, 1},
    {"fortnight", 2, 14}, {"fortnights", 2, 14}, {"week", 2, 7}, {"weeks", 2, 7},
    {"day", 2, 1}, {"days", 2, 1}, {"hour", 3, 1}, {"hours", 3, 1},
    {"minute", 4, 1}, {"minutes", 4, 1}, {"min", 4, 1}, {"mins", 4, 1},
    {"second", 5, 1}, {"seconds", 5, 1}, {"sec", 5, 1}, {"secs", 5, 1}, {NULL, 0, 0}};
  int k;
  for (k = 0; u[k].name; k++)
    if (strcmp(w, u[k].name) == 0) {
      *unit = u[k].unit;
      *mult = u[k].mult;
      return 1;
    }
  return 0;
}


static int ordinal_word (const char *w, int *ord) {
  static const char *const o[] = {"last", "this", "next", "first", "", "third", "fourth", "fifth",
                                  "sixth", "seventh", "eighth", "ninth", "tenth", "eleventh",
                                  "twelfth", NULL};
  int k;
  for (k = 0; o[k]; k++)
    if (o[k][0] && strcmp(w, o[k]) == 0) {
      *ord = k - 1;
      return 1;
    }
  return 0;
}


static int zone_word (const char *w, long *off) {
  static const struct { const char *name; int mins; } z[] = {
    {"utc", 0}, {"gmt", 0}, {"ut", 0}, {"z", 0}, {"wet", 0}, {"west", 60}, {"bst", 60},
    {"cet", 60}, {"met", 60}, {"cest", 120}, {"mest", 120}, {"eet", 120}, {"eest", 180},
    {"msk", 180}, {"ist", 330}, {"hkt", 480}, {"sgt", 480}, {"pht", 480}, {"awst", 480},
    {"jst", 540}, {"kst", 540}, {"acst", 570}, {"aest", 600}, {"aedt", 660}, {"nzst", 720},
    {"nzdt", 780}, {"ast", -240}, {"adt", -180}, {"est", -300}, {"edt", -240},
    {"cst", -360}, {"cdt", -300}, {"mst", -420}, {"mdt", -360}, {"pst", -480},
    {"pdt", -420}, {"akst", -540}, {"akdt", -480}, {"hst", -600}, {NULL, 0}};
  int k;
  for (k = 0; z[k].name; k++)
    if (strcmp(w, z[k].name) == 0) {
      *off = z[k].mins * 60L;
      return 1;
    }
  return 0;
}


static DTok *pk (DParse *p, int k) {	/* token i+k, or the end */
  int i = p->i + k;
  return &p->tok[i < p->n && i >= 0 ? i : p->n];
}


static int is_char (const DTok *t, char c) {
  return t->type == 'c' && t->c == c;
}


static int is_meridian (const DTok *t) {
  return t->type == 'w' && (strcmp(t->word, "am") == 0 || strcmp(t->word, "pm") == 0);
}


static void add_rel (DParse *p, int unit, long long amount) {
  p->rel[unit] += amount;
  p->last_unit = unit;
  p->last_amount = amount;
  p->have_rel = 1;
}


static long long year_of (const DTok *t) {	/* two digits: 69..99 -> 19xx, else 20xx */
  long long v = t->v < 0 ? -t->v : t->v;
  if (t->digits == 2) return v < 69 ? v + 2000 : v + 1900;
  return v;
}


/* +hh, +hhmm, +hh:mm after a time or a zone name */
static int zone_number (DParse *p, long *off) {
  DTok *t = pk(p, 0);
  long long v = t->v < 0 ? -t->v : t->v;
  long secs;
  if (t->type != 'n' || t->sign == 0) return 0;
  p->i++;
  if (is_char(pk(p, 0), ':') && pk(p, 1)->type == 'n' && pk(p, 1)->sign == 0) {
    secs = (long)(v * 3600 + pk(p, 1)->v * 60);
    p->i += 2;
  }
  else if (t->digits <= 2) secs = (long)(v * 3600);
  else secs = (long)((v / 100) * 3600 + (v % 100) * 60);
  *off = t->sign < 0 ? -secs : secs;
  return 1;
}


static int parse_word (DParse *p, const char *w, const char *local_std, const char *local_dst) {
  int unit, m, ord;
  long long mult;
  long off;
  if (strcmp(w, "now") == 0 || strcmp(w, "today") == 0) p->have_rel = 1;
  else if (strcmp(w, "tomorrow") == 0) add_rel(p, 2, 1);
  else if (strcmp(w, "yesterday") == 0) add_rel(p, 2, -1);
  else if (strcmp(w, "ago") == 0) {	/* the unit right before it goes back */
    if (p->last_unit < 0) return -1;
    p->rel[p->last_unit] -= 2 * p->last_amount;
    p->last_amount = -p->last_amount;
  }
  else if (ordinal_word(w, &ord)) {	/* next week, last friday */
    DTok *n = pk(p, 0);
    if (n->type == 'w' && unit_word(n->word, &unit, &mult)) add_rel(p, unit, ord * mult);
    else if (n->type == 'w' && (m = day_word(n->word)) >= 0) {
      if (p->have_day) return -1;
      p->have_day = 1;
      p->day_num = m;
      p->day_ord = ord;
    }
    else return -1;
    p->i++;
  }
  else if (unit_word(w, &unit, &mult)) add_rel(p, unit, mult);
  else if ((m = day_word(w)) >= 0) {
    if (p->have_day) return -1;
    p->have_day = 1;
    p->day_num = m;
    p->day_ord = 0;
  }
  else if ((m = month_word(w)) > 0) {	/* Jan 5, Jan 5 2021, Jan 5, 2021 */
    DTok *n = pk(p, 0);
    if (p->have_date) return -1;
    p->have_date = 1;
    p->mon = m;
    p->mday = 1;
    if (n->type == 'n' && n->sign == 0 && !is_char(pk(p, 1), ':')) {
      p->mday = (int)n->v;
      p->i++;
      if (is_char(pk(p, 0), ',')) p->i++;
      n = pk(p, 0);
      if (n->type == 'n' && n->sign == 0 && n->digits >= 3 && !is_char(pk(p, 1), ':')) {
        p->year = year_of(n);
        p->have_year = 1;
        p->i++;
      }
    }
    else if (n->type == 'n' && n->sign < 0) {	/* Jan-2021 */
      p->year = year_of(n);
      p->have_year = 1;
      p->i++;
    }
    else return -1;
  }
  else if (strcmp(w, "am") == 0 || strcmp(w, "pm") == 0) {
    if (!p->have_time || p->meridian || p->hour < 1 || p->hour > 12) return -1;
    p->meridian = w[0] == 'a' ? 1 : 2;
  }
  else if (zone_word(w, &off)) {	/* UTC, PST, UTC+3 */
    long extra;
    if (p->have_zone) return -1;
    p->have_zone = 1;
    p->zone_off = off;
    if (zone_number(p, &extra)) p->zone_off += extra;
  }
  else if ((local_std[0] && m_stricmp(w, local_std) == 0) ||
           (local_dst[0] && m_stricmp(w, local_dst) == 0)) {
    /* our own zone's name, as date prints it: local time */
  }
  else if (!(strcmp(w, "t") == 0 && p->have_date && pk(p, 0)->type == 'n'))	/* 2021-03-04T05:06 */
    return -1;
  return 0;
}


static int parse_items (DParse *p, const char *local_std, const char *local_dst) {
  while (p->i < p->n) {
    DTok *t = pk(p, 0);
    int unit;
    long long mult;
    if (t->type == 'c') {
      if (t->c != ',') return -1;
      p->i++;
      continue;
    }
    if (t->type == 'w') {
      p->i++;
      if (parse_word(p, t->word, local_std, local_dst) != 0) return -1;
      continue;
    }
    if (t->sign == 0 && is_char(pk(p, 1), ':') && pk(p, 2)->type == 'n' && pk(p, 2)->sign == 0) {
      if (p->have_time) return -1;	/* HH:MM[:SS[.frac]] [am|pm] [+zone] */
      p->have_time = 1;
      p->hour = (int)t->v;
      p->min = (int)pk(p, 2)->v;
      p->sec = 0;
      p->ns = 0;
      p->i += 3;
      if (pk(p, -1)->has_frac) return -1;
      if (is_char(pk(p, 0), ':') && pk(p, 1)->type == 'n' && pk(p, 1)->sign == 0) {
        p->sec = (int)pk(p, 1)->v;
        p->ns = pk(p, 1)->ns;
        p->i += 2;
      }
      if (is_meridian(pk(p, 0))) {
        if (p->hour < 1 || p->hour > 12) return -1;
        p->meridian = pk(p, 0)->word[0] == 'a' ? 1 : 2;
        p->i++;
      }
      if (pk(p, 0)->type == 'n' && pk(p, 0)->sign != 0) {
        if (p->have_zone) return -1;
        zone_number(p, &p->zone_off);
        p->have_zone = 1;
      }
      continue;
    }
    if (t->sign == 0 && pk(p, 1)->type == 'n' && pk(p, 1)->sign < 0 && pk(p, 2)->type == 'n' &&
        pk(p, 2)->sign < 0 && !pk(p, 1)->has_frac) {	/* YYYY-MM-DD */
      if (p->have_date) return -1;
      p->have_date = p->have_year = 1;
      p->year = year_of(t);
      p->mon = (int)-pk(p, 1)->v;
      p->mday = (int)-pk(p, 2)->v;
      p->i += 3;
      continue;
    }
    if (t->sign == 0 && is_char(pk(p, 1), '/') && pk(p, 2)->type == 'n' && pk(p, 2)->sign == 0) {
      if (p->have_date) return -1;
      p->have_date = 1;
      if (is_char(pk(p, 3), '/') && pk(p, 4)->type == 'n' && pk(p, 4)->sign == 0) {
        p->have_year = 1;
        if (t->digits >= 4) {	/* Y/M/D */
          p->year = year_of(t);
          p->mon = (int)pk(p, 2)->v;
          p->mday = (int)pk(p, 4)->v;
        }
        else {	/* M/D/Y */
          p->mon = (int)t->v;
          p->mday = (int)pk(p, 2)->v;
          p->year = year_of(pk(p, 4));
        }
        p->i += 5;
      }
      else {	/* M/D */
        p->mon = (int)t->v;
        p->mday = (int)pk(p, 2)->v;
        p->i += 3;
      }
      continue;
    }
    if (t->sign == 0 && (pk(p, 1)->type == 'w' || (is_char(pk(p, 1), '-') && pk(p, 2)->type == 'w')) &&
        month_word(is_char(pk(p, 1), '-') ? pk(p, 2)->word : pk(p, 1)->word) > 0) {
      int dash = is_char(pk(p, 1), '-');	/* 5 jan [2021], 5-jan-2021 */
      DTok *n;
      if (p->have_date) return -1;
      p->have_date = 1;
      p->mday = (int)t->v;
      p->mon = month_word(pk(p, 1 + dash)->word);
      p->i += 2 + dash;
      n = pk(p, 0);
      if (n->type == 'n' && (n->sign < 0 || (n->sign == 0 && n->digits >= 3 && !is_char(pk(p, 1), ':')))) {
        p->year = year_of(n);
        p->have_year = 1;
        p->i++;
      }
      continue;
    }
    if (pk(p, 1)->type == 'w' && unit_word(pk(p, 1)->word, &unit, &mult)) {	/* 3 days, -2 weeks */
      add_rel(p, unit, t->v * mult);
      p->i += 2;
      continue;
    }
    if (t->sign == 0 && is_meridian(pk(p, 1))) {	/* 3pm */
      if (p->have_time || t->v < 1 || t->v > 12) return -1;
      p->have_time = 1;
      p->hour = (int)t->v;
      p->min = p->sec = 0;
      p->ns = 0;
      p->meridian = pk(p, 1)->word[0] == 'a' ? 1 : 2;
      p->i += 2;
      continue;
    }
    if (t->sign != 0 || t->has_frac) return -1;
    p->i++;	/* a number alone */
    if (p->have_date && !p->have_year && !p->have_rel && (p->have_time || t->digits > 2)) {
      p->year = year_of(t);
      p->have_year = 1;
    }
    else if (t->digits > 4) {	/* YYYYMMDD */
      if (p->have_date) return -1;
      p->have_date = p->have_year = 1;
      p->year = t->v / 10000;
      p->mon = (int)(t->v / 100 % 100);
      p->mday = (int)(t->v % 100);
    }
    else if (!p->have_time) {	/* HH or HHMM */
      p->have_time = 1;
      p->hour = (int)(t->digits <= 2 ? t->v : t->v / 100);
      p->min = (int)(t->digits <= 2 ? 0 : t->v % 100);
      p->sec = 0;
      p->ns = 0;
    }
    else return -1;
  }
  return 0;
}


/* 0 and *t, *ns; -1: not a date */
static int parse_date (const char *s, long long *t, long *ns) {
  DParse p;
  DT now, jan, jul;
  long long nt, year, wall, days;
  long nns;
  int mon, mday, hour, min, sec, r;
  char std[32], dst[32];
  while (isspace((unsigned char)*s)) s++;
  if (*s == '@') {	/* @SECONDS[.frac] */
    const char *x = s + 1;
    int neg = 0, dd = 0;
    long long v = 0;
    long f = 0;
    while (isspace((unsigned char)*x)) x++;
    if (*x == '+' || *x == '-') neg = *x++ == '-';
    if (!isdigit((unsigned char)*x)) return -1;
    while (isdigit((unsigned char)*x)) v = v * 10 + (*x++ - '0');
    if (*x == '.' || *x == ',') {
      for (x++; isdigit((unsigned char)*x); x++, dd++)
        if (dd < 9) f = f * 10 + (*x - '0');
      for (; dd < 9; dd++) f *= 10;
    }
    while (isspace((unsigned char)*x)) x++;
    if (*x) return -1;
    if (neg) {
      v = -v;
      if (f) {
        v--;
        f = 1000000000L - f;
      }
    }
    *t = v;
    *ns = f;
    return 0;
  }
  memset(&p, 0, sizeof(p));
  p.last_unit = -1;
  p.n = lex_date(s, &p.tok);
  now_time(&nt, &nns);
  dt_make(&now, nt, nns);
  dt_make(&jan, days_from_civil(now.year, 1, 15) * 86400, 0);	/* the zone's own names */
  dt_make(&jul, days_from_civil(now.year, 7, 15) * 86400, 0);
  snprintf(std, sizeof(std), "%s", jan.isdst ? jul.zone : jan.zone);
  snprintf(dst, sizeof(dst), "%s", jan.isdst ? jan.zone : jul.zone);
  r = parse_items(&p, std, dst);
  free(p.tok);
  if (r != 0) return -1;
  year = p.have_year ? p.year : now.year;
  mon = p.have_date ? p.mon : now.mon;
  mday = p.have_date ? p.mday : now.mday;
  if (mon < 1 || mon > 12 || mday < 1 || mday > month_days(year, mon)) return -1;
  if (p.have_time) {
    hour = p.hour;
    if (p.meridian) hour = hour % 12 + (p.meridian == 2 ? 12 : 0);
    min = p.min;
    sec = p.sec;
    nns = p.ns;
    if (hour > 23 || min > 59 || sec > 60) return -1;
    if (sec == 60) sec = 59;
  }
  else if (p.have_rel && !p.have_date && !p.have_day) {	/* now, 3 days: this time of day */
    hour = now.hour;
    min = now.min;
    sec = now.sec;
  }
  else {
    hour = min = sec = 0;
    nns = 0;
  }
  if (p.have_day && !p.have_date) {	/* monday, next friday, last sun */
    int wd = weekday_of(days_from_civil(year, mon, mday));
    mday += (p.day_num - wd + 7) % 7 + 7 * (p.day_ord - (0 < p.day_ord && wd != p.day_num));
  }
  {	/* years, months, days on the calendar, normalized like mktime */
    long long y = year + p.rel[0], m0 = (long long)(mon - 1) + p.rel[1];
    y += floor_div(m0, 12);
    m0 -= floor_div(m0, 12) * 12;
    days = days_from_civil(y, (int)m0 + 1, 1) + mday - 1 + p.rel[2];
  }
  wall = days * 86400 + hour * 3600L + min * 60L + sec;
  *t = p.have_zone ? wall - p.zone_off : local_to_t(wall);
  *t += p.rel[3] * 3600 + p.rel[4] * 60 + p.rel[5];
  *ns = nns;
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** date
** ===================================================================
*/

static int arg_kind (const char *arg, const char *const *names) {
  int k, hit = -1;
  size_t n = strlen(arg);
  for (k = 0; names[k]; k++) {
    if (strcmp(arg, names[k]) == 0) return k;
    if (n > 0 && strncmp(arg, names[k], n) == 0) {
      if (hit >= 0) return -2;
      hit = k;
    }
  }
  return hit;
}


static int bad_arg (int err, const char *arg, const char *opt, const char *const *names) {
  int k;
  tool_err(err, "date", "%s argument %s for '%s'", arg_kind(arg, names) == -2 ? "ambiguous" : "invalid",
           q(arg), opt);
  fd_puts(err, "Valid arguments are:\n");
  for (k = 0; names[k]; k++) fd_printf(err, "  - '%s'\n", names[k]);
  return try_help(err, "date", 1);
}


static void date_print (Out *o, const char *fmt, long long t, long ns) {
  DT d;
  Buf b;
  dt_make(&d, t, ns);
  buf_init(&b);
  fmt_time(&b, fmt, &d);
  if (b.s) out_putn(o, b.s, b.len);
  out_putc(o, '\n');
  buf_free(&b);
}


/* date MMDDhhmm[[CC]YY][.ss] */
static int parse_set (const char *s, long long *t, long *ns) {
  size_t n = strspn(s, "0123456789");
  long long now_t, year;
  long now_ns;
  int v[6], k, sec = 0, mon, mday, hour, min;
  DT now;
  if ((n != 8 && n != 10 && n != 12) ||
      (s[n] != '\0' && !(s[n] == '.' && isdigit((unsigned char)s[n + 1]) &&
                         isdigit((unsigned char)s[n + 2]) && s[n + 3] == '\0')))
    return -1;
  for (k = 0; k < (int)n / 2; k++) v[k] = (s[2 * k] - '0') * 10 + (s[2 * k + 1] - '0');
  mon = v[0];
  mday = v[1];
  hour = v[2];
  min = v[3];
  now_time(&now_t, &now_ns);
  dt_make(&now, now_t, 0);
  year = n == 8 ? now.year : n == 10 ? (v[4] < 69 ? 2000 + v[4] : 1900 + v[4]) : v[4] * 100 + v[5];
  if (s[n] == '.') sec = (s[n + 1] - '0') * 10 + (s[n + 2] - '0');
  if (mon < 1 || mon > 12 || mday < 1 || mday > month_days(year, mon) || hour > 23 || min > 59 || sec > 60)
    return -1;
  *t = local_to_t(days_from_civil(year, mon, mday) * 86400 + hour * 3600L + min * 60L + sec);
  *ns = 0;
  return 0;
}


static int date_run (Opts *g, int in, int out, int err) {
  static const char *const iso_names[] = {"hours", "minutes", "date", "seconds", "ns", NULL};
  static const char *const iso_fmts[] = {"%Y-%m-%dT%H%:z", "%Y-%m-%dT%H:%M%:z", "%Y-%m-%d",
                                         "%Y-%m-%dT%H:%M:%S%:z", "%Y-%m-%dT%H:%M:%S,%N%:z"};
  static const char *const rfc_names[] = {"date", "seconds", "ns", NULL};
  static const char *const rfc_fmts[] = {"%Y-%m-%d", "%Y-%m-%d %H:%M:%S%:z",
                                         "%Y-%m-%d %H:%M:%S.%N%:z"};
  LongOpt lo[] = {{"date", 'd', 1}, {"file", 'f', 1}, {"reference", 'r', 1}, {"utc", 'u', 0},
                  {"universal", 'u', 0}, {"rfc-email", 'R', 0}, {"rfc-2822", 'R', 0},
                  {"rfc-822", 'R', 0}, {"iso-8601", 'I', 2}, {"rfc-3339", 'T', 1},
                  {"set", 's', 1}, {"debug", 'D', 0}, {"version", OPT_VERSION, 0}, {NULL, 0, 0}};
  int c, status = 0;
  const char *date_s = NULL, *file = NULL, *ref = NULL, *fmt = NULL, *set = NULL;
  long long t;
  long ns;
  Out o;
  while ((c = opts_next(g, "d:f:r:uRIs:", lo)) != 0) {
    switch (c) {
      case 'd': date_s = g->arg; break;
      case 'f': file = g->arg; break;
      case 'r': ref = g->arg; break;
      case 'u': g_tz_force = "UTC0"; break;
      case 's': set = g->arg; break;
      case 'D': break;
      case 'R': case 'I': case 'T': {
        const char *a = g->arg;
        int k = 0;
        if (c == 'I' && a == NULL && g->cluster && *g->cluster) {	/* -Iseconds */
          a = g->cluster;
          g->cluster = NULL;
        }
        if (c == 'I' && a == NULL) a = "date";
        if (c != 'R' && (k = arg_kind(a, c == 'I' ? iso_names : rfc_names)) < 0)
          return bad_arg(err, a, c == 'I' ? "--iso-8601" : "--rfc-3339", c == 'I' ? iso_names : rfc_names);
        if (fmt) {
          tool_err(err, "date", "multiple output formats specified");
          return 1;
        }
        fmt = c == 'R' ? "%a, %d %b %Y %H:%M:%S %z" : c == 'I' ? iso_fmts[k] : rfc_fmts[k];
        break;
      }
      case OPT_HELP: return tool_help(out, "date");
      case OPT_VERSION: return tool_version(out, "date");
      default: return 1;
    }
  }
  if (g->ops.n > 0) {
    const char *a = g->ops.v[0];
    if (a[0] == '+') {
      if (fmt) {
        tool_err(err, "date", "multiple output formats specified");
        return 1;
      }
      fmt = a + 1;
      if (g->ops.n > 1) {
        tool_err(err, "date", "extra operand %s", q(g->ops.v[1]));
        return try_help(err, "date", 1);
      }
    }
    else if (date_s || file || ref || set) {
      tool_err(err, "date", "the argument %s lacks a leading '+';\nwhen using an option to specify "
               "date(s), any non-option\nargument must be a format string beginning with '+'", q(a));
      return try_help(err, "date", 1);
    }
    else if (g->ops.n > 1) {
      tool_err(err, "date", "extra operand %s", q(g->ops.v[1]));
      return try_help(err, "date", 1);
    }
  }
  if ((date_s != NULL) + (file != NULL) + (ref != NULL) > 1) {
    tool_err(err, "date", "the options to specify dates for printing are mutually exclusive");
    return try_help(err, "date", 1);
  }
  if (fmt == NULL) fmt = "%a %b %e %H:%M:%S %Z %Y";
  out_init(&o, out);
  if (set != NULL || (g->ops.n > 0 && g->ops.v[0][0] != '+')) {
    /* setting the clock: not ours to do, but the date is shown, as GNU does */
    int ok = set ? parse_date(set, &t, &ns) == 0 : parse_set(g->ops.v[0], &t, &ns) == 0;
    if (!ok) {
      tool_err(err, "date", "invalid date %s", q(set ? set : g->ops.v[0]));
      return 1;
    }
    tool_err(err, "date", "cannot set date: Operation not permitted");
    date_print(&o, fmt, t, ns);
    out_flush(&o);
    return 1;
  }
  if (file != NULL) {	/* -f FILE: a date on each line */
    In r;
    char *line;
    size_t len;
    if (in_open(&r, file, in) != 0) {
      tool_err(err, "date", "%s: %s", file, os_errmsg());
      return 1;
    }
    while (in_line(&r, &line, &len, '\n', NULL)) {
      if (len > 0 && line[len - 1] == '\r') line[--len] = '\0';
      if (parse_date(line, &t, &ns) == 0) date_print(&o, fmt, t, ns);
      else {
        out_flush(&o);
        tool_err(err, "date", "invalid date %s", q(line));
        status = 1;
      }
    }
    in_close(&r);
  }
  else {
    if (ref != NULL) {
      OsStat st;
      OsStatX x;
      char *native = path_to_native(ref);
      int bad = os_stat_x(native, 1, &st, &x) != 0;
      free(native);
      if (bad) {
        tool_err(err, "date", "%s: %s", ref, os_errmsg());
        return 1;
      }
      t = (long long)st.mtime;
      ns = x.mtime_ns;
    }
    else if (date_s != NULL) {
      if (parse_date(date_s, &t, &ns) != 0) {
        tool_err(err, "date", "invalid date %s", q(date_s));
        return 1;
      }
    }
    else now_time(&t, &ns);
    date_print(&o, fmt, t, ns);
  }
  if (out_flush(&o) != 0) {
    tool_err(err, "date", "write error");
    status = 1;
  }
  return status;
}


int t_date (int argc, char **argv, int in, int out, int err) {
  Opts g;
  int status;
  opts_init(&g, "date", argc, argv, err);
  status = date_run(&g, in, out, err);
  g_tz_force = NULL;
  opts_free(&g);
  return status;
}

/* }================================================================== */


/*
** {==================================================================
** stat
** ===================================================================
*/

typedef struct StatF {
  const char *name;	/* as given */
  const char *native;
  OsStat st;
  OsStatX x;
} StatF;


static const char *file_type (const OsStat *st) {
  if (st->is_link) return "symbolic link";
  if (st->is_dir) return "directory";
  if (st->is_fifo) return "fifo";
  if (st->is_sock) return "socket";
  if (st->is_chr) return "character special file";
  if (st->is_blk) return "block special file";
  return st->size == 0 ? "regular empty file" : "regular file";
}


static unsigned raw_mode (const OsStat *st) {
  unsigned t = st->is_link ? 0xA000 : st->is_dir ? 0x4000 : st->is_fifo ? 0x1000 :
               st->is_sock ? 0xC000 : st->is_chr ? 0x2000 : st->is_blk ? 0x6000 : 0x8000;
  return t | (st->mode & 07777);
}


/* GNU's shell-escape-always quoting, for %N */
static void quote_shell (Buf *b, const char *s) {
  const char *p;
  if (strchr(s, '\'') == NULL) {
    buf_putc(b, '\'');
    buf_puts(b, s);
    buf_putc(b, '\'');
    return;
  }
  if (strpbrk(s, "\"$`\\") == NULL) {
    buf_putc(b, '"');
    buf_puts(b, s);
    buf_putc(b, '"');
    return;
  }
  buf_putc(b, '\'');
  for (p = s; *p; p++) {
    if (*p == '\'') buf_puts(b, "'\\''");
    else buf_putc(b, *p);
  }
  buf_putc(b, '\'');
}


static void stat_time (Buf *b, time_t t, long ns) {
  DT d;
  dt_make(&d, (long long)t, ns);
  fmt_time(b, "%Y-%m-%d %H:%M:%S.%N %z", &d);
}


/* where the file is mounted: the longest mount point it is under */
static char *mount_point (const char *native) {
  Vec m;
  size_t i, best = 0;
  char *real = os_realpath(native), *res = NULL, *shown;
  if (real == NULL) return xstrdup("?");
  vec_init(&m);
  os_mounts(&m);
  for (i = 0; i < m.n; i++) {
    char *mp = strchr(m.v[i], '\t'), *end;
    size_t n;
    if (mp == NULL) continue;
    mp++;
    end = strchr(mp, '\t');
    n = end ? (size_t)(end - mp) : strlen(mp);
    while (n > 1 && (mp[n - 1] == '/' || mp[n - 1] == '\\')) n--;
    if (n > 0 && n >= best && m_fnncmp(real, mp, n) == 0 &&
        (real[n] == '\0' || real[n] == '/' || real[n] == '\\' || mp[n - 1] == '/' || mp[n - 1] == '\\')) {
      free(res);
      res = xstrndup(mp, n);
      best = n;
    }
  }
  vec_free(&m);
  free(real);
  if (res == NULL) return xstrdup("?");
  shown = path_to_display(res);
  free(res);
  return shown;
}


/* one %X directive; spec is "%[flags][width][.prec]" */
static void stat_field (Buf *b, const char *spec, size_t speclen, char conv, StatF *f) {
  const OsStat *st = &f->st;
  const OsStatX *x = &f->x;
  char flags[8], fmt[48], tmp[160];
  int nflags = 0, width = -1, prec = -1;
  char num = 0;	/* 'd' signed, 'u' unsigned, 'o' octal, 'x' hex; 0: text */
  long long sv = 0;
  unsigned long long uv = 0;
  size_t k = 1;
  Buf s;
  for (; k < speclen && strchr("-#0+ '", spec[k]); k++)
    if (nflags < 7 && spec[k] != '\'') flags[nflags++] = spec[k];
  flags[nflags] = '\0';
  if (k < speclen && isdigit((unsigned char)spec[k]))
    for (width = 0; k < speclen && isdigit((unsigned char)spec[k]); k++) width = width * 10 + (spec[k] - '0');
  if (k < speclen && spec[k] == '.')
    for (prec = 0, k++; k < speclen && isdigit((unsigned char)spec[k]); k++) prec = prec * 10 + (spec[k] - '0');
  buf_init(&s);
  buf_putn(&s, "", 0);
  switch (conv) {
    case 'n': buf_puts(&s, f->name); break;
    case 'N': {
      char *target;
      quote_shell(&s, f->name);
      if (st->is_link && (target = os_readlink(f->native)) != NULL) {
        buf_puts(&s, " -> ");
        quote_shell(&s, target);
        free(target);
      }
      break;
    }
    case 'a': num = 'o'; uv = st->mode & 07777; break;
    case 'A': {
      char m[11];
      mode_string(m, st);
      buf_puts(&s, m);
      break;
    }
    case 'f': num = 'x'; uv = raw_mode(st); break;
    case 'F': buf_puts(&s, file_type(st)); break;
    case 's': num = 'd'; sv = st->size; break;
    case 'b': num = 'd'; sv = st->blocks; break;
    case 'B': num = 'u'; uv = x->block_unit; break;
    case 'o': num = 'u'; uv = x->blksize; break;
    case 'd': num = 'u'; uv = st->dev; break;
    case 'D': num = 'x'; uv = st->dev; break;
    case 'i': num = 'u'; uv = st->ino; break;
    case 'h': num = 'u'; uv = st->nlink; break;
    case 'u': num = 'd'; sv = st->uid; break;
    case 'g': num = 'd'; sv = st->gid; break;
    case 't': num = 'x'; uv = x->rdev_major; break;
    case 'T': num = 'x'; uv = x->rdev_minor; break;
    case 'U': case 'G': {
      char *n = conv == 'U' ? os_user_name(st->uid) : os_group_name(st->gid);
      buf_puts(&s, n);
      free(n);
      break;
    }
    case 'm': {
      char *m = mount_point(f->native);
      buf_puts(&s, m);
      free(m);
      break;
    }
    case 'x': stat_time(&s, st->atime, x->atime_ns); break;
    case 'y': stat_time(&s, st->mtime, x->mtime_ns); break;
    case 'z': stat_time(&s, st->ctime, x->ctime_ns); break;
    case 'w':
      if (x->has_btime) stat_time(&s, x->btime, x->btime_ns);
      else buf_putc(&s, '-');
      break;
    case 'X': case 'Y': case 'Z': case 'W': {
      long long secs = conv == 'X' ? (long long)st->atime : conv == 'Y' ? (long long)st->mtime :
                       conv == 'Z' ? (long long)st->ctime : x->has_btime ? (long long)x->btime : 0;
      long nsv = conv == 'X' ? x->atime_ns : conv == 'Y' ? x->mtime_ns : conv == 'Z' ? x->ctime_ns :
                 x->has_btime ? x->btime_ns : 0;
      if (prec < 0) {
        num = 'd';
        sv = secs;
        break;
      }
      {	/* %.3Y: the seconds with that many decimals */
        char ns9[16];
        int p = memchr(spec, '.', speclen) && prec == 0 && !isdigit((unsigned char)spec[speclen - 1]) ? 9 : prec;
        if (p > 9) p = 9;
        sprintf(ns9, "%09ld", nsv);
        sprintf(tmp, "%lld", secs);
        if (p > 0) {
          strcat(tmp, ".");
          strncat(tmp, ns9, (size_t)p);
        }
        buf_puts(&s, tmp);
        prec = -1;
      }
      break;
    }
    default:	/* not one we know: as GNU stat prints it */
      buf_putc(b, '?');
      buf_free(&s);
      return;
  }
  if (num) {	/* printf with the flags that mean something for it */
    size_t n = 0, j;
    fmt[n++] = '%';
    for (j = 0; flags[j]; j++) {
      char fl = flags[j];
      if (fl == '-' || fl == '0' || (fl == '#' && (num == 'o' || num == 'x')) ||
          ((fl == '+' || fl == ' ') && num == 'd'))
        fmt[n++] = fl;
    }
    fmt[n] = '\0';
    if (width >= 0) n += (size_t)sprintf(fmt + n, "%d", width);
    if (prec >= 0) n += (size_t)sprintf(fmt + n, ".%d", prec);
    fmt[n++] = 'l';
    fmt[n++] = 'l';
    fmt[n++] = num;
    fmt[n] = '\0';
    if (num == 'd') snprintf(tmp, sizeof(tmp), fmt, sv);
    else snprintf(tmp, sizeof(tmp), fmt, uv);
    buf_puts(b, tmp);
  }
  else {	/* text: '-', width and precision */
    int len = (int)s.len, left = strchr(flags, '-') != NULL;
    if (prec >= 0 && prec < len) len = prec;
    if (!left) put_pad(b, width - len, ' ');
    buf_putn(b, s.s, (size_t)len);
    if (left) put_pad(b, width - len, ' ');
  }
  buf_free(&s);
}


/* backslash escapes of --printf */
static const char *stat_escape (Buf *b, const char *p) {
  int v, k;
  switch (*p) {
    case 'a': buf_putc(b, '\a'); return p + 1;
    case 'b': buf_putc(b, '\b'); return p + 1;
    case 'e': buf_putc(b, '\033'); return p + 1;
    case 'f': buf_putc(b, '\f'); return p + 1;
    case 'n': buf_putc(b, '\n'); return p + 1;
    case 'r': buf_putc(b, '\r'); return p + 1;
    case 't': buf_putc(b, '\t'); return p + 1;
    case 'v': buf_putc(b, '\v'); return p + 1;
    case '\\': buf_putc(b, '\\'); return p + 1;
    case '"': buf_putc(b, '"'); return p + 1;
    case 'x':
      if (!isxdigit((unsigned char)p[1])) {
        buf_putc(b, '\\');
        return p;
      }
      for (v = 0, k = 1; k <= 2 && isxdigit((unsigned char)p[k]); k++)
        v = v * 16 + (isdigit((unsigned char)p[k]) ? p[k] - '0' : (tolower((unsigned char)p[k]) - 'a' + 10));
      buf_putc(b, (char)v);
      return p + k;
    case '\0': buf_putc(b, '\\'); return p;
  }
  if (*p >= '0' && *p <= '7') {
    for (v = 0, k = 0; k < 3 && p[k] >= '0' && p[k] <= '7'; k++) v = v * 8 + (p[k] - '0');
    buf_putc(b, (char)v);
    return p + k;
  }
  buf_putc(b, '\\');
  buf_putc(b, *p);
  return p + 1;
}


static void stat_format (Buf *b, const char *fmt, StatF *f, int escapes) {
  const char *p;
  for (p = fmt; *p; p++) {
    const char *spec;
    if (*p == '\\' && escapes) {
      p = stat_escape(b, p + 1) - 1;
      continue;
    }
    if (*p != '%') {
      buf_putc(b, *p);
      continue;
    }
    spec = p++;
    if (*p == '%') {
      buf_putc(b, '%');
      continue;
    }
    while (*p && strchr("-#0+ '", *p)) p++;
    while (isdigit((unsigned char)*p)) p++;
    if (*p == '.') {
      p++;
      while (isdigit((unsigned char)*p)) p++;
    }
    if (*p == '\0') {	/* a lone % at the end */
      buf_puts(b, spec);
      break;
    }
    stat_field(b, spec, (size_t)(p - spec), *p, f);
  }
}


int t_stat (int argc, char **argv, int in, int out, int err) {
  static const char *const head = "  File: %N\n";
  static const char *const body =
    "  Size: %-10s\tBlocks: %-10b IO Block: %-6o %F\n"
    "Device: %Dh/%dd\tInode: %-10i  Links: %h\n";
  static const char *const body_dev =
    "  Size: %-10s\tBlocks: %-10b IO Block: %-6o %F\n"
    "Device: %Dh/%dd\tInode: %-10i  Links: %-5h Device type: %t,%T\n";
  static const char *const tail =
    "Access: (%04a/%10.10A)  Uid: (%5u/%8U)   Gid: (%5g/%8G)\n"
    "Access: %x\nModify: %y\nChange: %z\n Birth: %w";
  LongOpt lo[] = {{"dereference", 'L', 0}, {"format", 'c', 1}, {"printf", 'P', 1},
                  {"terse", 't', 0}, {"file-system", 'f', 0}, {"cached", 'C', 1},
                  {"version", OPT_VERSION, 0}, {NULL, 0, 0}};
  Opts g;
  int c, follow = 0, terse = 0, status = 0, printf_mode = 0;
  const char *fmt = NULL;
  size_t i;
  Out o;
  (void)in;
  opts_init(&g, "stat", argc, argv, err);
  while ((c = opts_next(&g, "Lc:tf", lo)) != 0) {
    switch (c) {
      case 'L': follow = 1; break;
      case 'c': fmt = g.arg; printf_mode = 0; break;
      case 'P': fmt = g.arg; printf_mode = 1; break;
      case 't': terse = 1; break;
      case 'C': break;
      case 'f':
        tool_err(err, "stat", "--file-system is not supported (df shows file systems)");
        opts_free(&g);
        return 1;
      case OPT_HELP: opts_free(&g); return tool_help(out, "stat");
      case OPT_VERSION: opts_free(&g); return tool_version(out, "stat");
      default: opts_free(&g); return 1;
    }
  }
  if (g.ops.n == 0) {
    tool_err(err, "stat", "missing operand");
    opts_free(&g);
    return try_help(err, "stat", 1);
  }
  out_init(&o, out);
  for (i = 0; i < g.ops.n && !tool_stop(); i++) {
    StatF f;
    char *native = path_to_native(g.ops.v[i]);
    Buf b;
    f.name = g.ops.v[i];
    f.native = native;
    if (os_stat_x(native, follow, &f.st, &f.x) != 0) {
      out_flush(&o);
      tool_err(err, "stat", "cannot stat %s: %s", q(f.name), os_errmsg());
      status = 1;
      free(native);
      continue;
    }
    buf_init(&b);
    if (fmt != NULL) stat_format(&b, fmt, &f, printf_mode);
    else if (terse) stat_format(&b, "%n %s %b %f %u %g %D %i %h %t %T %X %Y %Z %W %o", &f, 0);
    else {
      if (f.st.is_link) stat_format(&b, head, &f, 0);
      else {	/* the name as it is, unless a link */
        buf_puts(&b, "  File: ");
        buf_puts(&b, f.name);
        buf_putc(&b, '\n');
      }
      stat_format(&b, f.st.is_chr || f.st.is_blk ? body_dev : body, &f, 0);
      stat_format(&b, tail, &f, 0);
    }
    if (b.s) out_putn(&o, b.s, b.len);
    if (!printf_mode) out_putc(&o, '\n');
    buf_free(&b);
    free(native);
  }
  if (out_flush(&o) != 0) status = 1;
  opts_free(&g);
  return status;
}

/* }================================================================== */


/*
** {==================================================================
** nproc printenv tty yes sync
** ===================================================================
*/

int t_nproc (int argc, char **argv, int in, int out, int err) {
  LongOpt lo[] = {{"all", 'a', 0}, {"ignore", 'i', 1}, {"version", OPT_VERSION, 0}, {NULL, 0, 0}};
  Opts g;
  int c, all = 0;
  long long ignore = 0, n, k;
  const char *v;
  (void)in;
  opts_init(&g, "nproc", argc, argv, err);
  while ((c = opts_next(&g, "", lo)) != 0) {
    switch (c) {
      case 'a': all = 1; break;
      case 'i':
        if (str_to_ll(g.arg, &ignore) != 0 || ignore < 0) {
          tool_err(err, "nproc", "invalid number: %s", q(g.arg));
          opts_free(&g);
          return 1;
        }
        break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "nproc");
      case OPT_VERSION: opts_free(&g); return tool_version(out, "nproc");
      default: opts_free(&g); return 1;
    }
  }
  if (g.ops.n > 0) {
    tool_err(err, "nproc", "extra operand %s", q(g.ops.v[0]));
    opts_free(&g);
    return try_help(err, "nproc", 1);
  }
  opts_free(&g);
  n = os_nproc(all);
  if (!all) {	/* OpenMP's limits count, as for GNU nproc */
    v = var_get("OMP_NUM_THREADS");
    if (v && *v) {
      char *first = xstrndup(v, strcspn(v, ","));
      if (str_to_ll(first, &k) == 0 && k > 0) n = k;
      free(first);
    }
    v = var_get("OMP_THREAD_LIMIT");
    if (v && *v && str_to_ll(v, &k) == 0 && k > 0 && k < n) n = k;
  }
  n -= ignore;
  if (n < 1) n = 1;
  fd_printf(out, "%lld\n", n);
  return 0;
}


int t_printenv (int argc, char **argv, int in, int out, int err) {
  LongOpt lo[] = {{"null", '0', 0}, {"version", OPT_VERSION, 0}, {NULL, 0, 0}};
  Opts g;
  int c, zero = 0, status = 0;
  size_t i, k;
  Vec env;
  Out o;
  (void)in;
  opts_init(&g, "printenv", argc, argv, err);
  while ((c = opts_next(&g, "0", lo)) != 0) {
    switch (c) {
      case '0': zero = 1; break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "printenv");
      case OPT_VERSION: opts_free(&g); return tool_version(out, "printenv");
      default: opts_free(&g); return 2;
    }
  }
  vec_init(&env);
  var_env(&env);
  out_init(&o, out);
  if (g.ops.n == 0)
    for (k = 0; k < env.n; k++) {
      out_puts(&o, env.v[k]);
      out_putc(&o, zero ? '\0' : '\n');
    }
  for (i = 0; i < g.ops.n; i++) {
    const char *name = g.ops.v[i];
    size_t n = strlen(name);
    int found = 0;
    if (strchr(name, '=') == NULL)
      for (k = 0; k < env.n && !found; k++)
        if (strncmp(env.v[k], name, n) == 0 && env.v[k][n] == '=') {
          out_puts(&o, env.v[k] + n + 1);
          out_putc(&o, zero ? '\0' : '\n');
          found = 1;
        }
    if (!found) status = 1;
  }
  out_flush(&o);
  vec_free(&env);
  opts_free(&g);
  return status;
}


int t_tty (int argc, char **argv, int in, int out, int err) {
  LongOpt lo[] = {{"silent", 's', 0}, {"quiet", 's', 0}, {"version", OPT_VERSION, 0}, {NULL, 0, 0}};
  Opts g;
  int c, silent = 0;
  char *name;
  opts_init(&g, "tty", argc, argv, err);
  while ((c = opts_next(&g, "s", lo)) != 0) {
    switch (c) {
      case 's': silent = 1; break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "tty");
      case OPT_VERSION: opts_free(&g); return tool_version(out, "tty");
      default: opts_free(&g); return 2;
    }
  }
  if (g.ops.n > 0) {
    tool_err(err, "tty", "extra operand %s", q(g.ops.v[0]));
    opts_free(&g);
    return try_help(err, "tty", 2);
  }
  opts_free(&g);
  name = os_ttyname(in);
  if (!silent) fd_printf(out, "%s\n", name ? name : "not a tty");
  c = name ? 0 : 1;
  free(name);
  return c;
}


int t_yes (int argc, char **argv, int in, int out, int err) {
  Buf line, block;
  int i, first;
  long n = 0;
  (void)in;
  if ((first = std_opts(argc, argv, "yes", out, err, 1, &i)) >= 0) return first;
  buf_init(&line);
  if (i >= argc) buf_putc(&line, 'y');
  for (first = i; i < argc; i++) {
    if (i > first) buf_putc(&line, ' ');
    buf_puts(&line, argv[i]);
  }
  buf_putc(&line, '\n');
  buf_init(&block);	/* whole lines, 8 KiB or so at a time */
  do buf_putn(&block, line.s, line.len);
  while (block.len < 8192);
  buf_free(&line);
  for (;;) {
    if (os_write(out, block.s, block.len) < 0) {
      tool_err(err, "yes", "standard output: %s", os_errmsg());
      buf_free(&block);
      return 1;
    }
    if (++n % 16 == 0 && tool_stop()) break;
  }
  buf_free(&block);
  return 130;
}


int t_sync (int argc, char **argv, int in, int out, int err) {
  LongOpt lo[] = {{"data", 'd', 0}, {"file-system", 'f', 0}, {"version", OPT_VERSION, 0},
                  {NULL, 0, 0}};
  Opts g;
  int c, data = 0, fs = 0, status = 0;
  size_t i;
  (void)in;
  opts_init(&g, "sync", argc, argv, err);
  while ((c = opts_next(&g, "df", lo)) != 0) {
    switch (c) {
      case 'd': data = 1; break;
      case 'f': fs = 1; break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "sync");
      case OPT_VERSION: opts_free(&g); return tool_version(out, "sync");
      default: opts_free(&g); return 1;
    }
  }
  if (data && fs) {
    tool_err(err, "sync", "cannot specify both --data and --file-system");
    opts_free(&g);
    return 1;
  }
  if (data && g.ops.n == 0) {
    tool_err(err, "sync", "--data needs at least one argument");
    opts_free(&g);
    return 1;
  }
  if (g.ops.n == 0) os_sync();
  for (i = 0; i < g.ops.n; i++) {
    char *native = path_to_native(g.ops.v[i]);
    int fd = os_open(native, OS_READ);
    free(native);
    if (fd < 0) {
      tool_err(err, "sync", "error opening %s: %s", q(g.ops.v[i]), os_errmsg());
      status = 1;
      continue;
    }
    if (fs) os_sync();
    else if (os_fsync(fd) != 0) {
      tool_err(err, "sync", "error syncing %s: %s", q(g.ops.v[i]), os_errmsg());
      status = 1;
    }
    os_close(fd);
  }
  opts_free(&g);
  return status;
}

/* }================================================================== */


/*
** {==================================================================
** mktemp truncate
** ===================================================================
*/

static unsigned long long rand_next (void) {
  static unsigned long long s = 0;
  if (s == 0)
    s = ((unsigned long long)os_now_us() * 6364136223846793005ULL) ^
        ((unsigned long long)os_getpid() << 32) ^ 0x9E3779B97F4A7C15ULL;
  s ^= s << 13;
  s ^= s >> 7;
  s ^= s << 17;
  return s;
}


/* the folder of -t and --tmpdir: -p DIR, $TMPDIR, /tmp (-t: $TMPDIR first) */
static char *mktemp_dir (const char *pdir, int tflag) {
  const char *env = var_get("TMPDIR");
  char *t;
  OsStat st;
  int ok;
  if (env && *env && strpbrk(env, ":\\") != NULL) {	/* C:\Temp from Windows: as /c/Temp */
    char *shown, *native = path_env_to_native(env);
    shown = path_to_display(native);
    free(native);
    if (tflag || !(pdir && *pdir)) return shown;
    free(shown);
  }
  if (tflag && env && *env) return xstrdup(env);
  if (pdir && *pdir) return xstrdup(pdir);
  if (env && *env) return xstrdup(env);
  t = path_to_native("/tmp");
  ok = os_stat(t, &st) == 0 && st.is_dir;
  free(t);
  if (ok) return xstrdup("/tmp");
  t = path_tmpdir();
  {
    char *shown = path_to_display(t);
    free(t);
    return shown;
  }
}


static int mktemp_fail (int err, const char *fmt, const char *a) {	/* -q does not hide these */
  tool_err(err, "mktemp", fmt, q(a));
  return 1;
}


static int mktemp_make (Opts *g, int out, int err) {
  LongOpt lo[] = {{"directory", 'd', 0}, {"dry-run", 'u', 0}, {"quiet", 'q', 0},
                  {"tmpdir", 'p', 2}, {"suffix", 'S', 1}, {"version", OPT_VERSION, 0},
                  {NULL, 0, 0}};
  static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  int c, dir = 0, dry = 0, quiet = 0, use_tmpdir = 0, tflag = 0, tries, r = 1;
  const char *pdir = NULL, *suffix = NULL, *tmpl;
  char *path, *name = NULL;
  size_t nx, sl, xend, tl;
  while ((c = opts_next(g, "dup:qt", lo)) != 0) {
    switch (c) {
      case 'd': dir = 1; break;
      case 'u': dry = 1; break;
      case 'q': quiet = 1; break;
      case 'p': use_tmpdir = 1; pdir = g->arg; break;
      case 't': tflag = 1; use_tmpdir = 1; break;
      case 'S': suffix = g->arg; break;
      case OPT_HELP: return tool_help(out, "mktemp");
      case OPT_VERSION: return tool_version(out, "mktemp");
      default: return 1;
    }
  }
  if (g->ops.n > 1) {
    tool_err(err, "mktemp", "too many templates");
    return try_help(err, "mktemp", 1);
  }
  if (g->ops.n == 0) {
    tmpl = "tmp.XXXXXXXXXX";
    use_tmpdir = 1;
  }
  else tmpl = g->ops.v[0];
  tl = strlen(tmpl);
  if (suffix != NULL) {
    if (tl == 0 || tmpl[tl - 1] != 'X')
      return mktemp_fail(err, "with --suffix, template %s must end in X", tmpl);
    xend = tl;
  }
  else {	/* the last run of X's; what follows it is the suffix */
    const char *lastx = strrchr(tmpl, 'X');
    xend = lastx ? (size_t)(lastx - tmpl) + 1 : 0;
    suffix = tmpl + xend;
  }
  sl = strlen(suffix);
  for (nx = 0; nx < xend && tmpl[xend - 1 - nx] == 'X'; nx++) {}
  if (strchr(suffix, '/') != NULL)
    return mktemp_fail(err, "invalid suffix %s, contains directory separator", suffix);
  if (nx < 3) return mktemp_fail(err, "too few X's in template %s", tmpl);
  if (use_tmpdir) {
    char *base;
    if (tflag && strchr(tmpl, '/') != NULL)
      return mktemp_fail(err, "invalid template, %s, contains directory separator", tmpl);
    if (tmpl[0] == '/') {
      tool_err(err, "mktemp", "invalid template, %s; with --tmpdir, it may not be absolute", q(tmpl));
      return 1;
    }
    base = mktemp_dir(pdir, tflag);
    path = tool_join(base, tmpl);
    free(base);
  }
  else path = xstrdup(tmpl);
  if (suffix != tmpl + xend) {	/* --suffix: after the template */
    char *t = xstrcat3(path, suffix, "");
    free(path);
    path = t;
  }
  for (tries = 0; tries < 1000; tries++) {
    size_t k, xstart = strlen(path) - sl - nx;
    char *native;
    OsStat st;
    int ok;
    free(name);
    name = xstrdup(path);
    for (k = 0; k < nx; k++) name[xstart + k] = chars[rand_next() % 62];
    native = path_to_native(name);
    if (dry) ok = os_lstat(native, &st) != 0;
    else if (dir) {
      ok = os_mkdir(native) == 0;
      if (ok) os_chmod(native, 0700);
    }
    else {
      int fd = os_open(native, OS_EXCL);
      ok = fd >= 0;
      if (ok) {
        os_close(fd);
        os_chmod(native, 0600);
      }
    }
    free(native);
    if (ok) {
      fd_printf(out, "%s\n", name);
      r = 0;
      break;
    }
    if (!dry && os_errcode() != OS_E_EXIST) {
      if (!quiet)
        tool_err(err, "mktemp", "failed to create %s via template %s: %s",
                 dir ? "directory" : "file", q(path), os_errmsg());
      break;
    }
  }
  if (tries >= 1000 && !quiet)
    tool_err(err, "mktemp", "failed to create %s via template %s: File exists",
             dir ? "directory" : "file", q(path));
  free(name);
  free(path);
  return r;
}


int t_mktemp (int argc, char **argv, int in, int out, int err) {
  Opts g;
  int r;
  (void)in;
  opts_init(&g, "mktemp", argc, argv, err);
  r = mktemp_make(&g, out, err);
  opts_free(&g);
  return r;
}


static int truncate_usage (int err, const char *msg) {
  tool_err(err, "truncate", "%s", msg);
  return try_help(err, "truncate", 1);
}


int t_truncate (int argc, char **argv, int in, int out, int err) {
  LongOpt lo[] = {{"no-create", 'c', 0}, {"io-blocks", 'o', 0}, {"reference", 'r', 1},
                  {"size", 's', 1}, {"version", OPT_VERSION, 0}, {NULL, 0, 0}};
  Opts g;
  int c, nocreate = 0, blocks = 0, status = 0, have_size = 0;
  char op = 0;
  const char *ref = NULL;
  long long size = 0, refsize = -1;
  size_t i;
  (void)in;
  opts_init(&g, "truncate", argc, argv, err);
  while ((c = opts_next(&g, "cor:s:", lo)) != 0) {
    switch (c) {
      case 'c': nocreate = 1; break;
      case 'o': blocks = 1; break;
      case 'r': ref = g.arg; break;
      case 's': {
        const char *s = g.arg;
        while (isspace((unsigned char)*s)) s++;
        op = 0;
        if (*s && strchr("+-<>/%", *s)) op = *s++;
        while (isspace((unsigned char)*s)) s++;
        if (parse_size(s, &size) != 0) {
          tool_err(err, "truncate", "Invalid number: %s", q(g.arg));
          opts_free(&g);
          return 1;
        }
        if ((op == '/' || op == '%') && size == 0) {
          tool_err(err, "truncate", "division by zero");
          opts_free(&g);
          return 1;
        }
        have_size = 1;
        break;
      }
      case OPT_HELP: opts_free(&g); return tool_help(out, "truncate");
      case OPT_VERSION: opts_free(&g); return tool_version(out, "truncate");
      default: opts_free(&g); return 1;
    }
  }
  if (!have_size && ref == NULL) c = truncate_usage(err, "you must specify either '--size' or '--reference'");
  else if (have_size && ref != NULL && op == 0)
    c = truncate_usage(err, "you must specify a relative '--size' with '--reference'");
  else if (blocks && !have_size) c = truncate_usage(err, "'--io-blocks' was specified but '--size' was not");
  else if (g.ops.n == 0) c = truncate_usage(err, "missing file operand");
  else c = 0;
  if (c != 0) {
    opts_free(&g);
    return 1;
  }
  if (ref != NULL) {
    OsStat st;
    char *native = path_to_native(ref);
    int bad = os_stat(native, &st) != 0;
    free(native);
    if (bad) {
      tool_err(err, "truncate", "cannot stat %s: %s", q(ref), os_errmsg());
      opts_free(&g);
      return 1;
    }
    refsize = st.size;
  }
  for (i = 0; i < g.ops.n; i++) {
    const char *name = g.ops.v[i];
    char *native = path_to_native(name);
    OsStat st;
    OsStatX x;
    int fd, exists = os_stat_x(native, 1, &st, &x) == 0;
    long long cur = exists ? st.size : 0, want, unit = size, base;
    if (!exists && nocreate) {
      free(native);
      continue;
    }
    if (exists && st.is_dir) {
      tool_err(err, "truncate", "cannot open %s for writing: Is a directory", q(name));
      status = 1;
      free(native);
      continue;
    }
    fd = os_open(native, OS_RDWR);
    free(native);
    if (fd < 0) {
      tool_err(err, "truncate", "cannot open %s for writing: %s", q(name), os_errmsg());
      status = 1;
      continue;
    }
    if (blocks) unit = size * (long long)(x.blksize ? x.blksize : 512);
    base = refsize >= 0 ? refsize : cur;
    switch (op) {
      case '+': want = base + unit; break;
      case '-': want = base - unit; break;
      case '<': want = base > unit ? unit : base; break;
      case '>': want = base < unit ? unit : base; break;
      case '/': want = base / unit * unit; break;
      case '%': want = (base + unit - 1) / unit * unit; break;
      default: want = refsize >= 0 && !have_size ? refsize : unit; break;
    }
    if (want < 0) want = 0;
    if (want != cur && os_ftruncate(fd, want) != 0) {
      tool_err(err, "truncate", "failed to truncate %s at %lld bytes: %s", q(name), want, os_errmsg());
      status = 1;
    }
    os_close(fd);
  }
  opts_free(&g);
  return status;
}

/* }================================================================== */


/*
** {==================================================================
** expr
** ===================================================================
*/

typedef struct EVal {
  char *s;	/* the value as text; numbers too */
} EVal;

typedef struct Expr {
  char **args;
  int n, pos, err;
  char msg[512];
} Expr;


static EVal *ev_str (const char *s) {
  EVal *v = (EVal *)xmalloc(sizeof(EVal));
  v->s = xstrdup(s);
  return v;
}


static EVal *ev_int (long long i) {
  char num[24];
  return ev_str(ll_to_str(i, num));
}


static void ev_free (EVal *v) {
  if (v == NULL) return;
  free(v->s);
  free(v);
}


/* "", "0", "-0", "00": what expr calls null */
static int ev_null (const EVal *v) {
  const char *p = v->s;
  if (*p == '\0') return 1;
  if (*p == '-') p++;
  if (*p == '\0') return 0;
  for (; *p; p++)
    if (*p != '0') return 0;
  return 1;
}


static int looks_int (const char *s) {
  if (*s == '-') s++;
  if (!*s) return 0;
  for (; *s; s++)
    if (!isdigit((unsigned char)*s)) return 0;
  return 1;
}


static void ex_error (Expr *e, const char *fmt, const char *arg) {
  if (e->err) return;
  e->err = 1;
  snprintf(e->msg, sizeof(e->msg), fmt, arg ? q(arg) : "");
}


static int ev_toint (Expr *e, const EVal *v, long long *out) {
  if (looks_int(v->s) && str_to_ll(v->s, out) == 0) return 0;
  ex_error(e, looks_int(v->s) ? "integer is too large" : "non-integer argument", NULL);
  return -1;
}


static int ex_next (Expr *e, const char *s) {
  if (e->pos < e->n && strcmp(e->args[e->pos], s) == 0) {
    e->pos++;
    return 1;
  }
  return 0;
}


static EVal *ex_or (Expr *e, int eval);


static EVal *ex_primary (Expr *e, int eval) {
  EVal *v;
  if (e->err) return ev_str("");
  if (e->pos >= e->n) {
    ex_error(e, "syntax error: missing argument after %s", e->args[e->pos - 1]);
    return ev_str("");
  }
  if (ex_next(e, "(")) {
    v = ex_or(e, eval);
    if (e->err) return v;
    if (e->pos >= e->n) ex_error(e, "syntax error: expecting ')' after %s", e->args[e->pos - 1]);
    else if (!ex_next(e, ")")) ex_error(e, "syntax error: expecting ')' instead of %s", e->args[e->pos]);
    return v;
  }
  if (strcmp(e->args[e->pos], ")") == 0) {
    ex_error(e, "syntax error: unexpected ')'", NULL);
    return ev_str("");
  }
  return ev_str(e->args[e->pos++]);
}


static size_t ex_chars (const char *s, size_t n) {
  return tool_utf8() ? utf8_count(s, n) : n;
}


static int ex_charlen (const char *s, int u8) {
  int n = u8 ? utf8_len(s) : 1;
  return n < 1 ? 1 : n;
}


static EVal *ex_match (Expr *e, EVal *l, EVal *r) {	/* STRING : REGEX, anchored */
  char *msg = NULL;
  Regex *re = regex_new(r->s, 0, &msg);
  EVal *v;
  size_t m[20];
  int nsub;
  if (re == NULL) {
    ex_error(e, "%s", NULL);
    snprintf(e->msg, sizeof(e->msg), "%s", msg ? msg : "Invalid regular expression");
    e->msg[0] = (char)toupper((unsigned char)e->msg[0]);	/* as glibc's regcomp says it */
    free(msg);
    return ev_str("");
  }
  nsub = regex_nsub(re);
  if (nsub > 9) nsub = 9;
  if (regex_match(re, l->s, strlen(l->s), 0, 0, m) && m[0] == 0) {
    if (nsub == 0) v = ev_int((long long)ex_chars(l->s, m[1]));
    else if (m[2] == (size_t)-1) v = ev_str("");
    else {
      char *g = xstrndup(l->s + m[2], m[3] - m[2]);
      v = ev_str(g);
      free(g);
    }
  }
  else v = nsub > 0 ? ev_str("") : ev_int(0);
  regex_free(re);
  return v;
}


static EVal *ex_unary (Expr *e, int eval) {
  int u8 = tool_utf8();
  if (e->err) return ev_str("");
  if (ex_next(e, "+")) {
    if (e->pos >= e->n) {
      ex_error(e, "syntax error: missing argument after %s", "+");
      return ev_str("");
    }
    return ev_str(e->args[e->pos++]);
  }
  if (ex_next(e, "length")) {
    EVal *a = ex_unary(e, eval), *v = ev_int((long long)ex_chars(a->s, strlen(a->s)));
    ev_free(a);
    return v;
  }
  if (ex_next(e, "match")) {
    EVal *a = ex_unary(e, eval), *b = ex_unary(e, eval), *v;
    v = eval && !e->err ? ex_match(e, a, b) : ev_str("");
    ev_free(a);
    ev_free(b);
    return v;
  }
  if (ex_next(e, "index")) {	/* where the first of CHARS is in STRING */
    EVal *a = ex_unary(e, eval), *b = ex_unary(e, eval);
    long long pos = 0, k = 0;
    const char *p;
    for (p = a->s; *p && pos == 0; p += ex_charlen(p, u8)) {
      const char *c;
      int len = ex_charlen(p, u8);
      k++;
      for (c = b->s; *c; c += ex_charlen(c, u8))
        if (ex_charlen(c, u8) == len && memcmp(c, p, (size_t)len) == 0) {
          pos = k;
          break;
        }
    }
    ev_free(a);
    ev_free(b);
    return ev_int(pos);
  }
  if (ex_next(e, "substr")) {
    EVal *a = ex_unary(e, eval), *b = ex_unary(e, eval), *c = ex_unary(e, eval), *v;
    long long pos, len;
    if (!looks_int(b->s) || !looks_int(c->s) || str_to_ll(b->s, &pos) != 0 ||
        str_to_ll(c->s, &len) != 0 || pos < 1 || len < 1)
      v = ev_str("");
    else {	/* characters pos .. pos+len-1 */
      const char *p = a->s, *start = NULL;
      long long k = 1;
      for (; *p; p += ex_charlen(p, u8), k++) {
        if (k == pos) start = p;
        if (k == pos + len) break;
      }
      if (start == NULL) v = ev_str("");
      else {
        char *s = xstrndup(start, (size_t)(p - start));
        v = ev_str(s);
        free(s);
      }
    }
    ev_free(a);
    ev_free(b);
    ev_free(c);
    return v;
  }
  return ex_primary(e, eval);
}


static EVal *ex_colon (Expr *e, int eval) {
  EVal *l = ex_unary(e, eval);
  while (!e->err && ex_next(e, ":")) {
    EVal *r = ex_unary(e, eval);
    if (eval && !e->err) {
      EVal *v = ex_match(e, l, r);
      ev_free(l);
      l = v;
    }
    ev_free(r);
  }
  return l;
}


static int ex_calc (Expr *e, char op, long long a, long long b, long long *res) {
  const long long max = 0x7FFFFFFFFFFFFFFFLL, min = -max - 1;
  int over = 0;
  if ((op == '/' || op == '%') && b == 0) {
    ex_error(e, "division by zero", NULL);
    return -1;
  }
  switch (op) {
    case '+': over = (b > 0 && a > max - b) || (b < 0 && a < min - b); if (!over) *res = a + b; break;
    case '-': over = (b < 0 && a > max + b) || (b > 0 && a < min + b); if (!over) *res = a - b; break;
    case '*':
      if (a != 0 && b != 0) {
        unsigned long long ua = a < 0 ? (unsigned long long)(-(a + 1)) + 1 : (unsigned long long)a;
        unsigned long long ub = b < 0 ? (unsigned long long)(-(b + 1)) + 1 : (unsigned long long)b;
        over = ua > (unsigned long long)max / ub;
      }
      if (!over) *res = a * b;
      break;
    default:
      over = a == min && b == -1;
      if (!over) *res = op == '/' ? a / b : a % b;
      break;
  }
  if (over) {
    ex_error(e, "integer is too large", NULL);
    return -1;
  }
  return 0;
}


static EVal *ex_arith (Expr *e, int eval, int mul) {
  EVal *l = mul ? ex_colon(e, eval) : ex_arith(e, eval, 1);
  for (;;) {
    char op;
    EVal *r;
    long long a, b, res;
    if (e->err) return l;
    if (mul && ex_next(e, "*")) op = '*';
    else if (mul && ex_next(e, "/")) op = '/';
    else if (mul && ex_next(e, "%")) op = '%';
    else if (!mul && ex_next(e, "+")) op = '+';
    else if (!mul && ex_next(e, "-")) op = '-';
    else return l;
    r = mul ? ex_colon(e, eval) : ex_arith(e, eval, 1);
    if (eval && !e->err && ev_toint(e, l, &a) == 0 && ev_toint(e, r, &b) == 0 &&
        ex_calc(e, op, a, b, &res) == 0) {
      ev_free(l);
      l = ev_int(res);
    }
    ev_free(r);
  }
}


static EVal *ex_compare (Expr *e, int eval) {
  static const char *const ops[] = {"<", "<=", "=", "==", "!=", ">=", ">", NULL};
  EVal *l = ex_arith(e, eval, 0);
  for (;;) {
    int k, cmp, res;
    long long a, b;
    EVal *r;
    if (e->err) return l;
    for (k = 0; ops[k]; k++)
      if (ex_next(e, ops[k])) break;
    if (ops[k] == NULL) return l;
    r = ex_arith(e, eval, 0);
    if (eval && !e->err) {
      if (looks_int(l->s) && looks_int(r->s) && str_to_ll(l->s, &a) == 0 && str_to_ll(r->s, &b) == 0)
        cmp = a < b ? -1 : a > b;
      else {
        cmp = strcmp(l->s, r->s);
        cmp = cmp < 0 ? -1 : cmp > 0;
      }
      res = k == 0 ? cmp < 0 : k == 1 ? cmp <= 0 : k <= 3 ? cmp == 0 : k == 4 ? cmp != 0 :
            k == 5 ? cmp >= 0 : cmp > 0;
      ev_free(l);
      l = ev_int(res);
    }
    ev_free(r);
  }
}


static EVal *ex_and (Expr *e, int eval) {
  EVal *l = ex_compare(e, eval);
  while (!e->err && ex_next(e, "&")) {
    EVal *r = ex_compare(e, eval && !ev_null(l));
    if (ev_null(l) || ev_null(r)) {
      ev_free(l);
      l = ev_int(0);
    }
    ev_free(r);
  }
  return l;
}


static EVal *ex_or (Expr *e, int eval) {
  EVal *l = ex_and(e, eval);
  while (!e->err && ex_next(e, "|")) {
    EVal *r = ex_and(e, eval && ev_null(l));
    if (ev_null(l)) {
      ev_free(l);
      l = r;
      if (ev_null(l)) {
        ev_free(l);
        l = ev_int(0);
      }
    }
    else ev_free(r);
  }
  return l;
}


int t_expr (int argc, char **argv, int in, int out, int err) {
  Expr e;
  EVal *v;
  int status;
  (void)in;
  if (argc == 2 && strcmp(argv[1], "--help") == 0) return tool_help(out, "expr");
  if (argc == 2 && strcmp(argv[1], "--version") == 0) return tool_version(out, "expr");
  memset(&e, 0, sizeof(e));
  e.args = argv + 1;
  e.n = argc - 1;
  if (e.n > 0 && strcmp(e.args[0], "--") == 0) {
    e.args++;
    e.n--;
  }
  if (e.n == 0) {
    tool_err(err, "expr", "missing operand");
    return try_help(err, "expr", 2);
  }
  v = ex_or(&e, 1);
  if (!e.err && e.pos < e.n) ex_error(&e, "syntax error: unexpected argument %s", e.args[e.pos]);
  if (e.err) {
    tool_err(err, "expr", "%s", e.msg);
    ev_free(v);
    return 2;
  }
  fd_printf(out, "%s\n", v->s);
  status = ev_null(v);
  ev_free(v);
  return status;
}

/* }================================================================== */


/*
** {==================================================================
** Running commands: nohup, timeout, xargs
** ===================================================================
*/

/* 0 no such command, 1 a builtin (ours), 2 a program (*exe) */
static int cmd_find (const char *name, char **exe) {
  *exe = NULL;
  if (*name == '\0') return 0;
  if (builtin_find(name, 0) != NULL && builtin_enabled(name)) return 1;
  if ((*exe = sh_find_command(name)) != NULL) return 2;
  if (builtin_fallback(name) != NULL) return 1;
  return 0;
}


/* in this mmc, with these descriptors (-1: the same) */
static int run_here (int argc, char **argv, int fd0, int fd1, int fd2) {
  int save[3], fds[3], k, st;
  fds[0] = fd0;
  fds[1] = fd1;
  fds[2] = fd2;
  for (k = 0; k < 3; k++) {
    save[k] = sh_fd[k];
    if (fds[k] >= 0) sh_fd[k] = fds[k];
  }
  st = sh_eval_argv(argc, argv, sh_fd[0], sh_fd[1], sh_fd[2], EX_NOFUNC);
  for (k = 0; k < 3; k++) sh_fd[k] = save[k];
  return st;
}


static int has_ext (const char *s, const char *ext) {
  size_t n = strlen(s), e = strlen(ext);
  return n > e && m_stricmp(s + n - e, ext) == 0;
}


/* a builtin, or a script Windows cannot start by itself: in a child mmc
** that gets our exported variables and runs just this command */
static int spawn_stage_cmd (int argc, char **argv, const int *fds, OsProc *proc, long *pid) {
  static int counter = 0;
  Buf b;
  Vec env;
  char *dir = path_tmpdir(), name[80], *file, *a[5], pidbuf[24];
  int fd, k, r;
  size_t i;
  vec_init(&env);
  var_env(&env);
  buf_init(&b);
  for (i = 0; i < env.n; i++) {	/* the child sets some up its own way: ours win */
    const char *eq = strchr(env.v[i], '=');
    char *val;
    if (eq == NULL || !is_name_n(env.v[i], (size_t)(eq - env.v[i]))) continue;
    val = shell_quote(eq + 1);
    buf_puts(&b, "export ");
    buf_putn(&b, env.v[i], (size_t)(eq - env.v[i]) + 1);
    buf_puts(&b, val);
    buf_puts(&b, " 2>/dev/null\n");
    free(val);
  }
  buf_puts(&b, "command");
  for (k = 0; k < argc; k++) {
    char *qa = shell_quote(argv[k]);
    buf_putc(&b, ' ');
    buf_puts(&b, qa);
    free(qa);
  }
  buf_putc(&b, '\n');
  sprintf(name, "mmc-run-%ld-%d.sh", os_getpid(), ++counter);
  file = path_join(dir, name);
  free(dir);
  if ((fd = os_open(file, OS_WRITE)) < 0) {
    buf_free(&b);
    vec_free(&env);
    free(file);
    return -1;
  }
  os_write(fd, b.s, b.len);
  os_close(fd);
  buf_free(&b);
  a[0] = (char *)mmc_exe();
  a[1] = "--stage";
  a[2] = file;
  a[3] = ll_to_str(os_getpid(), pidbuf);
  a[4] = NULL;
  r = os_spawn(mmc_exe(), a, env.v, fds, 3, proc, pid);
  if (r != 0) os_unlink(file);
  vec_free(&env);
  free(file);
  return r;
}


/* starts the command side by side: 0 ok, 127 not found, 126 cannot run */
static int spawn_cmd (int argc, char **argv, const int *fds, OsProc *proc, long *pid) {
  char *exe = NULL;
  int kind = cmd_find(argv[0], &exe), r;
  if (kind == 0) return 127;
  if (kind == 2 && (MMC_SEP == '/' || has_ext(exe, ".exe") || has_ext(exe, ".com") ||
                    has_ext(exe, ".bat") || has_ext(exe, ".cmd"))) {
    Vec args, env;
    int k;
    vec_init(&args);
    vec_push(&args, xstrdup(argv[0]));
    for (k = 1; k < argc; k++) vec_push(&args, path_arg_to_native(argv[k]));	/* /d/x -> D:\x */
    vec_init(&env);
    var_env(&env);
    r = os_spawn(exe, args.v, env.v, fds, 3, proc, pid);
    vec_free(&args);
    vec_free(&env);
  }
  else r = spawn_stage_cmd(argc, argv, fds, proc, pid);
  free(exe);
  return r == 0 ? 0 : 126;
}


static int dev_open (const char *path) {
  char *n = path_to_native(path);
  int fd = os_open(n, OS_READ);
  free(n);
  return fd;
}


/* a duration: 1.5, 10s, 2m, 1h, 1d; -1 if bad */
static double parse_duration (const char *s) {
  char *end;
  double v = strtod(s, &end);
  if (end == s || !(v >= 0)) return -1;
  if (*end) {
    if (end[1] != '\0') return -1;
    if (*end == 'm') v *= 60;
    else if (*end == 'h') v *= 3600;
    else if (*end == 'd') v *= 86400;
    else if (*end != 's') return -1;
  }
  return v;
}


int t_nohup (int argc, char **argv, int in, int out, int err) {
  int i = 1, nin = -1, nout = -1, nerr = -1, status;
  int ignoring_input, redirect_out, redirect_err;
  const char *trap;
  char *exe = NULL;
  if ((status = std_opts(argc, argv, "nohup", out, err, 125, &i)) >= 0) return status;
  if (i >= argc) {
    tool_err(err, "nohup", "missing operand");
    return try_help(err, "nohup", 125);
  }
  if (cmd_find(argv[i], &exe) == 0) {
    tool_err(err, "nohup", "failed to run command %s: No such file or directory", q(argv[i]));
    return 127;
  }
  free(exe);
  ignoring_input = os_is_tty(in);
  redirect_out = os_is_tty(out);
  redirect_err = os_is_tty(err);
  if (ignoring_input) nin = dev_open("/dev/null");
  if (redirect_out) {	/* nohup.out here, else in $HOME */
    char *native = path_to_native("nohup.out"), *shown = xstrdup("nohup.out");
    const char *home = var_get("HOME");
    nout = os_open(native, OS_APPEND);
    free(native);
    if (nout < 0 && home && *home) {
      free(shown);
      shown = tool_join(home, "nohup.out");
      native = path_to_native(shown);
      nout = os_open(native, OS_APPEND);
      free(native);
    }
    if (nout < 0) {
      tool_err(err, "nohup", "failed to open %s: %s", q(shown), os_errmsg());
      free(shown);
      if (nin >= 0) os_close(nin);
      return 125;
    }
    tool_err(err, "nohup", ignoring_input ? "ignoring input and appending output to %s" :
             "appending output to %s", q(shown));
    free(shown);
  }
  if (redirect_err) {
    if (!redirect_out)
      tool_err(err, "nohup", ignoring_input ? "ignoring input and redirecting stderr to stdout" :
               "redirecting stderr to stdout");
    nerr = nout >= 0 ? nout : out;
  }
  else if (ignoring_input && !redirect_out) tool_err(err, "nohup", "ignoring input");
  os_catch_signal(1, 2);	/* SIGHUP ignored: so for what it starts too */
  status = run_here(argc - i, argv + i, nin, nout, nerr);
  trap = trap_get(1);
  os_catch_signal(1, trap == NULL ? 0 : *trap == '\0' ? 2 : 1);
  if (nin >= 0) os_close(nin);
  if (nout >= 0) os_close(nout);
  return status;
}


static int timeout_bad (int err, const char *fmt, const char *a) {
  tool_err(err, "timeout", fmt, a);
  return try_help(err, "timeout", 125);
}


static int timeout_run (Opts *g, int out, int err) {
  LongOpt lo[] = {{"signal", 's', 1}, {"kill-after", 'k', 1}, {"preserve-status", 'p', 0},
                  {"foreground", 'f', 0}, {"verbose", 'v', 0}, {"version", OPT_VERSION, 0},
                  {NULL, 0, 0}};
  int c, sig = 15, preserve = 0, verbose = 0, fds[3], r, timed_out = 0, killed = 0, status = 0;
  int naps = 0;
  double dur, kill_after = 0;
  long long deadline, kill_deadline = 0;
  OsProc proc;
  long pid;
  g->no_permute = 1;
  while ((c = opts_next(g, "s:k:v", lo)) != 0) {
    switch (c) {
      case 's':
        sig = trap_signum(g->arg);
        if (sig <= 0 || sig > 64) return timeout_bad(err, "%s: invalid signal", q(g->arg));
        break;
      case 'k':
        if ((kill_after = parse_duration(g->arg)) < 0)
          return timeout_bad(err, "invalid time interval %s", q(g->arg));
        break;
      case 'p': preserve = 1; break;
      case 'f': break;
      case 'v': verbose = 1; break;
      case OPT_HELP: return tool_help(out, "timeout");
      case OPT_VERSION: return tool_version(out, "timeout");
      default: return 125;
    }
  }
  if (g->ops.n < 2) return try_help(err, "timeout", 125);	/* as GNU: no more than that */
  if ((dur = parse_duration(g->ops.v[0])) < 0)
    return timeout_bad(err, "invalid time interval %s", q(g->ops.v[0]));
  fds[0] = sh_fd[0];
  fds[1] = sh_fd[1];
  fds[2] = sh_fd[2];
  r = spawn_cmd((int)g->ops.n - 1, g->ops.v + 1, fds, &proc, &pid);
  if (r != 0) {
    if (r == 127) tool_err(err, "timeout", "failed to run command %s: No such file or directory", q(g->ops.v[1]));
    return r;
  }
  deadline = os_now_us() + (long long)(dur * 1e6);
  while (os_poll_proc(proc, &status) != 1) {
    long long now = os_now_us(), left;
    int ms = naps < 20 ? 1 : naps < 100 ? 5 : 20;	/* short naps first: quick commands end quickly */
    if (tool_stop()) {	/* Ctrl-C: it goes too */
      os_kill_tree(pid, 2);
      os_wait(proc);
      return 130;
    }
    if (!timed_out && dur > 0 && now >= deadline) {
      timed_out = 1;
      if (verbose) tool_err(err, "timeout", "sending signal %s to command %s", trap_signame(sig), q(g->ops.v[1]));
      os_kill_tree(pid, sig);
      kill_deadline = now + (long long)(kill_after * 1e6);
    }
    else if (timed_out && kill_after > 0 && !killed && now >= kill_deadline) {
      killed = 1;
      if (verbose) tool_err(err, "timeout", "sending signal KILL to command %s", q(g->ops.v[1]));
      os_kill_tree(pid, 9);
    }
    left = (timed_out ? kill_deadline : deadline) - now;
    if ((dur > 0 || timed_out) && left > 0 && left / 1000 < ms) ms = (int)(left / 1000) + 1;
    os_sleep_ms(ms);
    naps++;
  }
  if (killed || (timed_out && sig == 9 && !preserve)) return 128 + 9;
  if (timed_out && !preserve) return 124;
  return status;
}


int t_timeout (int argc, char **argv, int in, int out, int err) {
  Opts g;
  int r;
  (void)in;
  opts_init(&g, "timeout", argc, argv, err);
  r = timeout_run(&g, out, err);
  opts_free(&g);
  return r;
}


/* xargs: the input, the command lines it makes, the children of -P */
typedef struct XArgs {
  int fd, own_fd, eof;
  unsigned char buf[65536];
  size_t pos, len;
  int delim;	/* -0 / -d: that byte; -1: blanks and quotes */
  const char *eofstr, *repl;
  long max_args, max_lines, max_chars, procs;
  int no_empty, verbose, prompt, exit_big, from_file;
  Vec init;	/* the command and its first arguments */
  size_t init_chars;
  int status, stop, err;
  int cmd_in;	/* what the commands read: /dev/null */
  OsProc *kids;
  long *kid_pids;
  int nkids;
  char *cmdname;
} XArgs;


static int x_getc (XArgs *x) {
  if (x->pos >= x->len) {
    long n;
    if (x->eof) return -1;
    n = os_read(x->fd, x->buf, sizeof(x->buf));
    if (n <= 0 || tool_stop()) {
      x->eof = 1;
      return -1;
    }
    x->len = (size_t)n;
    x->pos = 0;
  }
  return x->buf[x->pos++];
}


static int x_blank (int c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}


static int x_quote_err (XArgs *x, int quote) {
  tool_err(x->err, "xargs", "unmatched %s quote; by default quotes are special to xargs "
           "unless you use the -0 option", quote == '"' ? "double" : "single");
  return -1;
}


/* the next item: 1 got one, 0 the end, -1 an error. *eol: its line ended
** with it (-L counts lines; a blank at a line's end makes it go on) */
static int x_item (XArgs *x, Buf *item, int *eol, int whole_line) {
  int c, quote = 0;
  buf_free(item);
  buf_putn(item, "", 0);
  *eol = 0;
  if (x->delim >= 0) {	/* -0, -d: nothing is special */
    int got = 0;
    while ((c = x_getc(x)) >= 0) {
      got = 1;
      if (c == x->delim) {
        *eol = 1;
        return 1;
      }
      buf_putc(item, (char)c);
    }
    return got;
  }
  do c = x_getc(x);	/* blanks and empty lines before it */
  while (c >= 0 && x_blank(c));
  if (c < 0) return 0;
  for (;; c = x_getc(x)) {
    if (c < 0) {
      if (quote) return x_quote_err(x, quote);
      break;
    }
    if (quote) {
      if (c == quote) quote = 0;
      else if (c == '\n') return x_quote_err(x, quote);
      else buf_putc(item, (char)c);
      continue;
    }
    if (c == '\n') {
      *eol = 1;
      break;
    }
    if (x_blank(c) && !whole_line) {	/* blanks up to the newline: this line */
      int d;
      do d = x_getc(x);
      while (d >= 0 && d != '\n' && x_blank(d));
      if (d == '\n') *eol = x->max_lines == 0;
      else if (d >= 0) x->pos--;	/* the next item's first byte */
      break;
    }
    if (c == '\'' || c == '"') quote = c;
    else if (c == '\\') {
      int d = x_getc(x);
      if (d < 0) break;
      buf_putc(item, (char)d);
    }
    else buf_putc(item, (char)c);
  }
  if (x->eofstr != NULL && strcmp(item->s, x->eofstr) == 0) {
    x->eof = 1;
    x->pos = x->len;
    return 0;
  }
  return 1;
}


static void x_show (XArgs *x, Vec *argv) {
  Buf b;
  size_t i;
  buf_init(&b);
  for (i = 0; i < argv->n; i++) {
    if (i) buf_putc(&b, ' ');
    buf_puts(&b, argv->v[i]);
  }
  buf_puts(&b, x->prompt ? " ?..." : "\n");
  fd_puts(x->err, b.s);
  buf_free(&b);
}


/* a finished command's status, the GNU way */
static void x_status (XArgs *x, int st) {
  if (st == 255) {
    tool_err(x->err, "xargs", "%s: exited with status 255; aborting", x->cmdname);
    x->status = 124;
    x->stop = 1;
  }
  else if (st != 0 && x->status == 0) x->status = 123;
}


static void x_reap (XArgs *x, int block) {
  for (;;) {
    int k, done = 0;
    for (k = 0; k < x->nkids; k++) {
      int st;
      if (os_poll_proc(x->kids[k], &st) == 1) {
        x_status(x, st);
        x->kids[k] = x->kids[x->nkids - 1];
        x->kid_pids[k] = x->kid_pids[x->nkids - 1];
        x->nkids--;
        k--;
        done = 1;
      }
    }
    if (!block || done || x->nkids == 0) return;
    if (tool_stop()) {
      for (k = 0; k < x->nkids; k++) {
        os_kill_tree(x->kid_pids[k], 2);
        os_wait(x->kids[k]);
      }
      x->nkids = 0;
      x->stop = 1;
      x->status = 130;
      return;
    }
    os_sleep_ms(2);
  }
}


static void x_run (XArgs *x, Vec *argv) {
  char *exe = NULL;
  int kind;
  if (x->stop) return;
  free(x->cmdname);
  x->cmdname = xstrdup(argv->v[0]);
  if (x->verbose) x_show(x, argv);
  if (x->prompt) {	/* -p: the terminal says */
    int fd = dev_open("/dev/tty"), yes;
    yes = tool_ask(fd >= 0 ? fd : sh_fd[0], x->err, "%s", "");
    if (fd >= 0) os_close(fd);
    if (!yes) return;
  }
  kind = cmd_find(argv->v[0], &exe);
  free(exe);
  if (kind == 0) {
    tool_err(x->err, "xargs", "%s: No such file or directory", argv->v[0]);
    x->status = 127;
    x->stop = 1;
    return;
  }
  if (x->procs != 1) {	/* side by side */
    int fds[3], r;
    OsProc proc;
    long pid;
    while (x->procs > 0 && x->nkids >= x->procs && !x->stop) x_reap(x, 1);
    if (x->stop) return;
    fds[0] = x->cmd_in;
    fds[1] = sh_fd[1];
    fds[2] = sh_fd[2];
    r = spawn_cmd((int)argv->n, argv->v, fds, &proc, &pid);
    if (r != 0) {
      tool_err(x->err, "xargs", "%s: %s", argv->v[0],
               r == 127 ? "No such file or directory" : "Permission denied");
      x->status = r;
      x->stop = 1;
      return;
    }
    x->kids = (OsProc *)xrealloc(x->kids, (size_t)(x->nkids + 1) * sizeof(OsProc));
    x->kid_pids = (long *)xrealloc(x->kid_pids, (size_t)(x->nkids + 1) * sizeof(long));
    x->kids[x->nkids] = proc;
    x->kid_pids[x->nkids++] = pid;
    x_reap(x, 0);
    return;
  }
  x_status(x, run_here((int)argv->n, argv->v, x->cmd_in, -1, -1));
  if (tool_stop()) {
    x->stop = 1;
    x->status = 130;
  }
}


/* -I: the replace string in each first argument (not the command)
** becomes the item */
static void x_replace (XArgs *x, Vec *argv, const char *item) {
  size_t i, rl = strlen(x->repl);
  for (i = 0; i < x->init.n; i++) {
    const char *a = x->init.v[i], *p;
    Buf b;
    if (i == 0 || rl == 0 || strstr(a, x->repl) == NULL) {
      vec_push(argv, xstrdup(a));
      continue;
    }
    buf_init(&b);
    while ((p = strstr(a, x->repl)) != NULL) {
      buf_putn(&b, a, (size_t)(p - a));
      buf_puts(&b, item);
      a = p + rl;
    }
    buf_puts(&b, a);
    vec_push(argv, buf_take(&b));
  }
}


static long x_number (int err, const char *opt, const char *arg, long min) {
  long long v;
  if (str_to_ll(arg, &v) != 0) {
    tool_err(err, "xargs", "invalid number \"%s\" for -%s option", arg, opt);
    return try_help(err, "xargs", -1);
  }
  if (v < min) {
    tool_err(err, "xargs", "value %s for -%s option should be >= %ld", arg, opt, min);
    return try_help(err, "xargs", -1);
  }
  return v > 0x7FFFFFFFL ? 0x7FFFFFFFL : (long)v;
}


/* -d's argument: one character or an escape; -2 if bad */
static int x_delim (const char *s) {
  if (s[0] != '\\' || s[1] == '\0') return s[0] && !s[1] ? (unsigned char)s[0] : -2;
  if (strchr("ntrabfv\\", s[1]) && s[2] == '\0') {
    static const char from[] = "ntrabfv\\", to[] = "\n\t\r\a\b\f\v\\";
    return (unsigned char)to[strchr(from, s[1]) - from];
  }
  if (s[1] >= '0' && s[1] <= '7') {
    long v = strtol(s + 1, NULL, 8);
    return v < 256 ? (int)v : -2;
  }
  if (s[1] == 'x' && isxdigit((unsigned char)s[2])) {
    long v = strtol(s + 2, NULL, 16);
    return v < 256 ? (int)v : -2;
  }
  return -2;
}


static int xargs_options (XArgs *x, Opts *g, int out, int err, int *show_limits, const char **argfile) {
  LongOpt lo[] = {{"null", '0', 0}, {"delimiter", 'd', 1}, {"max-args", 'n', 1},
                  {"max-lines", 'l', 2}, {"replace", 'i', 2}, {"no-run-if-empty", 'r', 0},
                  {"verbose", 't', 0}, {"interactive", 'p', 0}, {"max-procs", 'P', 1},
                  {"max-chars", 's', 1}, {"arg-file", 'a', 1}, {"eof", 'e', 2}, {"exit", 'x', 0},
                  {"open-tty", 'o', 0}, {"show-limits", 'S', 0}, {"process-slot-var", 'V', 1},
                  {"version", OPT_VERSION, 0}, {NULL, 0, 0}};
  int c;
  g->no_permute = 1;
  while ((c = opts_next(g, "0d:n:L:I:rtpP:s:a:E:xoile", lo)) != 0) {
    const char *arg = g->arg;
    if ((c == 'i' || c == 'l' || c == 'e') && arg == NULL && g->cluster && *g->cluster) {
      arg = g->cluster;	/* -i{} -l5 -eEND: the rest of the word */
      g->cluster = NULL;
    }
    switch (c) {
      case '0': x->delim = 0; break;
      case 'd':
        if ((x->delim = x_delim(arg)) == -2) {
          tool_err(err, "xargs", "invalid input delimiter specification %s: the delimiter must "
                   "be either a single character or an escape sequence starting with \\.", arg);
          return -1;
        }
        break;
      case 'n':
        if ((x->max_args = x_number(err, "n", arg, 1)) < 0) return -1;
        x->max_lines = 0;
        break;
      case 'L': case 'l':
        if ((x->max_lines = arg == NULL ? 1 : x_number(err, "L", arg, 1)) < 0) return -1;
        x->max_args = 0;
        x->repl = NULL;
        break;
      case 'I': case 'i':
        x->repl = arg ? arg : "{}";
        x->max_lines = x->max_args = 0;
        break;
      case 'r': x->no_empty = 1; break;
      case 't': x->verbose = 1; break;
      case 'p': x->prompt = x->verbose = 1; break;
      case 'P': if ((x->procs = x_number(err, "P", arg, 0)) < 0) return -1; break;
      case 's': if ((x->max_chars = x_number(err, "s", arg, 1)) < 0) return -1; break;
      case 'a': *argfile = arg; break;
      case 'E': case 'e': x->eofstr = arg && *arg ? arg : NULL; break;
      case 'x': x->exit_big = 1; break;
      case 'o': x->cmd_in = -2; break;
      case 'S': *show_limits = 1; break;
      case 'V': break;
      case OPT_HELP: tool_help(out, "xargs"); return 0;
      case OPT_VERSION: tool_version(out, "xargs"); return 0;
      default: return -1;
    }
  }
  return 1;
}


static void x_flush_cmd (XArgs *x, Vec *cmd, size_t *chars, size_t *nargs, long *lines) {
  x_run(x, cmd);
  vec_free(cmd);
  vec_init(cmd);
  vec_copy(cmd, x->init.v, x->init.n);
  *chars = x->init_chars;
  *nargs = 0;
  *lines = 0;
}


static void xargs_loop (XArgs *x) {
  Buf item;
  Vec cmd;
  size_t chars = x->init_chars, nargs = 0;
  long lines = 0;
  int r = 0, eol, ran = 0, got_any = 0;
  buf_init(&item);
  vec_init(&cmd);
  if (x->repl != NULL) {	/* -I: a command for each line */
    while (!x->stop && (r = x_item(x, &item, &eol, 1)) > 0) {
      size_t k, n = 0;
      vec_free(&cmd);
      vec_init(&cmd);
      x_replace(x, &cmd, item.s);
      for (k = 0; k < cmd.n; k++) n += strlen(cmd.v[k]) + 1;
      if (n > (size_t)x->max_chars) {
        tool_err(x->err, "xargs", "argument line too long");
        x->status = 1;
        break;
      }
      x_run(x, &cmd);
    }
    if (r < 0) x->status = 1;
    vec_free(&cmd);
    buf_free(&item);
    return;
  }
  vec_copy(&cmd, x->init.v, x->init.n);
  while (!x->stop) {
    if ((r = x_item(x, &item, &eol, 0)) <= 0) {
      if (r < 0) x->status = 1;
      break;
    }
    got_any = 1;
    if (chars + item.len + 1 > (size_t)x->max_chars) {
      if (nargs == 0 || (x->exit_big && (x->max_args || x->max_lines))) {
        tool_err(x->err, "xargs", "argument line too long");
        x->status = 1;
        nargs = 0;
        break;
      }
      x_flush_cmd(x, &cmd, &chars, &nargs, &lines);	/* this one goes into the next */
      ran = 1;
    }
    vec_push(&cmd, xstrdup(item.s));
    chars += item.len + 1;
    nargs++;
    if (eol) lines++;
    if ((x->max_args && (long)nargs >= x->max_args) || (x->max_lines && lines >= x->max_lines)) {
      x_flush_cmd(x, &cmd, &chars, &nargs, &lines);
      ran = 1;
    }
    if (tool_stop()) {
      x->stop = 1;
      x->status = 130;
    }
  }
  if (!x->stop && r >= 0 && (nargs > 0 || (!ran && !got_any && !x->no_empty))) x_run(x, &cmd);
  vec_free(&cmd);
  buf_free(&item);
}


int t_xargs (int argc, char **argv, int in, int out, int err) {
  Opts g;
  XArgs *x = (XArgs *)xmalloc(sizeof(XArgs));
  int r, show_limits = 0;
  const char *argfile = NULL;
  long env_bytes = 0, upper;
  Vec env;
  size_t i;
  memset(x, 0, sizeof(*x));
  x->delim = -1;
  x->procs = 1;
  x->err = err;
  x->fd = in;
  vec_init(&x->init);
  opts_init(&g, "xargs", argc, argv, err);
  r = xargs_options(x, &g, out, err, &show_limits, &argfile);
  if (r <= 0) {
    opts_free(&g);
    free(x);
    return r < 0 ? 1 : 0;
  }
  if (g.ops.n == 0) vec_push(&x->init, xstrdup("echo"));
  for (i = 0; i < g.ops.n; i++) vec_push(&x->init, xstrdup(g.ops.v[i]));
  opts_free(&g);
  if (x->repl != NULL) x->exit_big = 1;
  vec_init(&env);	/* the command line limit, worked out as GNU does */
  var_env(&env);
  for (i = 0; i < env.n; i++) env_bytes += (long)strlen(env.v[i]) + 1;
  vec_free(&env);
  upper = os_arg_max() - env_bytes - 2048;
  if (upper < 4096) upper = 4096;
  if (show_limits) {
    fd_printf(err, "Your environment variables take up %ld bytes\n", env_bytes);
    fd_printf(err, "POSIX upper limit on argument length (this system): %ld\n", upper);
    fd_printf(err, "POSIX smallest allowable upper limit on argument length (all systems): 4096\n");
    fd_printf(err, "Maximum length of command we could actually use: %ld\n", upper - env_bytes);
    fd_printf(err, "Size of command buffer we are actually using: %ld\n",
              x->max_chars ? x->max_chars : upper < 131072 ? upper : 131072);
    fd_printf(err, "Maximum parallelism (--max-procs must be no greater): 2147483647\n\n");
  }
  if (x->max_chars == 0) x->max_chars = upper < 131072 ? upper : 131072;
  else if (x->max_chars > upper) {
    tool_err(err, "xargs", "value for -s option should be <= %ld", upper);
    x->max_chars = upper;
  }
  for (i = 0; i < x->init.n; i++) x->init_chars += strlen(x->init.v[i]) + 1;
  if (x->init_chars > (size_t)x->max_chars) {
    tool_err(err, "xargs", "argument list too long");
    vec_free(&x->init);
    free(x);
    return 1;
  }
  if (argfile != NULL) {
    char *native = path_to_native(argfile);
    x->fd = strcmp(argfile, "-") == 0 ? in : os_open(native, OS_READ);
    free(native);
    if (x->fd < 0) {
      tool_err(err, "xargs", "Cannot open input file %s: %s", q(argfile), os_errmsg());
      vec_free(&x->init);
      free(x);
      return 1;
    }
    x->own_fd = x->fd != in;
    x->from_file = 1;
  }
  /* what the commands read: -o the terminal, -a our input, else /dev/null */
  r = x->cmd_in == -2 ? dev_open("/dev/tty") : x->from_file ? -1 : dev_open("/dev/null");
  x->cmd_in = r >= 0 ? r : in;
  xargs_loop(x);
  while (x->nkids > 0) x_reap(x, 1);
  if (x->own_fd) os_close(x->fd);
  if (r >= 0) os_close(r);
  r = x->status;
  free(x->kids);
  free(x->kid_pids);
  free(x->cmdname);
  vec_free(&x->init);
  free(x);
  return r;
}

/* }================================================================== */

/* {==================================================================
** cygpath: git-bash's path converter, so scripts written for git-bash run
** here too. -u (default) gives /e/x, -w E:\x, -m E:/x; relative paths stay
** relative unless -a; -p converts a whole PATH-style list.
** =================================================================== */

static int cyg_is_abs (const char *p) {
#ifdef _WIN32
  if (p[0] == '\\' || (isalpha((unsigned char)p[0]) && p[1] == ':')) return 1;
#endif
  return p[0] == '/';
}

static char *cyg_seps (char *s, char from, char to) {
  char *q;
  for (q = s; *q; q++)
    if (*q == from) *q = to;
  return s;
}

static char *cyg_one (const char *p, int mode, int absolute) {
  char *native, *r;
  if (!cyg_is_abs(p) && !absolute) {
    r = xstrdup(p);
    return mode == 'w' ? cyg_seps(r, '/', '\\') : cyg_seps(r, '\\', '/');
  }
  if (!cyg_is_abs(p)) {
    char *cwd = os_getcwd();
    char *rel = path_to_native(p);
    native = path_join(cwd, rel);
    free(cwd);
    free(rel);
  } else {
    native = path_to_native(p);
  }
  if (mode == 'u') {
    r = path_to_display(native);
    free(native);
    return r;
  }
  return mode == 'm' ? cyg_seps(native, '\\', '/') : native;
}

int t_cygpath (int argc, char **argv, int in, int out, int err) {
  LongOpt lo[] = {{"unix", 'u', 0}, {"windows", 'w', 0}, {"mixed", 'm', 0},
                  {"absolute", 'a', 0}, {"path", 'p', 0},
                  {"version", OPT_VERSION, 0}, {NULL, 0, 0}};
  Opts g;
  int c, mode = 'u', absolute = 0, list = 0;
  size_t i;
  Out o;
  (void)in;
  opts_init(&g, "cygpath", argc, argv, err);
  while ((c = opts_next(&g, "uwmap", lo)) != 0) {
    switch (c) {
      case 'u': case 'w': case 'm': mode = c; break;
      case 'a': absolute = 1; break;
      case 'p': list = 1; break;
      case OPT_HELP: opts_free(&g); return tool_help(out, "cygpath");
      case OPT_VERSION: opts_free(&g); return tool_version(out, "cygpath");
      default: opts_free(&g); return 2;
    }
  }
  if (g.ops.n == 0) {
    tool_err(err, "cygpath", "missing operand");
    opts_free(&g);
    return 2;
  }
  out_init(&o, out);
  for (i = 0; i < g.ops.n; i++) {
    const char *a = g.ops.v[i];
    if (list) {	/* "a:b" or "a;b" in, the target style's separator out */
      char sep_in = strchr(a, ';') != NULL ? ';' : ':';
      const char *s = a;
      int first = 1;
      for (;;) {
        const char *e = strchr(s, sep_in);
        size_t n = e ? (size_t)(e - s) : strlen(s);
        char *part = xstrndup(s, n), *conv;
        if (!first) out_putc(&o, mode == 'u' ? ':' : ';');
        conv = part[0] ? cyg_one(part, mode, absolute) : xstrdup("");
        out_puts(&o, conv);
        free(conv);
        free(part);
        first = 0;
        if (e == NULL) break;
        s = e + 1;
      }
    } else {
      char *conv = cyg_one(a, mode, absolute);
      out_puts(&o, conv);
      free(conv);
    }
    out_putc(&o, '\n');
  }
  out_flush(&o);
  opts_free(&g);
  return 0;
}

/* }================================================================== */
