/*
** mterm.c - mmc-term, the terminal window of the mmc shell
**
**   mmc-term                     the mmc shell in a window, in this folder
**   mmc-term -e prog [args...]   run another program instead
**   mmc-term --hold ...          keep the window when the program ends
**   mmc-term --theme light       dark or light, just for this window
*/

#include "mterm.h"

#include <stdlib.h>
#include <string.h>


static const char *const usage =
  TERM_NAME " " MMC_VERSION " - the terminal window of the mmc shell\n\n"
  "usage: " TERM_NAME " [--theme dark|light] [--hold] [-e program [args...]]\n\n"
  "Settings: etc/mmcterm.conf in the mmc folder.\n"
  "Keys: Ctrl+Shift+C/V copy and paste, Ctrl+Shift+T theme, Ctrl + and - zoom,\n"
  "F11 full screen, Shift+PgUp/PgDn scroll, right click for the menu.";


int main (int argc, char **argv) {
  AppArgs args;
  int i, w, h;
  os_args(&argc, &argv);
  memset(&args, 0, sizeof(args));
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--hold") == 0) args.hold = 1;
    else if (strcmp(argv[i], "--theme") == 0 && i + 1 < argc) args.theme = argv[++i];
    else if (strcmp(argv[i], "--render-test") == 0 && i + 1 < argc)
      args.render_test = argv[++i];
    else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
      args.cmd = &argv[i + 1];	/* argv ends with NULL: so does this */
      break;
    }
    else {
      win_message(TERM_NAME, usage);
      return (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) ? 0 : 2;
    }
  }
  if (app_init(&args, argv[0]) != 0) return 1;
  if (args.render_test != NULL) return app_render_test(args.render_test) ? 1 : 0;
  app_initial_size(&w, &h);
  if (win_create(w, h, TERM_NAME) != 0) {
    win_message(TERM_NAME, "Cannot open a window.");
    return 1;
  }
  if (app_start() != 0) return 1;
  return win_run();
}
