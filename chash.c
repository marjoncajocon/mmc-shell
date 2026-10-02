/* chash.c -- checksums and encodings: md5sum, sha*sum, cksum, sum, base64, base32
**
** Not written yet: each command says so and fails, so a script never
** mistakes it for a working tool. A program of the same name in PATH
** (git-bash's) still wins, as for every fallback. */

#include "mmc.h"

static int not_yet (const char *name, int err) {
  tool_err(err, name, "not available in mmc yet; use git-bash's %s", name);
  return 127;
}

int t_md5sum (int argc, char **argv, int in, int out, int err) {
  (void)argc; (void)argv; (void)in; (void)out;
  return not_yet("md5sum", err);
}

int t_sha1sum (int argc, char **argv, int in, int out, int err) {
  (void)argc; (void)argv; (void)in; (void)out;
  return not_yet("sha1sum", err);
}

int t_sha224sum (int argc, char **argv, int in, int out, int err) {
  (void)argc; (void)argv; (void)in; (void)out;
  return not_yet("sha224sum", err);
}

int t_sha256sum (int argc, char **argv, int in, int out, int err) {
  (void)argc; (void)argv; (void)in; (void)out;
  return not_yet("sha256sum", err);
}

int t_sha384sum (int argc, char **argv, int in, int out, int err) {
  (void)argc; (void)argv; (void)in; (void)out;
  return not_yet("sha384sum", err);
}

int t_sha512sum (int argc, char **argv, int in, int out, int err) {
  (void)argc; (void)argv; (void)in; (void)out;
  return not_yet("sha512sum", err);
}

int t_cksum (int argc, char **argv, int in, int out, int err) {
  (void)argc; (void)argv; (void)in; (void)out;
  return not_yet("cksum", err);
}

int t_sum (int argc, char **argv, int in, int out, int err) {
  (void)argc; (void)argv; (void)in; (void)out;
  return not_yet("sum", err);
}

int t_base64 (int argc, char **argv, int in, int out, int err) {
  (void)argc; (void)argv; (void)in; (void)out;
  return not_yet("base64", err);
}

int t_base32 (int argc, char **argv, int in, int out, int err) {
  (void)argc; (void)argv; (void)in; (void)out;
  return not_yet("base32", err);
}
