/*
** twin32.c - Windows backend of mmc-term
**
** Opens the window, shows the picture that tdraw.c painted, forwards
** keys and the mouse to tapp.c. Newer APIs (per-monitor DPI, colored
** title bar) are looked up at run time, so older Windows still starts.
*/

#include "mterm.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED	0x02E0
#endif
#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL	0x020E
#endif
#define WM_APP_WAKE	(WM_APP + 1)

typedef union Fn {
  FARPROC raw;
  BOOL (WINAPI *set_dpi_context) (HANDLE);
  UINT (WINAPI *dpi_for_window) (HWND);
  UINT (WINAPI *dpi_for_system) (void);
  BOOL (WINAPI *adjust_for_dpi) (LPRECT, DWORD, BOOL, DWORD, UINT);
  int (WINAPI *metrics_for_dpi) (int, UINT);
  HRESULT (WINAPI *dwm_set) (HWND, DWORD, LPCVOID, DWORD);
} Fn;

static HWND hwnd = NULL;
static Fn fn_dpi_window, fn_dpi_system, fn_adjust, fn_dwm, fn_metrics;
static volatile LONG wake_pending = 0;
static UINT cur_dpi = 96;
static WCHAR high_surrogate = 0;
static int swallow_char = 0, tracking_mouse = 0;
static int is_fullscreen = 0;
static WINDOWPLACEMENT saved_place;
static LONG saved_style;
static const Frame *last_frame = NULL;
static int custom_chrome = 0;	/* no system title bar: tdraw.c paints ours */

static wchar_t *widen (const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w = (wchar_t *)xmalloc((size_t)(n > 0 ? n : 1) * sizeof(wchar_t));
  w[0] = L'\0';
  MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  return w;
}


static char *narrow (const wchar_t *w, int len) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, len, NULL, 0, NULL, NULL);
  char *s = (char *)xmalloc((size_t)n + 1);
  WideCharToMultiByte(CP_UTF8, 0, w, len, s, n, NULL, NULL);
  s[n] = '\0';
  return s;
}


static void load_optional (void) {
  HMODULE user = GetModuleHandleA("user32.dll");
  HMODULE dwm = LoadLibraryA("dwmapi.dll");
  Fn ctx;
  ctx.raw = GetProcAddress(user, "SetProcessDpiAwarenessContext");
  fn_dpi_window.raw = GetProcAddress(user, "GetDpiForWindow");
  fn_dpi_system.raw = GetProcAddress(user, "GetDpiForSystem");
  fn_adjust.raw = GetProcAddress(user, "AdjustWindowRectExForDpi");
  fn_metrics.raw = GetProcAddress(user, "GetSystemMetricsForDpi");
  fn_dwm.raw = dwm ? GetProcAddress(dwm, "DwmSetWindowAttribute") : NULL;
  /* -4: per monitor aware v2 */
  if (ctx.raw == NULL || !ctx.set_dpi_context((HANDLE)(intptr_t)-4))
    SetProcessDPIAware();
  if (fn_dpi_system.raw != NULL) cur_dpi = fn_dpi_system.dpi_for_system();
  else {
    HDC dc = GetDC(NULL);
    cur_dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
  }
  if (cur_dpi == 0) cur_dpi = 96;
}


float win_scale (void) {
  static int loaded = 0;
  if (!loaded) {
    loaded = 1;
    load_optional();
  }
  return (float)cur_dpi / 96.0f;
}


unsigned win_ticks (void) {
  return (unsigned)GetTickCount();
}


static int metric (int index) {
  return fn_metrics.raw != NULL ? fn_metrics.metrics_for_dpi(index, cur_dpi)
                                : GetSystemMetrics(index);
}


/* the invisible resize border Windows keeps around a window */
static int frame_x (void) { return metric(SM_CXFRAME) + metric(92); }
static int frame_y (void) { return metric(SM_CYFRAME) + metric(92); }


