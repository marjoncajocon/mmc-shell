/*
** tpty.c - pseudo terminal of mmc-term
**
** Starts the shell "behind" the window: whatever the window writes is
** the keyboard of the shell, whatever the shell prints comes back as
** bytes for tvt.c. Windows uses ConPTY (Windows 10 1809 or newer),
** Linux and macOS use a classic pty.
*/

#include "mterm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#ifdef _WIN32

/*
** {==================================================================
** Windows: ConPTY
** ===================================================================
*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE	0x00020016
#endif
#ifndef EXTENDED_STARTUPINFO_PRESENT
#define EXTENDED_STARTUPINFO_PRESENT	0x00080000
#endif

typedef HRESULT (WINAPI *CreatePcFn) (COORD, HANDLE, HANDLE, DWORD, void **);
typedef HRESULT (WINAPI *ResizePcFn) (void *, COORD);
typedef void (WINAPI *ClosePcFn) (void *);

/* the functions are looked up at run time so old Windows can say "no" */
typedef union FnPtr {
  FARPROC raw;
  CreatePcFn create;
  ResizePcFn resize;
  ClosePcFn close;
} FnPtr;

static FnPtr fn_create, fn_resize, fn_close;
static void *pcon = NULL;
static HANDLE in_write = NULL, out_read = NULL, process = NULL;
static CRITICAL_SECTION lock;
static Buf queue;
static size_t queue_pos = 0;
static volatile LONG closed = 0, exited = 0;
static DWORD exit_code = 0;


static void quote_arg (Buf *b, const char *a) {
  const char *p;
  if (a[0] != '\0' && strpbrk(a, " \t\"") == NULL) {
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


static wchar_t *widen (const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w = (wchar_t *)xmalloc((size_t)(n > 0 ? n : 1) * sizeof(wchar_t));
  w[0] = L'\0';
  MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  return w;
}


static DWORD WINAPI reader_thread (LPVOID arg) {
  char buf[16384];
  DWORD n;
  (void)arg;
  while (ReadFile(out_read, buf, sizeof(buf), &n, NULL) && n > 0) {
    size_t pending;
    EnterCriticalSection(&lock);
    buf_putn(&queue, buf, n);
    pending = queue.len - queue_pos;
    LeaveCriticalSection(&lock);
    win_wake();
    while (pending > ((size_t)4 << 20) && !closed) {	/* window is behind */
      Sleep(2);
      EnterCriticalSection(&lock);
      pending = queue.len - queue_pos;
      LeaveCriticalSection(&lock);
    }
  }
  InterlockedExchange(&closed, 1);
  win_wake();
  return 0;
}


static DWORD WINAPI waiter_thread (LPVOID arg) {
  (void)arg;
  WaitForSingleObject(process, INFINITE);
  GetExitCodeProcess(process, &exit_code);
  Sleep(60);	/* let the last output arrive */
  InterlockedExchange(&exited, 1);
  win_wake();
  return 0;
}


int pty_spawn (const char *exe, char **argv, int cols, int rows) {
  HMODULE k32 = GetModuleHandleA("kernel32.dll");
  HANDLE in_read = NULL, out_write = NULL;
  HANDLE old_in, old_out, old_err;
  STARTUPINFOEXW si;
  PROCESS_INFORMATION pi;
  SIZE_T attr_size = 0;
  COORD size;
  Buf cl;
  wchar_t *wexe, *wcl;
  int i, ok;
  fn_create.raw = GetProcAddress(k32, "CreatePseudoConsole");
  fn_resize.raw = GetProcAddress(k32, "ResizePseudoConsole");
  fn_close.raw = GetProcAddress(k32, "ClosePseudoConsole");
  if (fn_create.raw == NULL || fn_resize.raw == NULL || fn_close.raw == NULL) {
    win_message(TERM_NAME, "This Windows has no pseudo console (ConPTY).\n"
                           "Windows 10 version 1809 or newer is needed.");
    return -1;
  }
  if (!CreatePipe(&in_read, &in_write, NULL, 0) ||
      !CreatePipe(&out_read, &out_write, NULL, 0))
    return -1;
  size.X = (SHORT)cols;
  size.Y = (SHORT)rows;
  if (fn_create.create(size, in_read, out_write, 0, &pcon) != S_OK) return -1;
  memset(&si, 0, sizeof(si));
  memset(&pi, 0, sizeof(pi));
  si.StartupInfo.cb = sizeof(si);
  InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
  si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)xmalloc(attr_size);
  InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size);
  UpdateProcThreadAttribute(si.lpAttributeList, 0,
                            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pcon,
                            sizeof(pcon), NULL, NULL);
  buf_init(&cl);
  quote_arg(&cl, exe);
  for (i = 1; argv[i] != NULL; i++) {
    buf_putc(&cl, ' ');
    quote_arg(&cl, argv[i]);
  }
  wexe = widen(exe);
  wcl = widen(cl.s);
  /* our own std handles (if any) must not leak into the child: it has to
  ** take the ones of the pseudo console */
  old_in = GetStdHandle(STD_INPUT_HANDLE);
  old_out = GetStdHandle(STD_OUTPUT_HANDLE);
  old_err = GetStdHandle(STD_ERROR_HANDLE);
  SetStdHandle(STD_INPUT_HANDLE, NULL);
  SetStdHandle(STD_OUTPUT_HANDLE, NULL);
  SetStdHandle(STD_ERROR_HANDLE, NULL);
  ok = CreateProcessW(wexe, wcl, NULL, NULL, FALSE,
                      EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                      NULL, NULL, &si.StartupInfo, &pi) != 0;
  SetStdHandle(STD_INPUT_HANDLE, old_in);
  SetStdHandle(STD_OUTPUT_HANDLE, old_out);
  SetStdHandle(STD_ERROR_HANDLE, old_err);
  DeleteProcThreadAttributeList(si.lpAttributeList);
  free(si.lpAttributeList);
  free(wexe);
  free(wcl);
  buf_free(&cl);
  CloseHandle(in_read);
  CloseHandle(out_write);
  if (!ok) return -1;
  CloseHandle(pi.hThread);
  process = pi.hProcess;
  InitializeCriticalSection(&lock);
  buf_init(&queue);
  CloseHandle(CreateThread(NULL, 0, reader_thread, NULL, 0, NULL));
  CloseHandle(CreateThread(NULL, 0, waiter_thread, NULL, 0, NULL));
  return 0;
}


