/*
** mos.c - operating system layer
**
** Everything that differs between Windows and Linux/macOS/Android lives
** here. All strings crossing this interface are UTF-8; all paths are
** native.
*/

#include "mmc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile int os_interrupted = 0;
volatile int os_pending[65];
int os_pipe_exit = 0;


#ifdef _WIN32

/*
** {==================================================================
** Windows
** ===================================================================
*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
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
static int g_umask = 022;
static int g_int_mode = 0;	/* trap on INT: 0 default, 1 trap, 2 ignore */


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
  /* Ctrl-C goes to the running child too; the shell survives and unwinds */
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
    if (g_int_mode != 2) {
      os_interrupted = (g_int_mode == 0);
      os_pending[2] = 1;
    }
    return TRUE;
  }
  return FALSE;
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


static int has_ext (const char *path, const char *ext) {
  size_t n = strlen(path), e = strlen(ext);
  return n > e && m_stricmp(path + n - e, ext) == 0;
}


static int stat_common (const char *native, OsStat *st) {
  WIN32_FILE_ATTRIBUTE_DATA d;
  wchar_t *w = widen(native);
  int ok = GetFileAttributesExW(w, GetFileExInfoStandard, &d) != 0;
  unsigned long long t;
  free(w);
  memset(st, 0, sizeof(*st));
  if (!ok) {
    if (m_stricmp(native, "NUL") == 0) {	/* /dev/null */
      st->exists = 1;
      st->is_chr = 1;
      st->mode = 0666;
      return 0;
    }
    return -1;
  }
  st->exists = 1;
  st->is_dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  st->is_link = (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
  st->size = (long long)(((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow);
  t = ((unsigned long long)d.ftLastWriteTime.dwHighDateTime << 32) |
      d.ftLastWriteTime.dwLowDateTime;
  st->mtime = (time_t)(t / 10000000ULL - 11644473600ULL);
  t = ((unsigned long long)d.ftLastAccessTime.dwHighDateTime << 32) |
      d.ftLastAccessTime.dwLowDateTime;
  st->atime = (time_t)(t / 10000000ULL - 11644473600ULL);
  st->mode = (d.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 0444 : 0644;
  if (st->is_dir || has_ext(native, ".exe") || has_ext(native, ".com") ||
      has_ext(native, ".bat") || has_ext(native, ".cmd") || has_ext(native, ".sh"))
    st->mode |= 0111;
  st->uid = 1000;
  st->gid = 1000;
  return 0;
}


int os_stat (const char *native, OsStat *st) {
  int r = stat_common(native, st);
  st->is_link = 0;
  return r;
}


int os_lstat (const char *native, OsStat *st) {
  return stat_common(native, st);
}


int os_is_exec (const char *native) {
  OsStat st;
  return os_stat(native, &st) == 0 && !st.is_dir;
}


/* a "#!" line makes a file executable, as in git-bash */
static int has_shebang (const char *native) {
  char head[2];
  int fd = os_open(native, OS_READ), ok = 0;
  if (fd < 0) return 0;
  ok = os_read(fd, head, 2) == 2 && head[0] == '#' && head[1] == '!';
  os_close(fd);
  return ok;
}


int os_access (const char *native, int what) {
  OsStat st;
  if (os_stat(native, &st) != 0) return 0;
  if (what == 'r') return 1;
  if (what == 'w') return (st.mode & 0200) != 0 || st.is_dir;
  if (st.is_dir || (st.mode & 0111)) return 1;
  return has_shebang(native);
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


int os_unlink (const char *native) {
  wchar_t *w = widen(native);
  int ok = DeleteFileW(w) != 0;
  free(w);
  return ok ? 0 : -1;
}


char *os_realpath (const char *native) {
  wchar_t *w = widen(native), buf[4096];
  DWORD n = GetFullPathNameW(w, 4096, buf, NULL);
  free(w);
  if (n == 0 || n >= 4096) return NULL;
  return narrow(buf);
}


int os_open (const char *native, int mode) {
  int flags = _O_BINARY | _O_NOINHERIT;
  wchar_t *w = widen(native);
  int fd;
  if (mode == OS_READ) flags |= _O_RDONLY;
  else if (mode == OS_WRITE) flags |= _O_WRONLY | _O_CREAT | _O_TRUNC;
  else if (mode == OS_RDWR) flags |= _O_RDWR | _O_CREAT;
  else if (mode == OS_EXCL) flags |= _O_WRONLY | _O_CREAT | _O_EXCL;
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
  if (fd >= 0) _close(fd);
}


int os_fd_valid (int fd) {
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  return h != INVALID_HANDLE_VALUE && h != (HANDLE)(intptr_t)-2;
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
  if (h == INVALID_HANDLE_VALUE) return -1;
  while (left > 0) {
    DWORD put = 0;
    if (!WriteFile(h, p, (DWORD)left, &put, NULL) || put == 0) {
      DWORD e = GetLastError();
      /* nobody reads the pipe any more: what SIGPIPE does elsewhere */
      if (os_pipe_exit && (e == ERROR_NO_DATA || e == ERROR_BROKEN_PIPE)) exit(128 + 13);
      return -1;
    }
    p += put;
    left -= put;
  }
  return (long)n;
}


int os_wait_readable (int fd, int ms) {
  HANDLE h;
  DWORD type;
  DWORD start = GetTickCount();
  if (fd < 0) {
    Sleep((DWORD)(ms > 0 ? ms : 0));
    return 0;
  }
  h = (HANDLE)_get_osfhandle(fd);
  type = GetFileType(h);
  if (type == FILE_TYPE_CHAR) {
    DWORD m;
    if (GetConsoleMode(h, &m)) return WaitForSingleObject(h, (DWORD)(ms < 0 ? INFINITE : ms)) == WAIT_OBJECT_0;
    return 1;
  }
  if (type != FILE_TYPE_PIPE) return 1;	/* files are always ready */
  for (;;) {
    DWORD avail = 0;
    if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) return 1;	/* EOF: ready */
    if (avail > 0) return 1;
    if (ms >= 0 && GetTickCount() - start >= (DWORD)ms) return 0;
    Sleep(10);
  }
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


int os_term_rows (void) {
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (!GetConsoleScreenBufferInfo(g_hout, &info)) return 24;
  return info.srWindow.Bottom - info.srWindow.Top + 1;
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


long os_getppid (void) {
  static long cached = -1;
  HANDLE snap;
  PROCESSENTRY32 pe;
  DWORD me = GetCurrentProcessId();
  if (cached >= 0) return cached;
  cached = 0;
  snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  pe.dwSize = sizeof(pe);
  if (Process32First(snap, &pe)) {
    do {
      if (pe.th32ProcessID == me) {
        cached = (long)pe.th32ParentProcessID;
        break;
      }
    } while (Process32Next(snap, &pe));
  }
  CloseHandle(snap);
  return cached;
}


long os_getuid (void) { return 1000; }
long os_geteuid (void) { return 1000; }


long long os_now_us (void) {
  FILETIME ft;
  unsigned long long t;
  GetSystemTimeAsFileTime(&ft);
  t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  return (long long)(t / 10ULL) - 11644473600000000LL;
}


static double ft_seconds (FILETIME ft) {
  unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  return (double)t / 1e7;
}


void os_times (double t[4]) {
  FILETIME c, e, k, u;
  t[0] = t[1] = t[2] = t[3] = 0.0;
  if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
    t[0] = ft_seconds(u);
    t[1] = ft_seconds(k);
  }
}


int os_umask (int mask) {
  int old = g_umask;
  if (mask >= 0) g_umask = mask & 0777;
  return old;
}


const char *os_type (void) { return "msys"; }


const char *os_machine (void) {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "aarch64";
#else
  return "x86_64";
#endif
}


/* quoting rules of the Microsoft C runtime (and close enough for cmd) */
static void quote_arg (Buf *b, const char *a, int force, int batch) {
  const char *p;
  /* PortableGit's programs expand * ? [ { and ' in an unquoted argument
  ** themselves: those are quoted too, so they arrive as they are */
  const char *special = batch ? " \t\"&|<>^()%!;,=" : " \t\"*?[{'";
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
static wchar_t *build_env_block (char **envp) {
  Vec env;
  size_t i, len = 0, cap = 0;
  wchar_t *out = NULL;
  vec_init(&env);
  if (envp == NULL) os_env_list(&env);
  else for (i = 0; envp[i] != NULL; i++) vec_push(&env, xstrdup(envp[i]));
  {	/* Windows wants the block sorted, case-insensitively */
    size_t a, b;
    for (a = 1; a < env.n; a++)
      for (b = a; b > 0 && m_stricmp(env.v[b - 1], env.v[b]) > 0; b--) {
        char *t = env.v[b];
        env.v[b] = env.v[b - 1];
        env.v[b - 1] = t;
      }
  }
  for (i = 0; i < env.n; i++) {
    char *entry = env.v[i], *made = NULL;
    char *eq = strchr(entry + 1, '=');
    wchar_t *w;
    size_t n;
    if (eq != NULL && eq[1] == '/') {
      char *conv = path_env_to_native(eq + 1);
      if (strcmp(conv, eq + 1) != 0) {
        *eq = '\0';
        made = xstrcat3(entry, "=", conv);
        *eq = '=';
        entry = made;
      }
      free(conv);
    }
    w = widen(entry);
    n = wcslen(w) + 1;
    if (len + n + 1 > cap) {
      cap = (len + n + 1) * 2;
      out = (wchar_t *)xrealloc(out, cap * sizeof(wchar_t));
    }
    memcpy(out + len, w, n * sizeof(wchar_t));
    len += n;
    free(w);
    free(made);
  }
  vec_free(&env);
  if (out == NULL) {	/* empty environment: two terminators */
    out = (wchar_t *)xmalloc(2 * sizeof(wchar_t));
    out[len++] = L'\0';
  }
  out[len] = L'\0';
  return out;
}


static HANDLE inheritable (int fd) {
  HANDLE me = GetCurrentProcess();
  HANDLE h, copy = NULL;
  if (fd < 0) return NULL;
  h = (HANDLE)_get_osfhandle(fd);
  if (h == INVALID_HANDLE_VALUE || h == (HANDLE)(intptr_t)-2) return NULL;
  if (!DuplicateHandle(me, h, me, &copy, 0, TRUE, DUPLICATE_SAME_ACCESS))
    return NULL;
  return copy;
}


int os_spawn (const char *exe, char **argv, char **envp, const int *fds,
              int nfds, OsProc *proc, long *pid) {
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
  si.hStdInput = inheritable(nfds > 0 ? fds[0] : 0);
  si.hStdOutput = inheritable(nfds > 1 ? fds[1] : 1);
  si.hStdError = inheritable(nfds > 2 ? fds[2] : 2);
  wapp = widen(app);
  wcl = widen(cl.s);
  wenv = build_env_block(envp);
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


int os_poll_proc (OsProc proc, int *status) {
  HANDLE h = (HANDLE)proc;
  DWORD code = 1;
  if (WaitForSingleObject(h, 0) != WAIT_OBJECT_0) return 0;
  GetExitCodeProcess(h, &code);
  CloseHandle(h);
  *status = (int)code;
  return 1;
}


void os_detach (OsProc proc) {
  CloseHandle((HANDLE)proc);
}


static int proc_pause (long pid, int stop);


int os_kill (long pid, int sig) {
  HANDLE h;
  int ok;
  if (sig == 19 || sig == 20 || sig == 21 || sig == 22) return proc_pause(pid, 1);	/* STOP TSTP TTIN TTOU */
  if (sig == 18) return proc_pause(pid, 0);	/* CONT */
  if (sig == 17 || sig == 23 || sig == 28) return 0;	/* CHLD URG WINCH: nothing to do */
  h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
  if (h == NULL) return -1;
  if (sig == 0) {	/* only asks whether it exists */
    DWORD code = 0;
    ok = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return ok ? 0 : -1;
  }
  ok = TerminateProcess(h, (UINT)(128 + sig)) != 0;
  CloseHandle(h);
  return ok ? 0 : -1;
}


int os_exec (const char *exe, char **argv, char **envp) {
  (void)exe; (void)argv; (void)envp;
  return -1;
}

/*
** {==================================================================
** Job control. A console has no process groups and no Ctrl-Z for us
** (the key goes to the program as input), but a process can be stopped
** and let go on: kill -STOP / -CONT, fg and bg use that.
** ===================================================================
*/

typedef LONG (NTAPI *NtProcFn) (HANDLE);


/* stop (1) or let go on (0) every thread of a process */
static int proc_pause (long pid, int stop) {
  static NtProcFn suspend = NULL, resume = NULL;
  HANDLE h;
  LONG r;
  if (suspend == NULL) {
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    if (nt == NULL) return -1;
    suspend = (NtProcFn)(void (*)(void))GetProcAddress(nt, "NtSuspendProcess");
    resume = (NtProcFn)(void (*)(void))GetProcAddress(nt, "NtResumeProcess");
    if (suspend == NULL || resume == NULL) return -1;
  }
  h = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, (DWORD)pid);
  if (h == NULL) return -1;
  r = stop ? suspend(h) : resume(h);
  CloseHandle(h);
  return r >= 0 ? 0 : -1;
}


int os_job_control (int interactive) {
  (void)interactive;
  return 0;
}

int os_job_active (void) {
  return 0;
}

void os_job_pgid (long pgid) {
  (void)pgid;
}

void os_tty_give (long pgid) {
  (void)pgid;
}

int os_wait_fg (OsProc proc, int *stopped) {
  *stopped = 0;
  return os_wait(proc);
}

int os_suspend_self (void) {
  return -1;
}

/* }================================================================== */



int os_can_exec_replace (void) {
  return 0;
}


void os_catch_signal (int sig, int on) {
  if (sig == 2) g_int_mode = on;
}


typedef struct Tramp {
  OsThreadFn fn;
  void *arg;
} Tramp;

struct OsThread {
  HANDLE h;
};


static DWORD WINAPI thread_main (LPVOID p) {
  Tramp t = *(Tramp *)p;
  free(p);
  t.fn(t.arg);
  return 0;
}


OsThread *os_thread_start (OsThreadFn fn, void *arg) {
  Tramp *t = (Tramp *)xmalloc(sizeof(Tramp));
  OsThread *th = (OsThread *)xmalloc(sizeof(OsThread));
  t->fn = fn;
  t->arg = arg;
  th->h = CreateThread(NULL, 0, thread_main, t, 0, NULL);
  if (th->h == NULL) {
    free(t);
    free(th);
    return NULL;
  }
  return th;
}


void os_thread_join (OsThread *t) {
  if (t == NULL) return;
  WaitForSingleObject(t->h, INFINITE);
  CloseHandle(t->h);
  free(t);
}


#else

/*
** {==================================================================
** Linux, macOS, Android
** ===================================================================
*/

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* job control, see below */
static int g_jobctl;
static pid_t g_orig_pgrp, g_shell_pgrp;
static long g_spawn_pgid;

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char **environ;

static struct termios g_termios;
static int g_have_termios = 0;


static void on_signal (int sig) {
  if (sig > 0 && sig < 65) os_pending[sig] = 1;
  if (sig == SIGINT) os_interrupted = 1;
}


static void set_handler (int sig, void (*fn) (int)) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = fn;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;	/* no SA_RESTART: a read waiting on a pipe returns */
  sigaction(sig, &sa, NULL);
}


void os_init (void) {
  set_handler(SIGINT, on_signal);	/* Ctrl-C: the child dies, we unwind */
  signal(SIGQUIT, SIG_IGN);
  signal(SIGTSTP, SIG_IGN);
  signal(SIGTTOU, SIG_IGN);
  signal(SIGPIPE, SIG_IGN);
  g_have_termios = (tcgetattr(0, &g_termios) == 0);
}


void os_shutdown (void) {
  os_tty_fix();
  if (g_jobctl && g_orig_pgrp > 0) tcsetpgrp(0, g_orig_pgrp);	/* the terminal goes back */
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


static void fill_stat (OsStat *st, const struct stat *s) {
  st->exists = 1;
  st->is_dir = S_ISDIR(s->st_mode);
  st->is_link = S_ISLNK(s->st_mode);
  st->is_fifo = S_ISFIFO(s->st_mode);
  st->is_sock = S_ISSOCK(s->st_mode);
  st->is_chr = S_ISCHR(s->st_mode);
  st->is_blk = S_ISBLK(s->st_mode);
  st->mode = (unsigned)(s->st_mode & 07777);
  st->size = (long long)s->st_size;
  st->mtime = s->st_mtime;
  st->atime = s->st_atime;
  st->dev = (unsigned long long)s->st_dev;
  st->ino = (unsigned long long)s->st_ino;
  st->uid = (long)s->st_uid;
  st->gid = (long)s->st_gid;
}


int os_stat (const char *native, OsStat *st) {
  struct stat s;
  memset(st, 0, sizeof(*st));
  if (stat(native, &s) != 0) return -1;
  fill_stat(st, &s);
  return 0;
}


int os_lstat (const char *native, OsStat *st) {
  struct stat s;
  memset(st, 0, sizeof(*st));
  if (lstat(native, &s) != 0) return -1;
  fill_stat(st, &s);
  return 0;
}


int os_is_exec (const char *native) {
  OsStat st;
  return os_stat(native, &st) == 0 && !st.is_dir && access(native, X_OK) == 0;
}


int os_access (const char *native, int what) {
  return access(native, what == 'r' ? R_OK : what == 'w' ? W_OK : X_OK) == 0;
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


int os_unlink (const char *native) {
  return unlink(native);
}


char *os_realpath (const char *native) {
  return realpath(native, NULL);
}


int os_open (const char *native, int mode) {
  int flags = O_CLOEXEC;
  if (mode == OS_READ) flags |= O_RDONLY;
  else if (mode == OS_WRITE) flags |= O_WRONLY | O_CREAT | O_TRUNC;
  else if (mode == OS_RDWR) flags |= O_RDWR | O_CREAT;
  else if (mode == OS_EXCL) flags |= O_WRONLY | O_CREAT | O_EXCL;
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
  int r = fcntl(fd, F_DUPFD_CLOEXEC, 3);
  return r;
}


void os_close (int fd) {
  if (fd >= 0) close(fd);
}


int os_fd_valid (int fd) {
  return fcntl(fd, F_GETFD) != -1;
}


long os_read (int fd, void *buf, size_t n) {
  for (;;) {
    ssize_t r = read(fd, buf, n);
    if (r < 0 && errno == EINTR) {
      if (os_interrupted) return -1;
      continue;
    }
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


int os_wait_readable (int fd, int ms) {
  struct pollfd p;
  int r;
  if (fd < 0) {
    if (ms > 0) usleep((useconds_t)ms * 1000);
    return 0;
  }
  p.fd = fd;
  p.events = POLLIN;
  p.revents = 0;
  do r = poll(&p, 1, ms); while (r < 0 && errno == EINTR && !os_interrupted);
  return r > 0 ? 1 : (r == 0 ? 0 : -1);
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


int os_term_rows (void) {
#ifdef TIOCGWINSZ
  struct winsize ws;
  if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) return ws.ws_row;
#endif
  return 24;
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


long os_getpid (void) { return (long)getpid(); }
long os_getppid (void) { return (long)getppid(); }
long os_getuid (void) { return (long)getuid(); }
long os_geteuid (void) { return (long)geteuid(); }


long long os_now_us (void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000000LL + tv.tv_usec;
}


void os_times (double t[4]) {
  struct tms tm;
  double hz = (double)sysconf(_SC_CLK_TCK);
  if (hz <= 0) hz = 100;
  times(&tm);
  t[0] = (double)tm.tms_utime / hz;
  t[1] = (double)tm.tms_stime / hz;
  t[2] = (double)tm.tms_cutime / hz;
  t[3] = (double)tm.tms_cstime / hz;
}


int os_umask (int mask) {
  mode_t old;
  if (mask < 0) {
    old = umask(022);
    umask(old);
    return (int)old;
  }
  return (int)umask((mode_t)mask);
}


const char *os_type (void) {
#if defined(__ANDROID__)
  return "linux-android";
#elif defined(__APPLE__)
  return "darwin";
#elif defined(__linux__)
  return "linux-gnu";
#else
  return "unix";
#endif
}


const char *os_machine (void) {
#if defined(__aarch64__)
  return "aarch64";
#elif defined(__arm__)
  return "arm";
#elif defined(__x86_64__)
  return "x86_64";
#elif defined(__i386__)
  return "i686";
#else
  return "unknown";
#endif
}


int os_spawn (const char *exe, char **argv, char **envp, const int *fds,
              int nfds, OsProc *proc, long *pid) {
  pid_t p = fork();
  if (p < 0) {
    fd_printf(2, "mmc: %s: cannot fork: %s\n", argv[0], strerror(errno));
    return -1;
  }
  if (p == 0) {	/* child */
    int tmp[MMC_FDS], k;
    if (g_spawn_pgid >= 0) setpgid(0, (pid_t)g_spawn_pgid);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    if (nfds > MMC_FDS) nfds = MMC_FDS;
    /* move everything out of the way first, then into place */
    for (k = 0; k < nfds; k++) tmp[k] = (fds[k] >= 0) ? fcntl(fds[k], F_DUPFD, 100) : -1;
    for (k = 0; k < nfds; k++) {
      if (tmp[k] >= 0) dup2(tmp[k], k);
      else close(k);
    }
    for (k = 0; k < nfds; k++)
      if (tmp[k] >= 0) close(tmp[k]);
    execve(exe, argv, envp ? envp : environ);
    fd_printf(2, "mmc: %s: cannot execute: %s\n", argv[0], strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);
  }
  if (g_spawn_pgid >= 0) setpgid(p, g_spawn_pgid > 0 ? (pid_t)g_spawn_pgid : p);
  *proc = (OsProc)p;
  *pid = (long)p;
  return 0;
}


static int decode (int st) {
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  if (WIFSIGNALED(st)) {
    if (WTERMSIG(st) == SIGINT) os_write(2, "\n", 1);
    return 128 + WTERMSIG(st);
  }
  return 1;
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
  return decode(st);
}


int os_poll_proc (OsProc proc, int *status) {
  int st = 0;
  pid_t r = waitpid((pid_t)proc, &st, WNOHANG | WUNTRACED);
  if (r <= 0) return r < 0 ? (*status = 127, 1) : 0;
  if (WIFSTOPPED(st)) {	/* a background job wanted the terminal, or kill -STOP */
    *status = 128 + WSTOPSIG(st);
    return 2;
  }
  *status = decode(st);
  return 1;
}


void os_detach (OsProc proc) {
  (void)proc;
}


/* the shell speaks Linux signal numbers (kill -l); macOS has others */
static int native_sig (int sig) {
  switch (sig) {
    case 7: return SIGBUS;
    case 10: return SIGUSR1;
    case 12: return SIGUSR2;
    case 17: return SIGCHLD;
    case 18: return SIGCONT;
    case 19: return SIGSTOP;
    case 20: return SIGTSTP;
    case 21: return SIGTTIN;
    case 22: return SIGTTOU;
    case 23: return SIGURG;
    case 28: return SIGWINCH;
    default: return sig;
  }
}


int os_kill (long pid, int sig) {
  return kill((pid_t)pid, native_sig(sig));
}

/*
** {==================================================================
** Job control: an interactive shell on a terminal has a process group
** of its own; every job gets one too, and the terminal while it runs in
** the foreground. Ctrl-Z stops it; fg and bg let it go on.
** ===================================================================
*/

static int g_jobctl = 0;
static pid_t g_orig_pgrp = -1, g_shell_pgrp = -1;
static long g_spawn_pgid = -1;	/* -1: children stay in our group */


int os_job_control (int interactive) {
  int tries;
  if (!interactive) {	/* a script stops on Ctrl-Z like any other program */
    signal(SIGTSTP, SIG_DFL);
    return 0;
  }
  if (!isatty(0)) return 0;
  for (tries = 0; tries < 20 && tcgetpgrp(0) != getpgrp(); tries++)
    kill(-getpgrp(), SIGTTIN);	/* started in the background: wait to be in front */
  if (tcgetpgrp(0) != getpgrp()) return 0;
  signal(SIGTTIN, SIG_IGN);
  g_orig_pgrp = getpgrp();
  if (getsid(0) != getpid()) setpgid(0, 0);	/* a group of our own */
  g_shell_pgrp = getpgrp();
  if (tcsetpgrp(0, g_shell_pgrp) != 0) return 0;
  g_jobctl = 1;
  return 1;
}


int os_job_active (void) {
  return g_jobctl;
}


void os_job_pgid (long pgid) {
  g_spawn_pgid = g_jobctl ? pgid : -1;
}


void os_tty_give (long pgid) {
  if (g_jobctl) tcsetpgrp(0, pgid > 0 ? (pid_t)pgid : g_shell_pgrp);
}


int os_wait_fg (OsProc proc, int *stopped) {
  int st = 0;
  *stopped = 0;
  for (;;) {
    pid_t r = waitpid((pid_t)proc, &st, WUNTRACED);
    if (r < 0 && errno == EINTR) continue;
    if (r < 0) return 1;
    if (WIFSTOPPED(st)) {
      if (g_jobctl) {	/* Ctrl-Z: it becomes a stopped job */
        *stopped = 1;
        return 128 + WSTOPSIG(st);
      }
      kill((pid_t)proc, SIGCONT);	/* no job control: it must not hang us */
      continue;
    }
    break;
  }
  return decode(st);
}


int os_suspend_self (void) {
  kill(getpid(), SIGSTOP);	/* whoever started us gets the terminal back */
  if (g_jobctl) tcsetpgrp(0, g_shell_pgrp);	/* and we take it again */
  return 0;
}

/* }================================================================== */



int os_exec (const char *exe, char **argv, char **envp) {
  os_tty_fix();
  signal(SIGINT, SIG_DFL);
  signal(SIGQUIT, SIG_DFL);
  signal(SIGTSTP, SIG_DFL);
  signal(SIGPIPE, SIG_DFL);
  execve(exe, argv, envp ? envp : environ);
  return -1;
}


int os_can_exec_replace (void) {
  return 1;
}


void os_catch_signal (int sig, int on) {
  if (sig <= 0 || sig >= 65 || sig == SIGKILL || sig == SIGSTOP) return;
  if (on == 1) set_handler(sig, on_signal);
  else if (on == 2) signal(sig, SIG_IGN);
  else if (sig == SIGINT) set_handler(sig, on_signal);	/* the shell keeps its own */
  else if (sig == SIGQUIT || sig == SIGTSTP || sig == SIGTTOU || sig == SIGPIPE)
    signal(sig, SIG_IGN);
  else signal(sig, SIG_DFL);
}


typedef struct Tramp {
  OsThreadFn fn;
  void *arg;
} Tramp;

struct OsThread {
  pthread_t t;
};


static void *thread_main (void *p) {
  Tramp t = *(Tramp *)p;
  free(p);
  t.fn(t.arg);
  return NULL;
}


OsThread *os_thread_start (OsThreadFn fn, void *arg) {
  Tramp *t = (Tramp *)xmalloc(sizeof(Tramp));
  OsThread *th = (OsThread *)xmalloc(sizeof(OsThread));
  t->fn = fn;
  t->arg = arg;
  if (pthread_create(&th->t, NULL, thread_main, t) != 0) {
    free(t);
    free(th);
    return NULL;
  }
  return th;
}


void os_thread_join (OsThread *t) {
  if (t == NULL) return;
  pthread_join(t->t, NULL);
  free(t);
}


/*
** {==================================================================
** Named pipes for <( ) and >( ): a FIFO in the temporary folder; a
** thread of ours copies between it and an ordinary pipe the other
** command has, so the data streams as it is made
** ===================================================================
*/

struct OsNPipe {
  char *path;
  int fd;	/* our end of the ordinary pipe */
  int to_reader;	/* 1: fd -> the program that opens it; 0: the other way */
  volatile int connected;
  int refs;
  pthread_mutex_t mu;
  OsThread *th;
};


static void npipe_unref (OsNPipe *np) {
  int left;
  pthread_mutex_lock(&np->mu);
  left = --np->refs;
  pthread_mutex_unlock(&np->mu);
  if (left > 0) return;
  unlink(np->path);
  free(np->path);
  pthread_mutex_destroy(&np->mu);
  free(np);
}


static void npipe_relay (void *arg) {
  OsNPipe *np = (OsNPipe *)arg;
  char buf[16384];
  int f = open(np->path, np->to_reader ? O_WRONLY : O_RDONLY);	/* waits for the program */
  np->connected = 1;
  if (f >= 0) {
    for (;;) {
      int from = np->to_reader ? np->fd : f, to = np->to_reader ? f : np->fd;
      ssize_t got = read(from, buf, sizeof(buf)), off = 0;
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) break;
      while (off < got) {
        ssize_t put = write(to, buf + off, (size_t)(got - off));
        if (put < 0 && errno == EINTR) continue;
        if (put <= 0) break;
        off += put;
      }
      if (off < got) break;	/* the other side went away (SIGPIPE is ignored) */
    }
    close(f);
  }
  close(np->fd);
  npipe_unref(np);
}


OsNPipe *os_npipe_new (const char *dir, int fd, int to_reader, char **path) {
  static int counter = 0;
  char name[64];
  OsNPipe *np = (OsNPipe *)xmalloc(sizeof(OsNPipe));
  memset(np, 0, sizeof(*np));
  sprintf(name, "mmc-ps-%ld-%d", (long)getpid(), ++counter);
  np->path = path_join(dir, name);
  unlink(np->path);
  if (mkfifo(np->path, 0600) != 0) {
    free(np->path);
    free(np);
    return NULL;
  }
  np->fd = fd;
  np->to_reader = to_reader;
  np->refs = 2;	/* the thread and the caller */
  pthread_mutex_init(&np->mu, NULL);
  np->th = os_thread_start(npipe_relay, np);
  if (np->th == NULL) {
    unlink(np->path);
    free(np->path);
    pthread_mutex_destroy(&np->mu);
    free(np);
    return NULL;
  }
  *path = xstrdup(np->path);
  return np;
}


/* the command is done. Nobody opened it: we do, so the thread ends. The
** data for a >( ) reader is all through when this returns. */
void os_npipe_end (OsNPipe *np) {
  int tries;
  for (tries = 0; !np->connected && tries < 200; tries++) {
    int f = open(np->path, (np->to_reader ? O_RDONLY : O_WRONLY) | O_NONBLOCK);
    if (f >= 0) {
      close(f);
      break;
    }
    usleep(5000);	/* the thread is not in its open() yet */
  }
  if (np->to_reader) {	/* a <( ) writer may go on for ever: let it */
    pthread_detach(np->th->t);
    free(np->th);
  }
  else os_thread_join(np->th);
  npipe_unref(np);
}

/* }================================================================== */

/* }================================================================== */

#endif
