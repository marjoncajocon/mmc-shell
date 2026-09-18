/*
** mos.c - operating system layer
**
** Everything that differs between Windows and Linux/macOS lives here.
** All strings crossing this interface are UTF-8; all paths are native.
*/

#include "mmc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#ifdef _WIN32

/*
** {==================================================================
** Windows
** ===================================================================
*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING	0x0004
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#define ENABLE_VIRTUAL_TERMINAL_INPUT	0x0200
#endif

static HANDLE g_hin, g_hout;
static DWORD g_inmode, g_outmode;
static int g_have_in, g_have_out;
static UINT g_incp, g_outcp;


static wchar_t *widen (const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w;
  if (n <= 0) n = 1;
  w = (wchar_t *)xmalloc((size_t)n * sizeof(wchar_t));
  w[0] = L'\0';
  MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  return w;
}


static char *narrow (const wchar_t *w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
  char *s;
  if (n <= 0) n = 1;
  s = (char *)xmalloc((size_t)n);
  s[0] = '\0';
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
  return s;
}


static BOOL WINAPI ctrl_handler (DWORD type) {
  /* Ctrl-C goes to the running child; the shell itself survives */
  return (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT);
}


void os_init (void) {
  g_hin = GetStdHandle(STD_INPUT_HANDLE);
  g_hout = GetStdHandle(STD_OUTPUT_HANDLE);
  g_have_in = GetConsoleMode(g_hin, &g_inmode) != 0;
  g_have_out = GetConsoleMode(g_hout, &g_outmode) != 0;
  g_incp = GetConsoleCP();
  g_outcp = GetConsoleOutputCP();
  if (g_incp != 0) SetConsoleCP(CP_UTF8);
  if (g_outcp != 0) SetConsoleOutputCP(CP_UTF8);
  g_inmode |= ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT;
  g_inmode &= ~(DWORD)ENABLE_VIRTUAL_TERMINAL_INPUT;
  SetConsoleCtrlHandler(ctrl_handler, TRUE);
  os_tty_fix();
}


void os_shutdown (void) {
  if (g_have_in) SetConsoleMode(g_hin, g_inmode);
  if (g_have_out) SetConsoleMode(g_hout, g_outmode);
  if (g_incp != 0) SetConsoleCP(g_incp);
  if (g_outcp != 0) SetConsoleOutputCP(g_outcp);
}