/* client size -> window size */
static void outer_size (int *w, int *h) {
  RECT r;
  if (custom_chrome) {	/* borders left, right and below; nothing on top */
    *w += 2 * frame_x();
    *h += frame_y();
    return;
  }
  DWORD style = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);
  r.left = r.top = 0;
  r.right = *w;
  r.bottom = *h;
  if (hwnd == NULL) style = WS_OVERLAPPEDWINDOW;
  if (fn_adjust.raw != NULL) fn_adjust.adjust_for_dpi(&r, style, FALSE, 0, cur_dpi);
  else AdjustWindowRectEx(&r, style, FALSE, 0);
  *w = r.right - r.left;
  *h = r.bottom - r.top;
}


/*
** {==================================================================
** Input
** ===================================================================
*/

static int current_mods (void) {
  int m = 0;
  if (GetKeyState(VK_SHIFT) < 0) m |= TM_SHIFT;
  if (GetKeyState(VK_CONTROL) < 0) m |= TM_CTRL;
  if (GetKeyState(VK_MENU) < 0) m |= TM_ALT;
  /* AltGr arrives as Ctrl + right Alt: it types characters, it is no chord */
  if ((m & TM_CTRL) && GetKeyState(VK_RMENU) < 0) m &= ~(TM_CTRL | TM_ALT);
  return m;
}


static int special_key (WPARAM vk) {
  switch (vk) {
    case VK_UP: return TK_UP;
    case VK_DOWN: return TK_DOWN;
    case VK_LEFT: return TK_LEFT;
    case VK_RIGHT: return TK_RIGHT;
    case VK_HOME: return TK_HOME;
    case VK_END: return TK_END;
    case VK_PRIOR: return TK_PGUP;
    case VK_NEXT: return TK_PGDN;
    case VK_INSERT: return TK_INSERT;
    case VK_DELETE: return TK_DELETE;
    case VK_RETURN: return TK_ENTER;
    case VK_TAB: return TK_TAB;
    case VK_BACK: return TK_BACKSPACE;
    case VK_ESCAPE: return TK_ESCAPE;
    default:
      if (vk >= VK_F1 && vk <= VK_F12) return TK_F1 + (int)(vk - VK_F1);
      return TK_NONE;
  }
}


/* returns 1 if the key was used up */
static int on_keydown (WPARAM vk) {
  int mods = current_mods();
  int key = special_key(vk);
  swallow_char = 0;
  if (key != TK_NONE) {
    if (vk == VK_F4 && (mods & TM_ALT)) return 0;	/* Alt+F4 closes */
    /* TranslateMessage also makes a WM_CHAR for these four: drop it */
    if (vk == VK_RETURN || vk == VK_TAB || vk == VK_BACK || vk == VK_ESCAPE)
      swallow_char = 1;
    return app_on_key(key, mods, 0);
  }
  if (mods & TM_CTRL) {	/* maybe a shortcut like Ctrl+Shift+C or Ctrl+= */
    uint32_t cp = 0;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) cp = (uint32_t)vk;
    else if (vk == VK_OEM_PLUS || vk == VK_ADD) cp = '=';
    else if (vk == VK_OEM_MINUS || vk == VK_SUBTRACT) cp = '-';
    else if (vk == VK_SPACE) cp = ' ';
    else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) cp = (uint32_t)('0' + (vk - VK_NUMPAD0));
    if (cp != 0 && app_on_key(TK_CHAR, mods, cp)) {
      swallow_char = 1;
      return 1;
    }
  }
  return 0;
}


static void on_char (WPARAM wc, int alt) {
  WCHAR pair[2];
  char *utf8;
  int n = 1;
  if (swallow_char) {
    swallow_char = 0;
    return;
  }
  if (wc >= 0xD800 && wc <= 0xDBFF) {	/* first half of a surrogate pair */
    high_surrogate = (WCHAR)wc;
    return;
  }
  pair[0] = (WCHAR)wc;
  if (wc >= 0xDC00 && wc <= 0xDFFF && high_surrogate != 0) {
    pair[0] = high_surrogate;
    pair[1] = (WCHAR)wc;
    n = 2;
  }
  high_surrogate = 0;
  if (wc == 0) {	/* Ctrl+2 / Ctrl+@ */
    app_on_key(TK_CHAR, TM_CTRL, ' ');
    return;
  }
  utf8 = narrow(pair, n);
  app_on_text(utf8, alt ? TM_ALT : 0);
  free(utf8);
}


