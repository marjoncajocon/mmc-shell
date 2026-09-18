/*
** mbio.c - echo, printf, read, mapfile
*/

#include "mmc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* \n \t \0nnn \xHH \uHHHH ... into b; returns 1 at \c (stop everything) */
static int put_escapes (Buf *b, const char *s, int echo_octal) {
  while (*s) {
    char c = *s++;
    if (c != '\\' || *s == '\0') {
      buf_putc(b, c);
      continue;
    }
    c = *s++;
    switch (c) {
      case 'a': buf_putc(b, '\a'); break;
      case 'b': buf_putc(b, '\b'); break;
      case 'c': return 1;
      case 'e': case 'E': buf_putc(b, '\033'); break;
      case 'f': buf_putc(b, '\f'); break;
      case 'n': buf_putc(b, '\n'); break;
      case 'r': buf_putc(b, '\r'); break;
      case 't': buf_putc(b, '\t'); break;
      case 'v': buf_putc(b, '\v'); break;
      case '\\': buf_putc(b, '\\'); break;
      case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
        int v = 0, k = 0, max = 3;
        if (echo_octal) {	/* echo: \0nnn, the 0 does not count */
          if (c != '0') {
            buf_putc(b, '\\');
            buf_putc(b, c);
            break;
          }
        }
        else {
          v = c - '0';
          k = 1;
        }
        while (k < max && *s >= '0' && *s <= '7') {
          v = v * 8 + (*s++ - '0');
          k++;
        }
        buf_putc(b, (char)v);
        break;
      }
      case 'x':
      case 'u':
      case 'U': {
        char tmp[16];
        size_t used = 0;
        char *r;
        int max = (c == 'x') ? 2 : (c == 'u') ? 4 : 8, k = 0;
        tmp[0] = '\\';
        tmp[1] = c;
        while (k < max && isxdigit((unsigned char)s[k])) {
          tmp[2 + k] = s[k];
          k++;
        }
        if (k == 0) {
          buf_putc(b, '\\');
          buf_putc(b, c);
          break;
        }
        r = expand_ansi_c(tmp, (size_t)(2 + k), NULL);
        buf_puts(b, r);
        free(r);
        (void)used;
        s += k;
        break;
      }
      default:
        buf_putc(b, '\\');
        buf_putc(b, c);
        break;
    }
  }
  return 0;
}


int b_echo (int argc, char **argv, int in, int out, int err) {
  Buf b;
  int i = 1, newline = 1, escapes = O("xpg_echo");
  (void)in; (void)err;
  for (; i < argc; i++) {	/* -n -e -E, alone or together: "-ne" */
    const char *a = argv[i];
    int ok = a[0] == '-' && a[1] != '\0', k;
    for (k = 1; ok && a[k]; k++) ok = strchr("neE", a[k]) != NULL;
    if (!ok) break;
    for (k = 1; a[k]; k++) {
      if (a[k] == 'n') newline = 0;
      else if (a[k] == 'e') escapes = 1;
      else escapes = 0;
    }
  }
  buf_init(&b);
  for (; i < argc; i++) {
    if (escapes) {
      if (put_escapes(&b, argv[i], 1)) {
        newline = 0;
        break;
      }
    }
    else buf_puts(&b, argv[i]);
    if (i + 1 < argc) buf_putc(&b, ' ');
  }
  if (newline) buf_putc(&b, '\n');
  if (b.len > 0 && os_write(out, b.s, b.len) < 0) {
    buf_free(&b);
    return 1;
  }
  buf_free(&b);
  return 0;
}


/*
** {==================================================================
** printf
** ===================================================================
*/