/* programs may leave the console in any state; put it back */
void os_tty_fix (void) {
  if (g_have_in) SetConsoleMode(g_hin, g_inmode);
  if (g_have_out)
    SetConsoleMode(g_hout, g_outmode | ENABLE_PROCESSED_OUTPUT |
                           ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}


void os_args (int *argc, char ***argv) {
  int n = 0, i;
  wchar_t **w = CommandLineToArgvW(GetCommandLineW(), &n);
  char **v;
  if (w == NULL) return;
  v = (char **)xmalloc(((size_t)n + 1) * sizeof(char *));
  for (i = 0; i < n; i++) v[i] = narrow(w[i]);
  v[n] = NULL;
  LocalFree(w);
  *argc = n;
  *argv = v;
}


char *os_getenv (const char *name) {
  wchar_t *wn = widen(name);
  wchar_t *wv = NULL;
  char *r = NULL;
  DWORD n;
  SetLastError(0);
  n = GetEnvironmentVariableW(wn, NULL, 0);
  if (n == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) goto done;
  wv = (wchar_t *)xmalloc(((size_t)n + 1) * sizeof(wchar_t));
  wv[0] = L'\0';
  GetEnvironmentVariableW(wn, wv, n + 1);
  r = narrow(wv);
 done:
  free(wn);
  free(wv);
  return r;
}


void os_setenv (const char *name, const char *value) {
  wchar_t *wn = widen(name);
  wchar_t *wv = value ? widen(value) : NULL;
  SetEnvironmentVariableW(wn, wv);
  free(wn);
  free(wv);
}


void os_env_list (Vec *out) {
  wchar_t *block = GetEnvironmentStringsW();
  wchar_t *p;
  if (block == NULL) return;
  for (p = block; *p; p += wcslen(p) + 1)
    if (*p != L'=') vec_push(out, narrow(p));	/* skip "=C:=C:\dir" */
  FreeEnvironmentStringsW(block);
}


char *os_getcwd (void) {
  DWORD n = GetCurrentDirectoryW(0, NULL);
  wchar_t *w = (wchar_t *)xmalloc(((size_t)n + 1) * sizeof(wchar_t));
  char *r;
  w[0] = L'\0';
  GetCurrentDirectoryW(n + 1, w);
  r = narrow(w);
  free(w);
  return r;
}


int os_chdir (const char *native) {
  wchar_t *w = widen(native);
  int ok = SetCurrentDirectoryW(w) != 0;
  free(w);
  return ok ? 0 : -1;
}


int os_stat (const char *native, OsStat *st) {
  WIN32_FILE_ATTRIBUTE_DATA d;
  wchar_t *w = widen(native);
  int ok = GetFileAttributesExW(w, GetFileExInfoStandard, &d) != 0;
  free(w);
  memset(st, 0, sizeof(*st));
  if (!ok) return -1;
  st->exists = 1;
  st->is_dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  st->size = (long long)(((unsigned long long)d.nFileSizeHigh << 32) |
                         d.nFileSizeLow);
  {
    unsigned long long t =
      ((unsigned long long)d.ftLastWriteTime.dwHighDateTime << 32) |
      d.ftLastWriteTime.dwLowDateTime;
    st->mtime = (time_t)(t / 10000000ULL - 11644473600ULL);
  }
  return 0;
}


int os_is_exec (const char *native) {
  OsStat st;
  return os_stat(native, &st) == 0 && !st.is_dir;
}


int os_listdir (const char *native, Vec *out) {
  WIN32_FIND_DATAW fd;
  char *pat = path_join(native[0] ? native : ".", "*");
  wchar_t *w = widen(pat);
  HANDLE h = FindFirstFileW(w, &fd);
  free(pat);
  free(w);
  if (h == INVALID_HANDLE_VALUE) return -1;
  do {
    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
      continue;
    vec_push(out, narrow(fd.cFileName));
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return 0;
}


int os_mkdir (const char *native) {
  wchar_t *w = widen(native);
  int ok = CreateDirectoryW(w, NULL) != 0;
  free(w);
  return ok ? 0 : -1;
}


int os_open (const char *native, int mode) {
  int flags = _O_BINARY | _O_NOINHERIT;
  wchar_t *w = widen(native);
  int fd;
  if (mode == OS_READ) flags |= _O_RDONLY;
  else if (mode == OS_WRITE) flags |= _O_WRONLY | _O_CREAT | _O_TRUNC;
  else flags |= _O_WRONLY | _O_CREAT;
  fd = _wopen(w, flags, _S_IREAD | _S_IWRITE);
  free(w);
  /* we write with WriteFile, so do the "append" part ourselves */
  if (fd >= 0 && mode == OS_APPEND) _lseeki64(fd, 0, SEEK_END);
  return fd;
}


int os_pipe (int fds[2]) {
  return _pipe(fds, 65536, _O_BINARY | _O_NOINHERIT);
}


int os_dup (int fd) {
  HANDLE me = GetCurrentProcess();
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  HANDLE copy;
  int r;
  if (h == INVALID_HANDLE_VALUE || h == (HANDLE)(intptr_t)-2) return -1;
  if (!DuplicateHandle(me, h, me, &copy, 0, FALSE, DUPLICATE_SAME_ACCESS))
    return -1;
  r = _open_osfhandle((intptr_t)copy, _O_BINARY | _O_NOINHERIT);
  if (r < 0) CloseHandle(copy);
  return r;
}


void os_close (int fd) {
  _close(fd);
}


long os_read (int fd, void *buf, size_t n) {
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  DWORD got = 0;
  if (!ReadFile(h, buf, (DWORD)n, &got, NULL)) {
    DWORD e = GetLastError();
    return (e == ERROR_BROKEN_PIPE || e == ERROR_HANDLE_EOF) ? 0 : -1;
  }
  return (long)got;
}


long os_write (int fd, const void *buf, size_t n) {
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  const char *p = (const char *)buf;
  size_t left = n;
  while (left > 0) {
    DWORD put = 0;
    if (!WriteFile(h, p, (DWORD)left, &put, NULL) || put == 0) return -1;
    p += put;
    left -= put;
  }
  return (long)n;
}


int os_is_tty (int fd) {
  DWORD m;
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  return GetConsoleMode(h, &m) != 0;
}


int os_tty_raw (int on) {
  DWORD m;
  if (!g_have_in) return -1;
  if (!on) return SetConsoleMode(g_hin, g_inmode) ? 0 : -1;
  m = g_inmode & ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                          ENABLE_PROCESSED_INPUT | ENABLE_MOUSE_INPUT |
                          ENABLE_WINDOW_INPUT);
  m |= ENABLE_VIRTUAL_TERMINAL_INPUT;
  return SetConsoleMode(g_hin, m) ? 0 : -1;
}


/* next byte of UTF-8 typed at the console, -1 on end of input */
int os_tty_getbyte (void) {
  static unsigned char q[8];
  static int qn = 0, qi = 0;
  wchar_t wc[2];
  DWORD got = 0;
  int cnt = 1;
  if (qi < qn) return q[qi++];
  if (!ReadConsoleW(g_hin, &wc[0], 1, &got, NULL) || got == 0) return -1;
  if (wc[0] >= 0xD800 && wc[0] <= 0xDBFF) {	/* surrogate pair */
    if (ReadConsoleW(g_hin, &wc[1], 1, &got, NULL) && got == 1) cnt = 2;
  }
  qn = WideCharToMultiByte(CP_UTF8, 0, wc, cnt, (char *)q, (int)sizeof(q),
                           NULL, NULL);
  qi = 0;
  if (qn <= 0) {
    qn = 0;
    return '?';
  }
  if (q[0] == 26 && qn == 1) return -1;	/* Ctrl-Z in cooked mode */
  return q[qi++];
}


int os_term_cols (void) {
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (!GetConsoleScreenBufferInfo(g_hout, &info)) return 80;
  return info.srWindow.Right - info.srWindow.Left + 1;
}


char *os_exe_path (const char *argv0) {
  wchar_t w[32768 / 8];
  DWORD n = GetModuleFileNameW(NULL, w, (DWORD)(sizeof(w) / sizeof(w[0])));
  if (n == 0 || n >= sizeof(w) / sizeof(w[0])) return xstrdup(argv0);
  return narrow(w);
}


char *os_hostname (void) {
  char *s = os_getenv("COMPUTERNAME");
  return s ? s : xstrdup("localhost");
}


char *os_username (void) {
  char *s = os_getenv("USERNAME");
  return (s && s[0]) ? s : xstrdup("user");
}


long os_getpid (void) {
  return (long)GetCurrentProcessId();
}


static int has_ext (const char *path, const char *ext) {
  size_t n = strlen(path), e = strlen(ext);
  return n > e && m_stricmp(path + n - e, ext) == 0;
}


/* quoting rules of the Microsoft C runtime (and close enough for cmd) */
static void quote_arg (Buf *b, const char *a, int force, int batch) {
  const char *p;
  const char *special = batch ? " \t\"&|<>^()%!;,=" : " \t\"";
  if (!force && a[0] != '\0' && strpbrk(a, special) == NULL) {
    buf_puts(b, a);
    return;
  }
  buf_putc(b, '"');
  for (p = a;; p++) {
    size_t slashes = 0;
    while (*p == '\\') {
      slashes++;
      p++;
    }
    if (*p == '\0') {
      while (slashes-- > 0) buf_puts(b, "\\\\");
      break;
    }
    if (*p == '"') {
      while (slashes-- > 0) buf_puts(b, "\\\\");
      buf_puts(b, "\\\"");
    }
    else {
      while (slashes-- > 0) buf_putc(b, '\\');
      buf_putc(b, *p);
    }
  }
  buf_putc(b, '"');
}


/* environment for the child, with Linux style values made native */
static wchar_t *build_env_block (void) {
  wchar_t *block = GetEnvironmentStringsW();
  wchar_t *out = NULL, *p;
  size_t len = 0, cap = 0;
  if (block == NULL) return NULL;
  for (p = block; *p; p += wcslen(p) + 1) {
    const wchar_t *entry = p;
    wchar_t *made = NULL;
    const wchar_t *eq = wcschr(p + 1, L'=');
    size_t n;
    if (eq != NULL && eq[1] == L'/') {
      char *val = narrow(eq + 1);
      char *conv = path_env_to_native(val);
      if (strcmp(val, conv) != 0) {
        wchar_t *wconv = widen(conv);
        size_t head = (size_t)(eq + 1 - p);
        made = (wchar_t *)xmalloc((head + wcslen(wconv) + 1) * sizeof(wchar_t));
        memcpy(made, p, head * sizeof(wchar_t));
        wcscpy(made + head, wconv);
        free(wconv);
        entry = made;
      }
      free(val);
      free(conv);
    }
    n = wcslen(entry) + 1;
    if (len + n + 1 > cap) {
      cap = (len + n + 1) * 2;
      out = (wchar_t *)xrealloc(out, cap * sizeof(wchar_t));
    }
    memcpy(out + len, entry, n * sizeof(wchar_t));
    len += n;
    free(made);
  }
  FreeEnvironmentStringsW(block);
  if (out == NULL) {	/* empty environment: two terminators */
    out = (wchar_t *)xmalloc(2 * sizeof(wchar_t));
    out[len++] = L'\0';
  }
  out[len] = L'\0';
  return out;
}


static HANDLE inheritable (int fd) {
  HANDLE me = GetCurrentProcess();
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  HANDLE copy = NULL;
  if (h == INVALID_HANDLE_VALUE || h == (HANDLE)(intptr_t)-2) return NULL;
  if (!DuplicateHandle(me, h, me, &copy, 0, TRUE, DUPLICATE_SAME_ACCESS))
    return NULL;
  return copy;
}


int os_spawn (const char *exe, char **argv, int in, int out, int err,
              OsProc *proc, long *pid) {
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  Buf cl;
  char *app;
  wchar_t *wapp, *wcl, *wenv;
  int i, ok;
  int batch = has_ext(exe, ".cmd") || has_ext(exe, ".bat");
  buf_init(&cl);
  if (batch) {	/* batch files are run by the command interpreter */
    app = os_getenv("ComSpec");
    if (app == NULL) app = xstrdup("C:\\Windows\\System32\\cmd.exe");
    quote_arg(&cl, app, 0, 0);
    buf_puts(&cl, " /d /s /c \"");
    quote_arg(&cl, exe, 1, 1);
  }
  else {
    app = xstrdup(exe);
    quote_arg(&cl, exe, 0, 0);
  }
  for (i = 1; argv[i] != NULL; i++) {
    buf_putc(&cl, ' ');
    quote_arg(&cl, argv[i], 0, batch);
  }
  if (batch) buf_putc(&cl, '"');
  memset(&si, 0, sizeof(si));
  memset(&pi, 0, sizeof(pi));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = inheritable(in);
  si.hStdOutput = inheritable(out);
  si.hStdError = inheritable(err);
  wapp = widen(app);
  wcl = widen(cl.s);
  wenv = build_env_block();
  ok = CreateProcessW(wapp, wcl, NULL, NULL, TRUE, CREATE_UNICODE_ENVIRONMENT,
                      wenv, NULL, &si, &pi) != 0;
  if (!ok)
    fd_printf(2, "mmc: %s: cannot execute (Windows error %lu)\n",
              argv[0], (unsigned long)GetLastError());
  if (si.hStdInput) CloseHandle(si.hStdInput);
  if (si.hStdOutput) CloseHandle(si.hStdOutput);
  if (si.hStdError) CloseHandle(si.hStdError);
  free(app);
  free(wapp);
  free(wcl);
  free(wenv);
  buf_free(&cl);
  if (!ok) return -1;
  CloseHandle(pi.hThread);
  *proc = (OsProc)pi.hProcess;
  *pid = (long)pi.dwProcessId;
  return 0;
}


int os_wait (OsProc proc) {
  HANDLE h = (HANDLE)proc;
  DWORD code = 1;
  WaitForSingleObject(h, INFINITE);
  GetExitCodeProcess(h, &code);
  CloseHandle(h);
  return (int)code;
}


void os_detach (OsProc proc) {
  CloseHandle((HANDLE)proc);
}


void os_reap (void) {
}

/* }================================================================== */

#else

/*
** {==================================================================
** Linux, macOS
** ===================================================================
*/

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char **environ;

static struct termios g_termios;
static int g_have_termios = 0;


void os_init (void) {
  signal(SIGINT, SIG_IGN);	/* Ctrl-C is for the running child */
  signal(SIGQUIT, SIG_IGN);
  signal(SIGTSTP, SIG_IGN);
  signal(SIGPIPE, SIG_IGN);
  g_have_termios = (tcgetattr(0, &g_termios) == 0);
}


void os_shutdown (void) {
  os_tty_fix();
}


void os_tty_fix (void) {
  if (g_have_termios) tcsetattr(0, TCSADRAIN, &g_termios);
}


void os_args (int *argc, char ***argv) {
  (void)argc;
  (void)argv;
}


char *os_getenv (const char *name) {
  const char *v = getenv(name);
  return v ? xstrdup(v) : NULL;
}


void os_setenv (const char *name, const char *value) {
  if (value) setenv(name, value, 1);
  else unsetenv(name);
}


void os_env_list (Vec *out) {
  char **e;
  for (e = environ; e && *e; e++) vec_push(out, xstrdup(*e));
}


char *os_getcwd (void) {
  size_t cap = 256;
  for (;;) {
    char *b = (char *)xmalloc(cap);
    if (getcwd(b, cap) != NULL) return b;
    free(b);
    if (errno != ERANGE) return xstrdup(".");
    cap *= 2;
  }
}


int os_chdir (const char *native) {
  return chdir(native);
}


int os_stat (const char *native, OsStat *st) {
  struct stat s;
  memset(st, 0, sizeof(*st));
  if (stat(native, &s) != 0) return -1;
  st->exists = 1;
  st->is_dir = S_ISDIR(s.st_mode);
  st->size = (long long)s.st_size;
  st->mtime = s.st_mtime;
  return 0;
}


int os_is_exec (const char *native) {
  OsStat st;
  return os_stat(native, &st) == 0 && !st.is_dir && access(native, X_OK) == 0;
}


int os_listdir (const char *native, Vec *out) {
  DIR *d = opendir(native[0] ? native : ".");
  struct dirent *e;
  if (d == NULL) return -1;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    vec_push(out, xstrdup(e->d_name));
  }
  closedir(d);
  return 0;
}