static void on_mouse (int type, int button, LPARAM lp, WPARAM wp, int arg) {
  int mods = 0;
  if (wp & MK_SHIFT) mods |= TM_SHIFT;
  if (wp & MK_CONTROL) mods |= TM_CTRL;
  if (GetKeyState(VK_MENU) < 0) mods |= TM_ALT;
  app_on_mouse(type, button, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), mods, arg);
}

/* }================================================================== */


/* our own redraws (win_redraw) mark just this pixel as out of date;
** what the system marks (the window uncovered, resized) is bigger */
static const RECT ours = {0, 0, 1, 1};


/* rows y .. y+h of the frame to the window */
static void put_rows (HDC dc, const Frame *f, int y, int h) {
  BITMAPINFO bi;
  memset(&bi, 0, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = f->w;
  bi.bmiHeader.biHeight = -h;	/* top row first */
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  SetDIBitsToDevice(dc, 0, y, (DWORD)f->w, (DWORD)h, 0, 0, 0, (UINT)h,
                    f->px + (size_t)y * (size_t)f->w, &bi, DIB_RGB_COLORS);
}


/*
** The window shows what changed: after our own redraw only the rows
** that were drawn again go to the screen (typing: one row of pixels, not
** the window), after the system's the whole picture.
*/
static void paint (void) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(hwnd, &ps);
  const Frame *f = app_render();
  int only_ours = EqualRect(&ps.rcPaint, &ours);
  if (f != NULL) last_frame = f;
  if (last_frame != NULL && last_frame->px != NULL) {
    if (only_ours && f != NULL && f->dh > 0 && f->dh < f->h) {
      HDC wdc = GetDC(hwnd);	/* not clipped to the one pixel */
      put_rows(wdc, f, f->dy, f->dh);
      ReleaseDC(hwnd, wdc);
    }
    else if (!only_ours || f != NULL)
      put_rows(dc, last_frame, 0, last_frame->h);
  }
  EndPaint(hwnd, &ps);
}