/* %q: bash's way, with backslashes, or $'...' for control characters */
static char *quote_q (const char *s) {
  Buf b;
  const char *p;
  int ctrl = 0;
  if (*s == '\0') return xstrdup("''");
  for (p = s; *p; p++)
    if ((unsigned char)*p < 32 || *p == 127) ctrl = 1;
  buf_init(&b);
  if (ctrl) {
    buf_puts(&b, "$'");
    for (p = s; *p; p++) {
      unsigned char c = (unsigned char)*p;
      switch (c) {
        case '\n': buf_puts(&b, "\\n"); break;
        case '\t': buf_puts(&b, "\\t"); break;
        case '\r': buf_puts(&b, "\\r"); break;
        case '\033': buf_puts(&b, "\\E"); break;
        case '\a': buf_puts(&b, "\\a"); break;
        case '\b': buf_puts(&b, "\\b"); break;
        case '\f': buf_puts(&b, "\\f"); break;
        case '\v': buf_puts(&b, "\\v"); break;
        case '\'': buf_puts(&b, "\\'"); break;
        case '\\': buf_puts(&b, "\\\\"); break;
        default:
          if (c < 32 || c == 127) buf_printf(&b, "\\%03o", c);
          else buf_putc(&b, (char)c);
      }
    }
    buf_putc(&b, '\'');
    return buf_take(&b);
  }
  for (p = s; *p; p++) {
    if (strchr(" !\"#$&'()*,;<>?[\\]^`{|}", *p) != NULL || (p == s && *p == '~'))
      buf_putc(&b, '\\');
    buf_putc(&b, *p);
  }
  return buf_take(&b);
}


/* a number argument: 'c gives the character code */
static long long num_arg (const char *s, int *bad) {
  long long v = 0;
  char *end;
  *bad = 0;
  if (s == NULL || *s == '\0') return 0;
  if (*s == '\'' || *s == '"') return (unsigned char)s[1];
  while (isspace((unsigned char)*s)) s++;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) v = (long long)strtoull(s, &end, 16);
  else if (s[0] == '0' && isdigit((unsigned char)s[1])) v = (long long)strtoull(s, &end, 8);
  else if (*s == '-' || *s == '+') v = strtoll(s, &end, 10);
  else v = (long long)strtoull(s, &end, 10);
  if (*end != '\0' && !isspace((unsigned char)*end)) {
    *bad = 1;
    sh_error("printf: %s: invalid number", s);
  }
  return v;
}


static double float_arg (const char *s, int *bad) {
  char *end;
  double v;
  *bad = 0;
  if (s == NULL || *s == '\0') return 0.0;
  if (*s == '\'' || *s == '"') return (double)(unsigned char)s[1];
  v = strtod(s, &end);
  if (*end != '\0') {
    *bad = 1;
    sh_error("printf: %s: invalid number", s);
  }
  return v;
}