int os_mkdir (const char *native) {
  return mkdir(native, 0777);
}


int os_open (const char *native, int mode) {
  int flags = O_CLOEXEC;
  if (mode == OS_READ) flags |= O_RDONLY;
  else if (mode == OS_WRITE) flags |= O_WRONLY | O_CREAT | O_TRUNC;
  else flags |= O_WRONLY | O_CREAT | O_APPEND;
  return open(native, flags, 0666);
}


int os_pipe (int fds[2]) {
  if (pipe(fds) != 0) return -1;
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  return 0;
}


int os_dup (int fd) {
  int r = dup(fd);
  if (r >= 0) fcntl(r, F_SETFD, FD_CLOEXEC);
  return r;
}


void os_close (int fd) {
  close(fd);
}


long os_read (int fd, void *buf, size_t n) {
  for (;;) {
    ssize_t r = read(fd, buf, n);
    if (r < 0 && errno == EINTR) continue;
    return (long)r;
  }
}


long os_write (int fd, const void *buf, size_t n) {
  const char *p = (const char *)buf;
  size_t left = n;
  while (left > 0) {
    ssize_t r = write(fd, p, left);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return -1;
    p += r;
    left -= (size_t)r;
  }
  return (long)n;
}


int os_is_tty (int fd) {
  return isatty(fd);
}