static LRESULT CALLBACK wndproc (HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_NCCALCSIZE:	/* our title bar: the client area takes the top */
      if (custom_chrome && wp) {
        RECT *r = &((NCCALCSIZE_PARAMS *)lp)->rgrc[0];
        if (is_fullscreen) return 0;
        r->left += frame_x();
        r->right -= frame_x();
        r->bottom -= frame_y();
        if (IsZoomed(h)) r->top += frame_y();	/* a maximized window overhangs */
        return 0;
      }
      break;
    case WM_NCHITTEST:
      if (custom_chrome && !is_fullscreen) {
        LRESULT hit = DefWindowProcW(h, msg, wp, lp);
        POINT p;
        RECT c;
        int band = frame_y();
        if (hit != HTCLIENT) return hit;	/* left, right, bottom borders */
        p.x = GET_X_LPARAM(lp);
        p.y = GET_Y_LPARAM(lp);
        ScreenToClient(h, &p);
        GetClientRect(h, &c);
        if (!IsZoomed(h) && p.y < band / 2 + 2) {	/* resize from the top */
          if (p.x < band) return HTTOPLEFT;
          if (p.x >= c.right - band) return HTTOPRIGHT;
          return HTTOP;
        }
        return app_hit_test(p.x, p.y) == HIT_CAPTION ? HTCAPTION : HTCLIENT;
      }
      break;
    case WM_NCACTIVATE:	/* never let the classic caption be painted */
      if (custom_chrome) return DefWindowProcW(h, msg, wp, -1);
      break;
    case WM_PAINT: paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
      if (wp != SIZE_MINIMIZED) app_on_resize(LOWORD(lp), HIWORD(lp));
      return 0;
    case WM_SIZING: {	/* snap to whole character cells */
      RECT *r = (RECT *)lp;
      int fw = 0, fh = 0, w, h2;
      outer_size(&fw, &fh);	/* size of the frame around the client area */
      w = (r->right - r->left) - fw;
      h2 = (r->bottom - r->top) - fh;
      app_snap_size(&w, &h2);
      if (wp == WMSZ_LEFT || wp == WMSZ_TOPLEFT || wp == WMSZ_BOTTOMLEFT)
        r->left = r->right - (w + fw);
      else r->right = r->left + w + fw;
      if (wp == WMSZ_TOP || wp == WMSZ_TOPLEFT || wp == WMSZ_TOPRIGHT)
        r->top = r->bottom - (h2 + fh);
      else r->bottom = r->top + h2 + fh;
      return TRUE;
    }
    case WM_ENTERSIZEMOVE: app_resizing(1); return 0;
    case WM_EXITSIZEMOVE: app_resizing(0); return 0;
    case WM_DPICHANGED: {
      const RECT *r = (const RECT *)lp;
      cur_dpi = HIWORD(wp);
      app_on_dpi((float)cur_dpi / 96.0f);
      SetWindowPos(h, NULL, r->left, r->top, r->right - r->left,
                   r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_KEYDOWN:
      if (on_keydown(wp)) return 0;
      break;
    case WM_SYSKEYDOWN:
      if (on_keydown(wp)) return 0;
      if (wp == VK_F10) return 0;	/* no menu bar to activate */
      break;
    case WM_CHAR: on_char(wp, 0); return 0;
    case WM_SYSCHAR:
      if (wp == ' ') break;	/* Alt+Space: the window menu */
      on_char(wp, 1);
      return 0;
    case WM_LBUTTONDOWN: SetCapture(h); on_mouse(TMS_DOWN, 1, lp, wp, 0); return 0;
    case WM_LBUTTONUP: ReleaseCapture(); on_mouse(TMS_UP, 1, lp, wp, 0); return 0;
    case WM_MBUTTONDOWN: on_mouse(TMS_DOWN, 2, lp, wp, 0); return 0;
    case WM_RBUTTONDOWN: on_mouse(TMS_DOWN, 3, lp, wp, 0); return 0;
    case WM_NCRBUTTONDOWN:	/* our title bar: our menu, not the system one */
      if (custom_chrome && wp == HTCAPTION) {
        POINT p;
        p.x = GET_X_LPARAM(lp);
        p.y = GET_Y_LPARAM(lp);
        ScreenToClient(h, &p);
        on_mouse(TMS_DOWN, 3, MAKELPARAM(p.x, p.y), 0, 0);
        return 0;
      }
      break;
    case WM_NCRBUTTONUP:
      if (custom_chrome && wp == HTCAPTION) return 0;
      break;
    case WM_MOUSEMOVE:
      if (!tracking_mouse) {
        TRACKMOUSEEVENT t;
        t.cbSize = sizeof(t);
        t.dwFlags = TME_LEAVE;
        t.hwndTrack = h;
        t.dwHoverTime = 0;
        tracking_mouse = TrackMouseEvent(&t) != 0;
      }
      on_mouse(TMS_MOVE, 0, lp, wp, 0);
      return 0;
    case WM_MOUSELEAVE:
      tracking_mouse = 0;
      app_on_mouse(TMS_LEAVE, 0, -1, -1, 0, 0);
      return 0;
    case WM_MOUSEWHEEL: {
      POINT p;
      int lines = GET_WHEEL_DELTA_WPARAM(wp) * 3 / WHEEL_DELTA;
      p.x = GET_X_LPARAM(lp);
      p.y = GET_Y_LPARAM(lp);
      ScreenToClient(h, &p);
      if (lines == 0) lines = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1;
      on_mouse(TMS_WHEEL, 0, MAKELPARAM(p.x, p.y), GET_KEYSTATE_WPARAM(wp), lines);
      return 0;
    }
    case WM_SETFOCUS: app_on_focus(1); return 0;
    case WM_KILLFOCUS: app_on_focus(0); return 0;
    case WM_TIMER: app_on_tick(win_ticks()); return 0;
    case WM_APP_WAKE:
      InterlockedExchange(&wake_pending, 0);
      app_on_wake();
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {	/* text beam, hand on buttons, arrow on UI */
        POINT p;
        int hit;
        GetCursorPos(&p);
        ScreenToClient(h, &p);
        hit = app_hit_test(p.x, p.y);
        SetCursor(LoadCursor(NULL, hit == HIT_BUTTON ? IDC_HAND :
                                   hit == HIT_CLIENT ? IDC_IBEAM : IDC_ARROW));
        return TRUE;
      }
      break;
    case WM_DESTROY:
      app_close_all();
      hwnd = NULL;
      PostQuitMessage(app_exit_code());
      return 0;
    default: break;
  }
  return DefWindowProcW(h, msg, wp, lp);
}


int win_create (int w, int h, const char *title) {
  WNDCLASSEXW wc;
  HINSTANCE inst = GetModuleHandleW(NULL);
  wchar_t *wtitle = widen(title);
  int ow = w, oh = h;
  win_scale();
  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = wndproc;
  wc.hInstance = inst;
  wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                               GetSystemMetrics(SM_CXICON),
                               GetSystemMetrics(SM_CYICON), 0);
  wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXSMICON),
                                 GetSystemMetrics(SM_CYSMICON), 0);
  wc.hCursor = LoadCursor(NULL, IDC_IBEAM);
  wc.lpszClassName = L"mmc-term";
  RegisterClassExW(&wc);
  outer_size(&ow, &oh);
  hwnd = CreateWindowExW(0, wc.lpszClassName, wtitle, WS_OVERLAPPEDWINDOW,
                         CW_USEDEFAULT, CW_USEDEFAULT, ow, oh, NULL, NULL,
                         inst, NULL);
  free(wtitle);
  if (hwnd == NULL) return -1;
  if (custom_chrome)
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_FRAMECHANGED | SWP_NOACTIVATE);
  if (fn_dpi_window.raw != NULL) {	/* the monitor it landed on may differ */
    UINT dpi = fn_dpi_window.dpi_for_window(hwnd);
    if (dpi != 0 && dpi != cur_dpi) {
      cur_dpi = dpi;
      app_on_dpi((float)dpi / 96.0f);
      app_initial_size(&w, &h);
      win_set_size(w, h);
    }
  }
  return 0;
}