int b_printf (int argc, char **argv, int in, int out, int err) {
  const char *var = NULL, *fmt;
  Buf b;
  int ai, status = 0, first = 1;
  (void)in; (void)err;
  ai = 1;
  if (ai < argc && strcmp(argv[ai], "-v") == 0 && ai + 1 < argc) {
    var = argv[ai + 1];
    ai += 2;
  }
  if (ai < argc && strcmp(argv[ai], "--") == 0) ai++;
  if (ai >= argc) {
    sh_error("printf: usage: printf [-v var] format [arguments]");
    return 2;
  }
  fmt = argv[ai++];
  buf_init(&b);
  /* the format is used again while arguments are left */
  while (first || ai < argc) {
    const char *p = fmt;
    int consumed = 0, stop = 0;
    first = 0;
    while (*p && !stop) {
      if (*p == '\\') {
        char one[16];
        int k = 0;
        one[k++] = *p++;
        if (*p) {
          one[k++] = *p++;
          if (one[1] >= '0' && one[1] <= '7')	/* \NNN */
            while (k < 4 && *p >= '0' && *p <= '7') one[k++] = *p++;
          else if (one[1] == 'x' || one[1] == 'u' || one[1] == 'U') {
            int max = one[1] == 'x' ? 2 : one[1] == 'u' ? 4 : 8, d = 0;
            while (d < max && isxdigit((unsigned char)*p)) {
              one[k++] = *p++;
              d++;
            }
          }
        }
        one[k] = '\0';
        if (one[1] >= '0' && one[1] <= '7') {
          int v = 0, j;
          for (j = 1; j < k; j++) v = v * 8 + (one[j] - '0');
          buf_putc(&b, (char)v);
        }
        else if (one[1] == 'c') stop = 1;
        else put_escapes(&b, one, 0);
        continue;
      }
      if (*p != '%') {
        buf_putc(&b, *p++);
        continue;
      }
      p++;
      if (*p == '%') {
        buf_putc(&b, '%');
        p++;
        continue;
      }
      {
        char spec[64], conv;
        int k = 0, width_star = 0, prec_star = 0;
        const char *arg;
        spec[k++] = '%';
        while (*p && strchr("-+ #0'", *p) && k < 40) {
          if (*p != '\'') spec[k++] = *p;
          p++;
        }
        if (*p == '*') {
          width_star = 1;
          p++;
        }
        else while (isdigit((unsigned char)*p) && k < 50) spec[k++] = *p++;
        if (*p == '.') {
          spec[k++] = *p++;
          if (*p == '*') {
            prec_star = 1;
            p++;
          }
          else while (isdigit((unsigned char)*p) && k < 58) spec[k++] = *p++;
        }
        while (*p == 'l' || *p == 'h' || *p == 'L' || *p == 'j' || *p == 'z' ||
               *p == 't')
          p++;	/* length modifiers mean nothing to a shell */
        if (width_star || prec_star) {	/* * takes a number from the arguments */
          char tmp[64];
          int bad, w = 0, pr = 0, j, dot = -1;
          if (width_star) {
            w = (int)num_arg(ai < argc ? argv[ai++] : NULL, &bad);
            consumed = 1;
          }
          if (prec_star) {
            pr = (int)num_arg(ai < argc ? argv[ai++] : NULL, &bad);
            consumed = 1;
          }
          for (j = 0; j < k; j++)
            if (spec[j] == '.') dot = j;
          if (width_star && prec_star) sprintf(tmp, "%.*s%d.%d", dot, spec, w, pr);
          else if (width_star) sprintf(tmp, "%.*s%d%s", dot >= 0 ? dot : k, spec, w,
                                        dot >= 0 ? spec + dot : "");
          else sprintf(tmp, "%.*s.%d", dot, spec, pr);
          strcpy(spec, tmp);
          k = (int)strlen(spec);
        }
        conv = *p ? *p++ : 's';
        if (conv == '(') {	/* %(fmt)T */
          const char *close = strchr(p, ')');
          char tfmt[128], outb[256];
          long long t;
          int bad;
          time_t tt;
          struct tm *tm;
          if (close == NULL || close[1] != 'T') {
            buf_puts(&b, "%(");
            continue;
          }
          snprintf(tfmt, sizeof(tfmt), "%.*s", (int)(close - p), p);
          p = close + 2;
          arg = ai < argc ? argv[ai++] : NULL;
          consumed = 1;
          t = (arg == NULL || *arg == '\0' || strcmp(arg, "-1") == 0) ? (long long)time(NULL)
            : num_arg(arg, &bad);
          tt = (time_t)t;
          tm = localtime(&tt);
          if (tm && strftime(outb, sizeof(outb), tfmt[0] ? tfmt : "%X", tm)) {
            char sfmt[72];
            snprintf(sfmt, sizeof(sfmt), "%ss", spec);
            buf_printf(&b, sfmt, outb);
          }
          continue;
        }
        arg = ai < argc ? argv[ai++] : NULL;
        if (arg != NULL) consumed = 1;
        spec[k] = '\0';
        switch (conv) {
          case 'd': case 'i': {
            int bad;
            long long v = num_arg(arg, &bad);
            char f[72];
            if (bad) status = 1;
            snprintf(f, sizeof(f), "%slld", spec);
            buf_printf(&b, f, v);
            break;
          }
          case 'o': case 'u': case 'x': case 'X': {
            int bad;
            long long v = num_arg(arg, &bad);
            char f[72];
            if (bad) status = 1;
            snprintf(f, sizeof(f), "%sll%c", spec, conv);
            buf_printf(&b, f, (unsigned long long)v);
            break;
          }
          case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            int bad;
            double v = float_arg(arg, &bad);
            char f[72];
            if (bad) status = 1;
            snprintf(f, sizeof(f), "%s%c", spec, conv);
            buf_printf(&b, f, v);
            break;
          }
          case 'c': {
            char f[72];
            snprintf(f, sizeof(f), "%sc", spec);
            if (arg && *arg) buf_printf(&b, f, arg[0]);
            else if (arg) buf_printf(&b, f, '\0');
            break;
          }
          case 'b': {
            Buf e;
            char f[72];
            int stopped;
            buf_init(&e);
            stopped = put_escapes(&e, arg ? arg : "", 1);
            snprintf(f, sizeof(f), "%ss", spec);
            buf_printf(&b, f, e.s ? e.s : "");
            buf_free(&e);
            if (stopped) {
              stop = 1;
              ai = argc;
            }
            break;
          }
          case 'q':
          case 'Q': {
            char *q = quote_q(arg ? arg : "");
            char f[72];
            snprintf(f, sizeof(f), "%ss", spec);
            buf_printf(&b, f, q);
            free(q);
            break;
          }
          case 's': {
            char f[72];
            snprintf(f, sizeof(f), "%ss", spec);
            buf_printf(&b, f, arg ? arg : "");
            break;
          }
          default:
            sh_error("printf: `%c': invalid format character", conv);
            buf_free(&b);
            return 1;
        }
      }
    }
    if (stop || !consumed) break;	/* a format without conversions runs once */
  }
  if (var != NULL) {
    int r;
    char *s = b.s ? b.s : "";
    {
      const char *br = strchr(var, '[');
      if (br != NULL && var[strlen(var) - 1] == ']') {
        char *name = xstrndup(var, (size_t)(br - var));
        char *sub = xstrndup(br + 1, strlen(br + 1) - 1);
        int f = var_flags(name);
        if (f >= 0 && (f & V_ASSOC)) r = var_akset(name, sub, s);
        else {
          long long idx = 0;
          arith_eval(sub, &idx);
          r = var_aset(name, idx, s);
        }
        free(name);
        free(sub);
      }
      else r = var_set(var, s);
    }
    buf_free(&b);
    return r != 0 ? 1 : status;
  }
  if (b.len > 0 && os_write(out, b.s, b.len) < 0) status = 1;
  buf_free(&b);
  return status;
}