int os_tty_raw (int on) {
  struct termios t;
  if (!g_have_termios) return -1;
  if (!on) return tcsetattr(0, TCSADRAIN, &g_termios);
  t = g_termios;
  t.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  t.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
  t.c_cc[VMIN] = 1;
  t.c_cc[VTIME] = 0;
  return tcsetattr(0, TCSADRAIN, &t);
}


int os_tty_getbyte (void) {
  unsigned char c;
  return (os_read(0, &c, 1) == 1) ? c : -1;
}


int os_term_cols (void) {
#ifdef TIOCGWINSZ
  struct winsize ws;
  if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
#endif
  return 80;
}


char *os_exe_path (const char *argv0) {
  char buf[4096];
  char *r;
#if defined(__APPLE__)
  uint32_t size = (uint32_t)sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) == 0 && (r = realpath(buf, NULL)) != NULL)
    return r;
#else
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    return xstrdup(buf);
  }
#endif
  if (strchr(argv0, '/') != NULL && (r = realpath(argv0, NULL)) != NULL)
    return r;
  {	/* started by name: look it up in PATH */
    Vec dirs;
    size_t i;
    char *path = os_getenv("PATH");
    vec_init(&dirs);
    if (path) path_list_split(path, &dirs);
    free(path);
    for (i = 0; i < dirs.n; i++) {
      char *cand = path_join(dirs.v[i], argv0);
      if (os_is_exec(cand) && (r = realpath(cand, NULL)) != NULL) {
        free(cand);
        vec_free(&dirs);
        return r;
      }
      free(cand);
    }
    vec_free(&dirs);
  }
  return xstrdup(argv0);
}