int win_run (void) {
  MSG msg;
  if (hwnd == NULL) return 1;
  SetTimer(hwnd, 1, 33, NULL);
  ShowWindow(hwnd, SW_SHOWNORMAL);
  UpdateWindow(hwnd);
  while (GetMessageW(&msg, NULL, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return (int)msg.wParam;
}


void win_close (void) {
  if (hwnd != NULL) DestroyWindow(hwnd);
}


void win_wake (void) {
  HWND h = hwnd;
  if (h != NULL && InterlockedExchange(&wake_pending, 1) == 0)
    PostMessageW(h, WM_APP_WAKE, 0, 0);
}


void win_redraw (void) {
  if (hwnd != NULL) InvalidateRect(hwnd, &ours, FALSE);	/* see paint() */
}


void win_set_title (const char *utf8) {
  wchar_t *w = widen(utf8);
  if (hwnd != NULL) SetWindowTextW(hwnd, w);
  free(w);
}


void win_set_size (int w, int h) {
  if (hwnd == NULL || is_fullscreen || IsZoomed(hwnd)) return;
  outer_size(&w, &h);
  SetWindowPos(hwnd, NULL, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}


void win_set_fullscreen (int on) {
  if (hwnd == NULL || on == is_fullscreen) return;
  is_fullscreen = on;
  if (on) {
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    saved_place.length = sizeof(saved_place);
    GetWindowPlacement(hwnd, &saved_place);
    saved_style = GetWindowLongW(hwnd, GWL_STYLE);
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowLongW(hwnd, GWL_STYLE, saved_style & ~(LONG)WS_OVERLAPPEDWINDOW);
    SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
  }
  else {
    SetWindowLongW(hwnd, GWL_STYLE, saved_style);
    SetWindowPlacement(hwnd, &saved_place);
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
  }
}


/* title bar in the theme colors (Windows 11), dark title bar (Windows 10) */
void win_set_chrome (uint32_t bg, uint32_t border, uint32_t text, int dark) {
  BOOL on = dark ? TRUE : FALSE;
  DWORD corner = 2;	/* round */
  COLORREF c;
  if (hwnd == NULL || fn_dwm.raw == NULL) return;
  fn_dwm.dwm_set(hwnd, 20, &on, sizeof(on));
  fn_dwm.dwm_set(hwnd, 33, &corner, sizeof(corner));
  c = RGB((bg >> 16) & 0xFF, (bg >> 8) & 0xFF, bg & 0xFF);
  fn_dwm.dwm_set(hwnd, 35, &c, sizeof(c));
  c = RGB((border >> 16) & 0xFF, (border >> 8) & 0xFF, border & 0xFF);
  fn_dwm.dwm_set(hwnd, 34, &c, sizeof(c));
  c = RGB((text >> 16) & 0xFF, (text >> 8) & 0xFF, text & 0xFF);
  fn_dwm.dwm_set(hwnd, 36, &c, sizeof(c));
}


void win_set_opacity (int percent) {
  LONG ex;
  if (hwnd == NULL) return;
  ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
  if (percent >= 100) {
    SetWindowLongW(hwnd, GWL_EXSTYLE, ex & ~(LONG)WS_EX_LAYERED);
    return;
  }
  SetWindowLongW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
  SetLayeredWindowAttributes(hwnd, 0, (BYTE)(percent * 255 / 100), LWA_ALPHA);
}


void win_set_clipboard (const char *utf8) {
  wchar_t *w = widen(utf8);
  size_t bytes = (wcslen(w) + 1) * sizeof(wchar_t);
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (mem != NULL && hwnd != NULL && OpenClipboard(hwnd)) {
    memcpy(GlobalLock(mem), w, bytes);
    GlobalUnlock(mem);
    EmptyClipboard();
    if (SetClipboardData(CF_UNICODETEXT, mem) == NULL) GlobalFree(mem);
    CloseClipboard();
  }
  else if (mem != NULL) GlobalFree(mem);
  free(w);
}


void win_request_paste (void) {
  if (hwnd == NULL || !OpenClipboard(hwnd)) return;
  {
    HANDLE mem = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *w = mem ? (const wchar_t *)GlobalLock(mem) : NULL;
    char *utf8 = w ? narrow(w, (int)wcslen(w)) : NULL;
    if (w) GlobalUnlock(mem);
    CloseClipboard();
    if (utf8 != NULL) app_on_paste(utf8);
    free(utf8);
  }
}


int win_custom_chrome (int want) {
  custom_chrome = want;
  return want;
}


void win_minimize (void) {
  if (hwnd != NULL) ShowWindow(hwnd, SW_MINIMIZE);
}


void win_toggle_maximize (void) {
  if (hwnd != NULL) ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
}


int win_is_maximized (void) {
  return hwnd != NULL && IsZoomed(hwnd);
}


void win_flash (void) {
  if (hwnd != NULL && GetForegroundWindow() != hwnd) FlashWindow(hwnd, TRUE);
}


void win_message (const char *title, const char *text) {
  wchar_t *wt = widen(title), *wx = widen(text);
  MessageBoxW(hwnd, wx, wt, MB_OK | MB_ICONINFORMATION);
  free(wt);
  free(wx);
}


void win_open_url (const char *utf8) {
  wchar_t *w = widen(utf8);
  ShellExecuteW(hwnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
  free(w);
}

#else

typedef int twin32_is_empty_here;	/* ISO C wants something in every file */

#endif