/* }================================================================== */


/*
** {==================================================================
** read
** ===================================================================
*/

/* one byte from fd; -1 EOF, -2 timeout */
static int read_byte (int fd, long long deadline_us) {
  unsigned char c;
  if (deadline_us > 0) {
    long long left = deadline_us - os_now_us();
    int r;
    if (left <= 0) return -2;
    r = os_wait_readable(fd, (int)(left / 1000) + 1);
    if (r == 0) return -2;
  }
  if (os_read(fd, &c, 1) != 1) return -1;
  return c;
}


static int is_ifs (int c, const char *ifs) {
  return c != '\0' && strchr(ifs, c) != NULL;
}


static int is_ifs_ws (int c, const char *ifs) {
  return (c == ' ' || c == '\t' || c == '\n') && is_ifs(c, ifs);
}


int b_read (int argc, char **argv, int in, int out, int err) {
  int raw = 0, silent = 0, nchars = -1, exact = 0, delim = '\n', fd = in, i;
  long long deadline = 0;
  const char *prompt = NULL, *array = NULL;
  Buf line, marks;	/* marks: 1 where a character was escaped */
  int c = 0, status = 0, raw_tty = 0;
  const char *ifs = var_get("IFS");
  (void)out; (void)err;
  if (ifs == NULL) ifs = " \t\n";
  for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
    const char *a = argv[i] + 1;
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    for (; *a; a++) {
      const char *val = NULL;
      if (strchr("pntdNuai", *a) != NULL) {
        if (a[1] != '\0') val = a + 1;
        else if (i + 1 < argc) val = argv[++i];
        else {
          sh_error("read: -%c: option requires an argument", *a);
          return 2;
        }
      }
      switch (*a) {
        case 'r': raw = 1; break;
        case 's': silent = 1; break;
        case 'e': break;
        case 'p': prompt = val; break;
        case 'n': nchars = atoi(val); break;
        case 'N': nchars = atoi(val); exact = 1; break;
        case 't': {
          double t = atof(val);
          if (t <= 0) {	/* read -t 0: is there input? */
            return os_wait_readable(fd, 0) > 0 ? 0 : 1;
          }
          deadline = os_now_us() + (long long)(t * 1e6);
          break;
        }
        case 'd': delim = val[0] ? (unsigned char)val[0] : 0; break;
        case 'u': {
          long long k;
          if (str_to_ll(val, &k) != 0 || k < 0 || k >= MMC_FDS || sh_fd[k] < 0) {
            sh_error("read: %s: invalid file descriptor specification", val);
            return 1;
          }
          fd = sh_fd[k];
          break;
        }
        case 'a': array = val; break;
        case 'i': break;
        default:
          sh_error("read: -%c: invalid option", *a);
          return 2;
      }
      if (val != NULL) break;
    }
  }
  if (prompt != NULL && os_is_tty(fd)) fd_puts(sh_fd[2] >= 0 ? sh_fd[2] : 2, prompt);
  if (silent && os_is_tty(fd) && fd == 0) raw_tty = (os_tty_raw(1) == 0);
  buf_init(&line);
  buf_init(&marks);
  for (;;) {
    if (nchars >= 0 && (int)utf8_count(line.s ? line.s : "", line.len) >= nchars) break;
    c = raw_tty ? os_tty_getbyte() : read_byte(fd, deadline);
    if (c == -2) {
      status = 142;	/* 128 + SIGALRM, like bash */
      break;
    }
    if (c < 0) {
      status = 1;
      break;
    }
    if (raw_tty && c == '\r') c = '\n';
    if (c == 3 && raw_tty) {
      status = 130;
      break;
    }
    if (c == '\r' && delim == '\n') {	/* CRLF input */
      int d = raw_tty ? '\n' : read_byte(fd, deadline);
      if (d == '\n') c = '\n';
      else {
        buf_putc(&line, '\r');
        buf_putc(&marks, 0);
        if (d < 0) {
          status = d == -2 ? 142 : 1;
          break;
        }
        c = d;
      }
    }
    if (!exact && c == delim) break;
    if (!raw && c == '\\') {
      int d = raw_tty ? os_tty_getbyte() : read_byte(fd, deadline);
      if (d < 0) {
        status = d == -2 ? 142 : 1;
        break;
      }
      if (d == '\n') continue;	/* a line continuation */
      buf_putc(&line, (char)d);
      buf_putc(&marks, 1);
      continue;
    }
    buf_putc(&line, (char)c);
    buf_putc(&marks, 0);
  }
  if (raw_tty) {
    os_tty_raw(0);
    fd_puts(2, "\n");
  }
  if (line.s == NULL) {
    buf_putc(&line, '\0');
    line.len = 0;
    buf_putc(&marks, '\0');
    marks.len = 0;
  }
  /* split the line into the variables */
  {
    Vec fields;
    const char *s = line.s, *m = marks.s;
    size_t n = line.len, k = 0;
    int nvars = argc - i;
    const char *last_name = NULL;
    vec_init(&fields);
    if (array != NULL || nvars != 1 || strcmp(argv[i], "REPLY") != 0 || 1) {
      /* leading IFS whitespace goes */
      while (k < n && !m[k] && is_ifs_ws((unsigned char)s[k], ifs)) k++;
      while (k < n) {
        Buf f;
        int field_count = (int)fields.n;
        buf_init(&f);
        /* the last variable takes the rest of the line */
        if (array == NULL && nvars > 0 && field_count == nvars - 1) {
          size_t end = n;
          while (end > k && !m[end - 1] && is_ifs_ws((unsigned char)s[end - 1], ifs)) end--;
          /* a trailing non-blank separator alone also goes */
          if (end > k && !m[end - 1] && is_ifs((unsigned char)s[end - 1], ifs) &&
              !is_ifs_ws((unsigned char)s[end - 1], ifs)) {
            size_t e2 = end - 1, j;
            int other = 0;
            for (j = k; j < e2; j++)
              if (!m[j] && is_ifs((unsigned char)s[j], ifs)) other = 1;
            if (!other) end = e2;
          }
          buf_putn(&f, s + k, end - k);
          if (f.s == NULL) buf_putc(&f, '\0'), f.len = 0;
          vec_push(&fields, buf_take(&f));
          k = n;
          break;
        }
        while (k < n && (m[k] || !is_ifs((unsigned char)s[k], ifs))) buf_putc(&f, s[k++]);
        vec_push(&fields, buf_take(&f));
        /* one separator: blanks around, and at most one non-blank */
        while (k < n && !m[k] && is_ifs_ws((unsigned char)s[k], ifs)) k++;
        if (k < n && !m[k] && is_ifs((unsigned char)s[k], ifs) &&
            !is_ifs_ws((unsigned char)s[k], ifs)) {
          k++;
          while (k < n && !m[k] && is_ifs_ws((unsigned char)s[k], ifs)) k++;
          if (k >= n && array == NULL && (int)fields.n < nvars) vec_push(&fields, xstrdup(""));
        }
      }
    }
    if (array != NULL) {
      size_t j;
      if (var_make_array(array, 0) != 0) status = 1;
      for (j = 0; j < fields.n; j++) var_aset(array, (long long)j, fields.v[j]);
    }
    else if (nvars <= 0) {	/* REPLY: the whole line, as it was */
      if (var_set("REPLY", line.s) != 0) status = 1;
    }
    else {
      int v;
      for (v = 0; v < nvars; v++) {
        const char *val = (size_t)v < fields.n ? fields.v[v] : "";
        last_name = argv[i + v];
        if (!is_name(last_name)) {
          sh_error("read: `%s': not a valid identifier", last_name);
          status = 1;
          break;
        }
        if (var_set(last_name, val) != 0) status = 1;
      }
    }
    vec_free(&fields);
  }
  buf_free(&line);
  buf_free(&marks);
  return status;
}