char *os_hostname (void) {
  char buf[256];
  if (gethostname(buf, sizeof(buf)) != 0) return xstrdup("localhost");
  buf[sizeof(buf) - 1] = '\0';
  buf[strcspn(buf, ".")] = '\0';
  return xstrdup(buf);
}


char *os_username (void) {
  char *s = os_getenv("USER");
  if (s == NULL || s[0] == '\0') {
    free(s);
    s = os_getenv("LOGNAME");
  }
  return (s && s[0]) ? s : xstrdup("user");
}


long os_getpid (void) {
  return (long)getpid();
}


int os_spawn (const char *exe, char **argv, int in, int out, int err,
              OsProc *proc, long *pid) {
  pid_t p = fork();
  if (p < 0) {
    fd_printf(2, "mmc: %s: cannot fork: %s\n", argv[0], strerror(errno));
    return -1;
  }
  if (p == 0) {	/* child */
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    if (err == 1 && out != 1) err = dup(1);	/* "2>&1 >file" */
    if (in != 0) dup2(in, 0);
    if (out != 1) dup2(out, 1);
    if (err != 2) dup2(err, 2);
    execv(exe, argv);
    fd_printf(2, "mmc: %s: cannot execute: %s\n", argv[0], strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);
  }
  *proc = (OsProc)p;
  *pid = (long)p;
  return 0;
}


int os_wait (OsProc proc) {
  int st = 0;
  for (;;) {
    pid_t r = waitpid((pid_t)proc, &st, WUNTRACED);
    if (r < 0 && errno == EINTR) continue;
    if (r < 0) return 1;
    if (WIFSTOPPED(st)) {	/* no job control: Ctrl-Z must not hang us */
      kill((pid_t)proc, SIGCONT);
      continue;
    }
    break;
  }
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  if (WIFSIGNALED(st)) {
    if (WTERMSIG(st) == SIGINT) os_write(2, "\n", 1);
    return 128 + WTERMSIG(st);
  }
  return 1;
}


void os_detach (OsProc proc) {
  (void)proc;
}


/* collects finished background processes */
void os_reap (void) {
  int st;
  while (waitpid(-1, &st, WNOHANG) > 0)
    ;
}

/* }================================================================== */

#endif