void pty_write (const char *s, size_t n) {
  while (n > 0 && in_write != NULL) {
    DWORD put = 0;
    if (!WriteFile(in_write, s, (DWORD)n, &put, NULL) || put == 0) return;
    s += put;
    n -= put;
  }
}


void pty_resize (int cols, int rows) {
  COORD size;
  if (pcon == NULL) return;
  size.X = (SHORT)cols;
  size.Y = (SHORT)rows;
  fn_resize.resize(pcon, size);
}


long pty_read (char *buf, size_t n) {
  size_t have;
  if (process == NULL) return -1;
  EnterCriticalSection(&lock);
  have = queue.len - queue_pos;
  if (have > n) have = n;
  if (have > 0) {
    memcpy(buf, queue.s + queue_pos, have);
    queue_pos += have;
    if (queue_pos == queue.len) {	/* all read: start over */
      queue.len = 0;
      queue_pos = 0;
    }
  }
  LeaveCriticalSection(&lock);
  if (have == 0 && closed) return -1;
  return (long)have;
}


int pty_fd (void) {
  return -1;
}


int pty_exited (int *code) {
  if (!exited) return 0;
  if (code) *code = (int)exit_code;
  return 1;
}


void pty_close (void) {
  if (pcon != NULL) {
    void *p = pcon;
    pcon = NULL;
    InterlockedExchange(&closed, 1);
    fn_close.close(p);	/* ends the shell if it is still there */
  }
  if (in_write != NULL) {
    CloseHandle(in_write);
    in_write = NULL;
  }
}

/* }================================================================== */

#else

/*
** {==================================================================
** Linux, macOS: pty
** ===================================================================
*/

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static int master = -1;
static pid_t child = -1;
static int child_status = 0, child_done = 0;


static void set_size (int fd, int cols, int rows) {
  struct winsize ws;
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = (unsigned short)cols;
  ws.ws_row = (unsigned short)rows;
  ioctl(fd, TIOCSWINSZ, &ws);
}


int pty_spawn (const char *exe, char **argv, int cols, int rows) {
  const char *slave_name;
  master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0) return -1;
  if (grantpt(master) != 0 || unlockpt(master) != 0 ||
      (slave_name = ptsname(master)) == NULL) {
    close(master);
    master = -1;
    return -1;
  }
  set_size(master, cols, rows);
  child = fork();
  if (child < 0) return -1;
  if (child == 0) {
    int slave;
    setsid();	/* new session: the pty becomes the controlling terminal */
    slave = open(slave_name, O_RDWR);
    if (slave < 0) _exit(126);
#ifdef TIOCSCTTY
    ioctl(slave, TIOCSCTTY, 0);
#endif
    dup2(slave, 0);
    dup2(slave, 1);
    dup2(slave, 2);
    if (slave > 2) close(slave);
    close(master);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    execv(exe, argv);
    _exit(127);
  }
  fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
  fcntl(master, F_SETFD, FD_CLOEXEC);
  return 0;
}


void pty_write (const char *s, size_t n) {
  while (n > 0 && master >= 0) {
    ssize_t r = write(master, s, n);
    if (r > 0) {
      s += r;
      n -= (size_t)r;
    }
    else if (r < 0 && (errno == EAGAIN || errno == EINTR)) {
      struct pollfd p;	/* the shell is busy: wait until it takes more */
      p.fd = master;
      p.events = POLLOUT;
      p.revents = 0;
      poll(&p, 1, 100);
    }
    else return;
  }
}


void pty_resize (int cols, int rows) {
  if (master >= 0) set_size(master, cols, rows);
}


long pty_read (char *buf, size_t n) {
  ssize_t r;
  if (master < 0) return -1;
  r = read(master, buf, n);
  if (r > 0) return (long)r;
  if (r < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
  return -1;	/* 0 or EIO: the shell side is closed */
}


int pty_fd (void) {
  return master;
}


int pty_exited (int *code) {
  if (!child_done && child > 0 && waitpid(child, &child_status, WNOHANG) == child)
    child_done = 1;
  if (!child_done) return 0;
  if (code)
    *code = WIFEXITED(child_status) ? WEXITSTATUS(child_status)
                                    : 128 + WTERMSIG(child_status);
  return 1;
}


void pty_close (void) {
  if (master >= 0) {
    close(master);	/* the shell gets a hangup */
    master = -1;
  }
}

/* }================================================================== */

#endif