int b_mapfile (int argc, char **argv, int in, int out, int err) {
  int i, delim = '\n', trim = 0, fd = in;
  long long count = 0, origin = 0, skip = 0, n = 0;
  int keep_origin = 0;
  const char *name = "MAPFILE";
  Buf line;
  (void)out; (void)err;
  for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
    const char *a = argv[i] + 1;
    const char *val = NULL;
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    if (strchr("dnOsuCc", *a) != NULL) {
      val = a[1] ? a + 1 : (i + 1 < argc ? argv[++i] : "");
    }
    switch (*a) {
      case 'd': delim = val[0] ? (unsigned char)val[0] : 0; break;
      case 'n': str_to_ll(val, &count); break;
      case 'O': str_to_ll(val, &origin); keep_origin = 1; break;
      case 's': str_to_ll(val, &skip); break;
      case 't': trim = 1; break;
      case 'u': {
        long long k;
        if (str_to_ll(val, &k) != 0 || k < 0 || k >= MMC_FDS || sh_fd[k] < 0) {
          sh_error("mapfile: %s: invalid file descriptor specification", val);
          return 1;
        }
        fd = sh_fd[k];
        break;
      }
      case 'C': case 'c': break;
      default:
        sh_error("mapfile: -%c: invalid option", *a);
        return 2;
    }
  }
  if (i < argc) name = argv[i];
  if (!keep_origin) {
    if (var_make_array(name, 0) != 0) return 1;
  }
  buf_init(&line);
  for (;;) {
    unsigned char c;
    long r = os_read(fd, &c, 1);
    if (r == 1 && c != (unsigned char)delim) {
      buf_putc(&line, (char)c);
      continue;
    }
    if (r != 1 && line.len == 0) break;
    if (r == 1 && !trim) buf_putc(&line, (char)c);
    if (skip > 0) skip--;
    else {
      var_aset(name, origin + n, line.s ? line.s : "");
      n++;
      if (count > 0 && n >= count) {
        buf_free(&line);
        break;
      }
    }
    buf_free(&line);
    buf_init(&line);
    if (r != 1) break;
  }
  buf_free(&line);
  return 0;
}

/* }================================================================== */
