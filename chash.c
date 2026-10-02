/*
** chash.c - checksums and encodings: md5sum sha1sum sha224sum sha256sum
** sha384sum sha512sum cksum sum base64 base32
**
** They follow GNU coreutils 8.32: the same options, output, messages and
** exit statuses. Notes:
** - MD5 (RFC 1321), SHA-1 and SHA-2 (FIPS 180-4) are written here from
**   the specs; everything streams, so a file of any size works.
** - The *sum tools share one driver: -b -t --tag -z, and -c with --quiet
**   --status --warn --strict --ignore-missing. A name holding '\' or a
**   newline is escaped ("\" at the start of the line), as GNU does.
** - On Windows the default mode is binary ('*' before the name), like
**   git-bash's tools (and GNU on any system with O_BINARY); elsewhere it
**   is ' '. Reading is always binary: -t changes only that character.
** - -c keeps a '\r' at the end of a line (part of the name), as 8.32 does.
** - cksum is the POSIX CRC (not gzip's CRC-32), with the size; sum is
**   BSD (-r, the default) or System V (-s).
** - base64 / base32 decode with gnulib's decoder, step for step, so
**   what is written before "invalid input" matches too.
** - Bad options are reported in glibc's words ("invalid option -- 'x'");
**   git-bash's getopt says "unknown option -- x".
*/

#include "mmc.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OPT_VERSION	(-3)
#define CHUNK		65536

#ifdef _WIN32
#define BINARY_DEFAULT	1	/* like O_BINARY systems: '*' unless stdin is a terminal */
#else
#define BINARY_DEFAULT	0
#endif


/*
** {==================================================================
** Shared helpers
** ===================================================================
*/

static int tool_version (int out, const char *tool) {
  fd_printf(out, "%s (mmc) %s\n", tool, MMC_VERSION);
  return 0;
}


static int try_help (int err, const char *tool) {
  fd_printf(err, "Try '%s --help' for more information.\n", tool);
  return 1;
}


/* a name in a message, quoted when a shell would need it: GNU's quotef */
static int qf_plain (const char *s) {
  const unsigned char *p = (const unsigned char *)s;
  if (*p == '\0') return 0;
  if ((p[0] == '#' || p[0] == '~') ||
      ((p[0] == '{' || p[0] == '}') && p[1] == '\0')) return 0;
  for (; *p; p++) {
    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')) continue;
    if (*p >= 0x80 && tool_utf8()) continue;
    if (strchr("%+,-./]_@#~{}", *p) == NULL) return 0;
  }
  return 1;
}


static const char *qf (const char *s) {
  static char *bufs[4];
  static int k = 0;
  const unsigned char *p;
  int compat = 1, squote = 0, pending = 0;
  Buf b;
  k = (k + 1) & 3;
  free(bufs[k]);
  if (qf_plain(s)) return (bufs[k] = xstrdup(s));
  for (p = (const unsigned char *)s; *p; p++) {
    if (*p == '\'') squote = 1;
    else if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
               (*p >= 0x80 && tool_utf8()) || strchr(" %+,-./:]_", *p) != NULL)) compat = 0;
  }
  buf_init(&b);
  if (squote && compat) {	/* "it's" */
    buf_putc(&b, '"');
    buf_puts(&b, s);
    buf_putc(&b, '"');
    return (bufs[k] = buf_take(&b));
  }
  buf_putc(&b, '\'');
  for (p = (const unsigned char *)s; *p; p++) {
    int c = *p;
    if (c < 0x20 || c == 0x7f || (c >= 0x80 && !tool_utf8())) {	/* 'a'$'\n''b' */
      static const char letters[] = "\aa\bb\ff\nn\rr\tt\vv";
      const char *l = c ? strchr(letters, c) : NULL;
      if (!pending) buf_puts(&b, "'$'");
      pending = 1;
      if (l != NULL && ((l - letters) & 1) == 0) {
        buf_putc(&b, '\\');
        buf_putc(&b, l[1]);
      }
      else buf_printf(&b, "\\%03o", c);
      continue;
    }
    if (pending) {
      buf_puts(&b, "''");
      pending = 0;
    }
    if (c == '\'') buf_puts(&b, "'\\''");
    else buf_putc(&b, (char)c);
  }
  buf_putc(&b, '\'');
  return (bufs[k] = buf_take(&b));
}


/* opens a file to read ("-": stdin), or says why not ("tool: name: why");
** skip_noent: a file that is not there fails with -2, silently */
static int open_in (In *r, const char *tool, const char *name, int in, int err, Out *o,
                    int skip_noent) {
  char *native;
  OsStat st;
  int isdir;
  if (strcmp(name, "-") == 0) {
    in_init(r, in, 0);
    return 0;
  }
  native = path_to_native(name);
  isdir = os_stat(native, &st) == 0 && st.is_dir;
  free(native);
  if (!isdir && in_open(r, name, in) == 0) return 0;
  if (!isdir && skip_noent && os_errcode() == OS_E_NOENT) return -2;
  if (o) out_flush(o);
  if (isdir) tool_err(err, tool, "%s: Is a directory", qf(name));
  else tool_err(err, tool, "%s: %s", qf(name), os_errmsg());
  return -1;
}


/* reads n bytes unless the input ends first (fread's way) */
static size_t read_full (In *r, unsigned char *p, size_t n, int *eof) {
  size_t got = 0;
  while (got < n) {
    long k = in_read(r, (char *)p + got, n - got);
    if (k <= 0 || tool_stop()) {
      *eof = 1;
      break;
    }
    got += (size_t)k;
  }
  return got;
}

/* }================================================================== */


/*
** {==================================================================
** MD5, SHA-1, SHA-224/256, SHA-384/512
** ===================================================================
*/

enum { H_MD5, H_SHA1, H_SHA224, H_SHA256, H_SHA384, H_SHA512 };

typedef struct Hash {
  int algo;
  size_t size, used;	/* block size; bytes waiting in block */
  uint64_t total;	/* bytes hashed */
  uint32_t h[8];
  uint64_t g[8];
  unsigned char block[128];
} Hash;

#define ROL32(x, n)	(((x) << (n)) | ((x) >> (32 - (n))))
#define ROR32(x, n)	(((x) >> (n)) | ((x) << (32 - (n))))
#define ROR64(x, n)	(((x) >> (n)) | ((x) << (64 - (n))))

static uint32_t get_le32 (const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}


static uint32_t get_be32 (const unsigned char *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}


static uint64_t get_be64 (const unsigned char *p) {
  return ((uint64_t)get_be32(p) << 32) | get_be32(p + 4);
}


static const uint32_t md5_k[64] = {
  0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
  0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
  0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
  0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
  0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
  0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
  0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
  0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

static const unsigned char md5_r[64] = {
  7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
  5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static void md5_block (Hash *h, const unsigned char *p) {
  uint32_t w[16], a = h->h[0], b = h->h[1], c = h->h[2], d = h->h[3], f, t;
  int i, g;
  for (i = 0; i < 16; i++) w[i] = get_le32(p + 4 * i);
  for (i = 0; i < 64; i++) {
    if (i < 16) { f = (b & c) | (~b & d); g = i; }
    else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
    else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
    else { f = c ^ (b | ~d); g = (7 * i) & 15; }
    t = d;
    d = c;
    c = b;
    f = a + f + md5_k[i] + w[g];
    b = b + ROL32(f, md5_r[i]);
    a = t;
  }
  h->h[0] += a;
  h->h[1] += b;
  h->h[2] += c;
  h->h[3] += d;
}


static void sha1_block (Hash *h, const unsigned char *p) {
  uint32_t w[80], a = h->h[0], b = h->h[1], c = h->h[2], d = h->h[3], e = h->h[4], f, k, t;
  int i;
  for (i = 0; i < 16; i++) w[i] = get_be32(p + 4 * i);
  for (; i < 80; i++) {
    t = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
    w[i] = ROL32(t, 1);
  }
  for (i = 0; i < 80; i++) {
    if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
    else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
    else { f = b ^ c ^ d; k = 0xca62c1d6; }
    t = ROL32(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = ROL32(b, 30);
    b = a;
    a = t;
  }
  h->h[0] += a;
  h->h[1] += b;
  h->h[2] += c;
  h->h[3] += d;
  h->h[4] += e;
}


static const uint32_t sha256_k[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_block (Hash *h, const unsigned char *p) {
  uint32_t w[64], s[8], t1, t2;
  int i;
  for (i = 0; i < 16; i++) w[i] = get_be32(p + 4 * i);
  for (; i < 64; i++) {
    uint32_t a = w[i - 15], b = w[i - 2];
    w[i] = w[i - 16] + (ROR32(a, 7) ^ ROR32(a, 18) ^ (a >> 3)) + w[i - 7] +
           (ROR32(b, 17) ^ ROR32(b, 19) ^ (b >> 10));
  }
  memcpy(s, h->h, sizeof(s));
  for (i = 0; i < 64; i++) {
    t1 = s[7] + (ROR32(s[4], 6) ^ ROR32(s[4], 11) ^ ROR32(s[4], 25)) +
         ((s[4] & s[5]) ^ (~s[4] & s[6])) + sha256_k[i] + w[i];
    t2 = (ROR32(s[0], 2) ^ ROR32(s[0], 13) ^ ROR32(s[0], 22)) +
         ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
    memmove(s + 1, s, 7 * sizeof(s[0]));
    s[4] += t1;
    s[0] = t1 + t2;
  }
  for (i = 0; i < 8; i++) h->h[i] += s[i];
}


static const uint64_t sha512_k[80] = {
  0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
  0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
  0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
  0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
  0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
  0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
  0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
  0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
  0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
  0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
  0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
  0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
  0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
  0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
  0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
  0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
  0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
  0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
  0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
  0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

static void sha512_block (Hash *h, const unsigned char *p) {
  uint64_t w[80], s[8], t1, t2;
  int i;
  for (i = 0; i < 16; i++) w[i] = get_be64(p + 8 * i);
  for (; i < 80; i++) {
    uint64_t a = w[i - 15], b = w[i - 2];
    w[i] = w[i - 16] + (ROR64(a, 1) ^ ROR64(a, 8) ^ (a >> 7)) + w[i - 7] +
           (ROR64(b, 19) ^ ROR64(b, 61) ^ (b >> 6));
  }
  memcpy(s, h->g, sizeof(s));
  for (i = 0; i < 80; i++) {
    t1 = s[7] + (ROR64(s[4], 14) ^ ROR64(s[4], 18) ^ ROR64(s[4], 41)) +
         ((s[4] & s[5]) ^ (~s[4] & s[6])) + sha512_k[i] + w[i];
    t2 = (ROR64(s[0], 28) ^ ROR64(s[0], 34) ^ ROR64(s[0], 39)) +
         ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
    memmove(s + 1, s, 7 * sizeof(s[0]));
    s[4] += t1;
    s[0] = t1 + t2;
  }
  for (i = 0; i < 8; i++) h->g[i] += s[i];
}


static void hash_init (Hash *h, int algo) {
  static const uint32_t i256[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  static const uint32_t i224[8] = {0xc1059ed8, 0x367cd507, 0x3070dd17, 0xf70e5939,
                                   0xffc00b31, 0x68581511, 0x64f98fa7, 0xbefa4fa4};
  static const uint64_t i512[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
                                   0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                                   0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
                                   0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
  static const uint64_t i384[8] = {0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL,
                                   0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
                                   0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL,
                                   0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL};
  static const uint32_t i1[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
  memset(h, 0, sizeof(*h));
  h->algo = algo;
  h->size = (algo == H_SHA384 || algo == H_SHA512) ? 128 : 64;
  switch (algo) {
    case H_MD5: case H_SHA1: memcpy(h->h, i1, sizeof(i1)); break;
    case H_SHA224: memcpy(h->h, i224, sizeof(i224)); break;
    case H_SHA256: memcpy(h->h, i256, sizeof(i256)); break;
    case H_SHA384: memcpy(h->g, i384, sizeof(i384)); break;
    default: memcpy(h->g, i512, sizeof(i512)); break;
  }
}


static void hash_block (Hash *h, const unsigned char *p) {
  switch (h->algo) {
    case H_MD5: md5_block(h, p); break;
    case H_SHA1: sha1_block(h, p); break;
    case H_SHA224: case H_SHA256: sha256_block(h, p); break;
    default: sha512_block(h, p); break;
  }
}


static void hash_update (Hash *h, const unsigned char *p, size_t n) {
  h->total += n;
  if (h->used > 0) {
    size_t take = h->size - h->used;
    if (take > n) take = n;
    memcpy(h->block + h->used, p, take);
    h->used += take;
    p += take;
    n -= take;
    if (h->used < h->size) return;
    hash_block(h, h->block);
    h->used = 0;
  }
  for (; n >= h->size; p += h->size, n -= h->size) hash_block(h, p);
  memcpy(h->block, p, n);
  h->used = n;
}


/* pads, and puts the digest in out; its length is returned */
static int hash_final (Hash *h, unsigned char *out) {
  size_t lenbytes = h->size == 128 ? 16 : 8, i;
  uint64_t bits = h->total << 3;
  int n, k;
  h->block[h->used++] = 0x80;
  if (h->used > h->size - lenbytes) {
    memset(h->block + h->used, 0, h->size - h->used);
    hash_block(h, h->block);
    h->used = 0;
  }
  memset(h->block + h->used, 0, h->size - h->used);
  for (i = 0; i < 8; i++) {
    unsigned char byte = (unsigned char)(bits >> (8 * i));
    if (h->algo == H_MD5) h->block[h->size - 8 + i] = byte;	/* little-endian */
    else h->block[h->size - 1 - i] = byte;
  }
  if (lenbytes == 16) h->block[h->size - 9] = (unsigned char)(h->total >> 61);
  hash_block(h, h->block);
  switch (h->algo) {
    case H_MD5:
      for (k = 0; k < 16; k++) out[k] = (unsigned char)(h->h[k / 4] >> (8 * (k % 4)));
      return 16;
    case H_SHA384: case H_SHA512:
      n = h->algo == H_SHA384 ? 48 : 64;
      for (k = 0; k < n; k++) out[k] = (unsigned char)(h->g[k / 8] >> (56 - 8 * (k % 8)));
      return n;
    default:
      n = h->algo == H_SHA1 ? 20 : h->algo == H_SHA224 ? 28 : 32;
      for (k = 0; k < n; k++) out[k] = (unsigned char)(h->h[k / 4] >> (24 - 8 * (k % 4)));
      return n;
  }
}

/* }================================================================== */


/*
** {==================================================================
** md5sum and sha*sum
** ===================================================================
*/

typedef struct SumRun {
  const char *tool, *tag;	/* "md5sum", "MD5" */
  int algo, hexlen;
  int in, err;
  Out *o;
  int binary;	/* -1 not said, 0 text, 1 binary */
  int tag_mode, delim, status_only, warn, quiet, strict, ignore_missing;
  int bsd_reversed;	/* -1 not known yet; checksum lines are one kind or the other */
} SumRun;


/* digest of one file; 0 failed (said why), 1 ok; *missing: not there,
** and --ignore-missing said to skip it */
static int sum_file (SumRun *s, const char *name, int *binary, unsigned char *dg, int *missing) {
  static unsigned char buf[CHUNK];
  In r;
  Hash h;
  long n;
  int got;
  *missing = 0;
  if (strcmp(name, "-") == 0) {
    if (BINARY_DEFAULT && *binary < 0) *binary = !os_is_tty(s->in);
  }
  got = open_in(&r, s->tool, name, s->in, s->err, s->o, s->ignore_missing);
  if (got == -2) {
    *missing = 1;
    return 1;
  }
  if (got != 0) return 0;
  hash_init(&h, s->algo);
  while ((n = in_read(&r, (char *)buf, sizeof(buf))) > 0 && !tool_stop())
    hash_update(&h, buf, (size_t)n);
  in_close(&r);
  hash_final(&h, dg);
  return 1;
}


/* the name, with \n and \ escaped if asked */
static void put_name (Out *o, const char *name, int escape) {
  if (!escape) {
    out_puts(o, name);
    return;
  }
  for (; *name; name++) {
    if (*name == '\n') out_puts(o, "\\n");
    else if (*name == '\\') out_puts(o, "\\\\");
    else out_putc(o, *name);
  }
}


static int lower (int c) {
  return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}


static int is_hex (int c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}


/* exactly hexlen hex digits, then the end */
static int hex_digits (SumRun *s, const char *p) {
  int i;
  for (i = 0; i < s->hexlen; i++)
    if (!is_hex((unsigned char)p[i])) return 0;
  return p[s->hexlen] == '\0';
}


/* undoes \\ and \n in place; NULL: not a valid escaped name */
static char *name_unescape (char *s, size_t n) {
  char *dst = s;
  size_t i;
  for (i = 0; i < n; i++) {
    if (s[i] == '\\') {
      if (i == n - 1) return NULL;
      i++;
      if (s[i] == 'n') *dst++ = '\n';
      else if (s[i] == '\\') *dst++ = '\\';
      else return NULL;
    }
    else if (s[i] == '\0') return NULL;
    else *dst++ = s[i];
  }
  if (dst < s + n) *dst = '\0';
  return s;
}


#define ISWHITE(c)	((c) == ' ' || (c) == '\t')

/* "TAG (name) = hex", after the "(" */
static int bsd_split (char *s, size_t n, char **hex, char **name, int escaped) {
  size_t i;
  if (n == 0) return 0;
  i = n - 1;
  while (i && s[i] != ')') i--;
  if (s[i] != ')') return 0;
  *name = s;
  if (escaped && name_unescape(s, i) == NULL) return 0;
  s[i++] = '\0';
  while (ISWHITE(s[i])) i++;
  if (s[i] != '=') return 0;
  i++;
  while (ISWHITE(s[i])) i++;
  *hex = s + i;
  return 1;
}


/* a checksum line: "hex  name", "hex *name", "TAG (name) = hex", "hex name" */
static int split_line (SumRun *s, char *line, size_t n, char **hex, int *binary, char **name) {
  int escaped = 0;
  size_t i = 0, taglen = strlen(s->tag);
  while (ISWHITE(line[i])) i++;
  if (line[i] == '\\') {
    i++;
    escaped = 1;
  }
  if (strncmp(line + i, s->tag, taglen) == 0) {
    i += taglen;
    if (line[i] == ' ') i++;
    if (line[i] == '(') {
      i++;
      *binary = 0;
      return bsd_split(line + i, n - i, hex, name, escaped);
    }
    return 0;
  }
  if (n - i < (size_t)s->hexlen + 2 + (line[i] == '\\')) return 0;
  *hex = line + i;
  i += (size_t)s->hexlen;
  if (!ISWHITE(line[i])) return 0;
  line[i++] = '\0';
  if (!hex_digits(s, *hex)) return 0;
  if (n - i == 1 || (line[i] != ' ' && line[i] != '*')) {	/* BSD reversed: "hex name" */
    if (s->bsd_reversed == 0) return 0;
    s->bsd_reversed = 1;
  }
  else if (s->bsd_reversed != 1) {
    s->bsd_reversed = 0;
    *binary = (line[i++] == '*');
  }
  *name = line + i;
  if (escaped) return name_unescape(line + i, n - i) != NULL;
  return 1;
}


static void warn_count (SumRun *s, unsigned long long n, const char *one, const char *many) {
  char msg[256];
  snprintf(msg, sizeof(msg), n == 1 ? one : many, n);
  tool_err(s->err, s->tool, "WARNING: %s", msg);
}


/* -c: checks the sums listed in one file; 1 all good */
static int sum_check (SumRun *s, const char *cname) {
  static const char hexd[] = "0123456789abcdef";
  unsigned long long misformatted = 0, mismatched = 0, failures = 0, lineno = 0;
  int properly = 0, matched = 0, is_stdin = strcmp(cname, "-") == 0;
  const char *shown = is_stdin ? "standard input" : cname;
  unsigned char dg[64];
  char *line;
  size_t len;
  In r;
  if (!is_stdin) {
    char *native = path_to_native(cname);
    OsStat st;
    int isdir = os_stat(native, &st) == 0 && st.is_dir;
    free(native);
    if (isdir) {
      out_flush(s->o);
      tool_err(s->err, s->tool, "%s: read error", qf(cname));
      return 0;
    }
  }
  if (open_in(&r, s->tool, cname, s->in, s->err, s->o, 0) != 0) return 0;
  while (in_line(&r, &line, &len, '\n', NULL) && !tool_stop()) {
    char *hex = NULL, *name = NULL;
    int binary = 0, ok, missing;
    lineno++;
    if (line[0] == '#') continue;
    if (len == 0) continue;
    line[len] = '\0';
    if (!(split_line(s, line, len, &hex, &binary, &name) && !(is_stdin && strcmp(name, "-") == 0) &&
          hex_digits(s, hex))) {
      misformatted++;
      if (s->warn) {
        out_flush(s->o);
        tool_err(s->err, s->tool, "%s: %llu: improperly formatted %s checksum line", qf(shown),
                 lineno, s->tag);
      }
      continue;
    }
    {
      int escape = !s->status_only && strchr(name, '\n') != NULL;
      properly = 1;
      ok = sum_file(s, name, &binary, dg, &missing);
      if (!ok) {
        failures++;
        if (!s->status_only) {
          if (escape) out_putc(s->o, '\\');
          put_name(s->o, name, escape);
          out_puts(s->o, ": FAILED open or read\n");
        }
      }
      else if (!(s->ignore_missing && missing)) {
        int k, nb = s->hexlen / 2;
        for (k = 0; k < nb; k++)
          if (lower(hex[2 * k]) != hexd[dg[k] >> 4] ||
              lower(hex[2 * k + 1]) != hexd[dg[k] & 15]) break;
        if (k != nb) mismatched++;
        else matched = 1;
        if (!s->status_only) {
          if (k != nb || !s->quiet) {
            if (escape) out_putc(s->o, '\\');
            put_name(s->o, name, escape);
          }
          if (k != nb) out_puts(s->o, ": FAILED\n");
          else if (!s->quiet) out_puts(s->o, ": OK\n");
        }
      }
    }
  }
  in_close(&r);
  out_flush(s->o);
  if (!properly)
    tool_err(s->err, s->tool, "%s: no properly formatted %s checksum lines found", qf(shown), s->tag);
  else if (!s->status_only) {
    if (misformatted)
      warn_count(s, misformatted, "%llu line is improperly formatted",
                 "%llu lines are improperly formatted");
    if (failures)
      warn_count(s, failures, "%llu listed file could not be read",
                 "%llu listed files could not be read");
    if (mismatched)
      warn_count(s, mismatched, "%llu computed checksum did NOT match",
                 "%llu computed checksums did NOT match");
    if (s->ignore_missing && !matched) tool_err(s->err, s->tool, "%s: no file was verified", qf(shown));
  }
  return properly && !mismatched && !failures && (!s->strict || misformatted == 0) &&
         (!s->ignore_missing || matched);
}


static int sum_main (int algo, int argc, char **argv, int in, int out, int err) {
  static const char *const tools[] = {"md5sum", "sha1sum", "sha224sum", "sha256sum", "sha384sum", "sha512sum"};
  static const char *const tags[] = {"MD5", "SHA1", "SHA224", "SHA256", "SHA384", "SHA512"};
  static const int hexlens[] = {32, 40, 56, 64, 96, 128};
  enum { O_QUIET = 1000, O_STATUS, O_STRICT, O_TAG, O_IGNORE };
  LongOpt lo[] = {{"binary", 'b', 0}, {"check", 'c', 0}, {"ignore-missing", O_IGNORE, 0},
                  {"quiet", O_QUIET, 0}, {"status", O_STATUS, 0}, {"text", 't', 0},
                  {"warn", 'w', 0}, {"strict", O_STRICT, 0}, {"tag", O_TAG, 0},
                  {"zero", 'z', 0}, {"version", OPT_VERSION, 0}, {NULL, 0, 0}};
  SumRun s;
  Opts g;
  Out o;
  int c, check = 0, ok = 1;
  const char *bad = NULL;
  size_t i;
  memset(&s, 0, sizeof(s));
  s.tool = tools[algo];
  s.tag = tags[algo];
  s.algo = algo;
  s.hexlen = hexlens[algo];
  s.in = in;
  s.err = err;
  s.o = &o;
  s.binary = -1;
  s.delim = '\n';
  s.bsd_reversed = -1;
  opts_init(&g, s.tool, argc, argv, err);
  while ((c = opts_next(&g, "bctwz", lo)) != 0) {
    switch (c) {
      case 'b': s.binary = 1; break;
      case 'c': check = 1; break;
      case 't': s.binary = 0; break;
      case 'z': s.delim = '\0'; break;
      case 'w': s.status_only = 0; s.warn = 1; s.quiet = 0; break;
      case O_STATUS: s.status_only = 1; s.warn = 0; s.quiet = 0; break;
      case O_QUIET: s.status_only = 0; s.warn = 0; s.quiet = 1; break;
      case O_STRICT: s.strict = 1; break;
      case O_IGNORE: s.ignore_missing = 1; break;
      case O_TAG: s.tag_mode = 1; s.binary = 1; break;
      case OPT_HELP: opts_free(&g); return tool_help(out, s.tool);
      case OPT_VERSION: opts_free(&g); return tool_version(out, s.tool);
      default: opts_free(&g); return 1;
    }
  }
  if (s.tag_mode && !s.binary) bad = "--tag does not support --text mode";
  else if (s.delim != '\n' && check) bad = "the --zero option is not supported when verifying checksums";
  else if (s.tag_mode && check) bad = "the --tag option is meaningless when verifying checksums";
  else if (s.binary >= 0 && check)
    bad = "the --binary and --text options are meaningless when verifying checksums";
  else if (s.ignore_missing && !check)
    bad = "the --ignore-missing option is meaningful only when verifying checksums";
  else if (s.status_only && !check) bad = "the --status option is meaningful only when verifying checksums";
  else if (s.warn && !check) bad = "the --warn option is meaningful only when verifying checksums";
  else if (s.quiet && !check) bad = "the --quiet option is meaningful only when verifying checksums";
  else if (s.strict && !check) bad = "the --strict option is meaningful only when verifying checksums";
  if (bad != NULL) {
    tool_err(err, s.tool, "%s", bad);
    opts_free(&g);
    return try_help(err, s.tool);
  }
  if (!BINARY_DEFAULT && s.binary < 0) s.binary = 0;
  if (g.ops.n == 0) vec_push(&g.ops, xstrdup("-"));
  out_init(&o, out);
  for (i = 0; i < g.ops.n && !tool_stop(); i++) {
    const char *name = g.ops.v[i];
    if (check) ok &= sum_check(&s, name);
    else {
      unsigned char dg[64];
      int binary = s.binary, missing, k, escape;
      if (!sum_file(&s, name, &binary, dg, &missing)) {
        ok = 0;
        continue;
      }
      escape = (strchr(name, '\\') != NULL || strchr(name, '\n') != NULL) && s.delim == '\n';
      if (escape) out_putc(&o, '\\');
      if (s.tag_mode) {
        out_printf(&o, "%s (", s.tag);
        put_name(&o, name, escape);
        out_puts(&o, ") = ");
      }
      for (k = 0; k < s.hexlen / 2; k++) out_printf(&o, "%02x", dg[k]);
      if (!s.tag_mode) {
        out_putc(&o, ' ');
        out_putc(&o, binary ? '*' : ' ');
        put_name(&o, name, escape);
      }
      out_putc(&o, s.delim);
    }
  }
  if (out_flush(&o) < 0) ok = 0;
  opts_free(&g);
  return tool_stop() ? 130 : ok ? 0 : 1;
}


int t_md5sum (int argc, char **argv, int in, int out, int err) {
  return sum_main(H_MD5, argc, argv, in, out, err);
}

int t_sha1sum (int argc, char **argv, int in, int out, int err) {
  return sum_main(H_SHA1, argc, argv, in, out, err);
}

int t_sha224sum (int argc, char **argv, int in, int out, int err) {
  return sum_main(H_SHA224, argc, argv, in, out, err);
}

int t_sha256sum (int argc, char **argv, int in, int out, int err) {
  return sum_main(H_SHA256, argc, argv, in, out, err);
}

int t_sha384sum (int argc, char **argv, int in, int out, int err) {
  return sum_main(H_SHA384, argc, argv, in, out, err);
}

int t_sha512sum (int argc, char **argv, int in, int out, int err) {
  return sum_main(H_SHA512, argc, argv, in, out, err);
}

/* }================================================================== */


/*
** {==================================================================
** cksum and sum
** ===================================================================
*/

/* POSIX cksum: CRC-32 MSB first (polynomial 0x04C11DB7), then the length */
static uint32_t crc_tab[256];

static uint32_t crc_add (uint32_t crc, const unsigned char *p, size_t n) {
  if (crc_tab[1] == 0) {
    uint32_t i, c;
    int k;
    for (i = 0; i < 256; i++) {
      c = i << 24;
      for (k = 0; k < 8; k++) c = (c & 0x80000000UL) ? (c << 1) ^ 0x04C11DB7UL : c << 1;
      crc_tab[i] = c;
    }
  }
  while (n--) crc = (crc << 8) ^ crc_tab[((crc >> 24) ^ *p++) & 0xFF];
  return crc;
}


/* "tool [FILE]...": the options are only --help, --version (and sum's) */
int t_cksum (int argc, char **argv, int in, int out, int err) {
  static unsigned char buf[CHUNK];
  LongOpt lo[] = {{"version", OPT_VERSION, 0}, {NULL, 0, 0}};
  Opts g;
  Out o;
  int c, ok = 1, named = 1;
  size_t i;
  opts_init(&g, "cksum", argc, argv, err);
  while ((c = opts_next(&g, "", lo)) != 0) {
    if (c == OPT_HELP) { opts_free(&g); return tool_help(out, "cksum"); }
    if (c == OPT_VERSION) { opts_free(&g); return tool_version(out, "cksum"); }
    opts_free(&g);
    return 1;
  }
  if (g.ops.n == 0) {
    vec_push(&g.ops, xstrdup("-"));
    named = 0;
  }
  out_init(&o, out);
  for (i = 0; i < g.ops.n && !tool_stop(); i++) {
    uint32_t crc = 0;
    uint64_t total = 0, n;
    unsigned char lb;
    long k;
    In r;
    if (open_in(&r, "cksum", g.ops.v[i], in, err, &o, 0) != 0) {
      ok = 0;
      continue;
    }
    while ((k = in_read(&r, (char *)buf, sizeof(buf))) > 0 && !tool_stop()) {
      crc = crc_add(crc, buf, (size_t)k);
      total += (uint64_t)k;
    }
    in_close(&r);
    for (n = total; n != 0; n >>= 8) {
      lb = (unsigned char)(n & 0xFF);
      crc = crc_add(crc, &lb, 1);
    }
    crc = ~crc & 0xFFFFFFFFUL;
    out_printf(&o, "%lu %llu", (unsigned long)crc, (unsigned long long)total);
    if (named) out_printf(&o, " %s", g.ops.v[i]);
    out_putc(&o, '\n');
  }
  if (out_flush(&o) < 0) ok = 0;
  opts_free(&g);
  return tool_stop() ? 130 : ok ? 0 : 1;
}


/* sum -r (BSD: rotating 16 bits, 1K blocks), sum -s (System V: 512) */
int t_sum (int argc, char **argv, int in, int out, int err) {
  static unsigned char buf[CHUNK];
  LongOpt lo[] = {{"sysv", 's', 0}, {"version", OPT_VERSION, 0}, {NULL, 0, 0}};
  Opts g;
  Out o;
  int c, ok = 1, sysv = 0;
  size_t i, files;
  opts_init(&g, "sum", argc, argv, err);
  while ((c = opts_next(&g, "rs", lo)) != 0) {
    if (c == 'r') sysv = 0;
    else if (c == 's') sysv = 1;
    else if (c == OPT_HELP) { opts_free(&g); return tool_help(out, "sum"); }
    else if (c == OPT_VERSION) { opts_free(&g); return tool_version(out, "sum"); }
    else { opts_free(&g); return 1; }
  }
  files = g.ops.n;
  if (files == 0) vec_push(&g.ops, xstrdup("-"));
  out_init(&o, out);
  for (i = 0; i < g.ops.n && !tool_stop(); i++) {
    uint64_t total = 0;
    uint32_t s = 0;	/* System V: the sum of the bytes, mod 2^32 */
    unsigned int bsd = 0;
    long k, j;
    In r;
    if (open_in(&r, "sum", g.ops.v[i], in, err, &o, 0) != 0) {
      ok = 0;
      continue;
    }
    while ((k = in_read(&r, (char *)buf, sizeof(buf))) > 0 && !tool_stop()) {
      total += (uint64_t)k;
      if (sysv)
        for (j = 0; j < k; j++) s += buf[j];
      else
        for (j = 0; j < k; j++) {
          bsd = (bsd >> 1) + ((bsd & 1) << 15);
          bsd = (bsd + buf[j]) & 0xffff;
        }
    }
    in_close(&r);
    if (sysv) {
      uint32_t t = (s & 0xffff) + (s >> 16);
      t = (t & 0xffff) + (t >> 16);
      out_printf(&o, "%lu %llu", (unsigned long)t, (unsigned long long)((total + 511) / 512));
      if (files > 0) out_printf(&o, " %s", g.ops.v[i]);
    }
    else {
      out_printf(&o, "%05u %5llu", bsd, (unsigned long long)((total + 1023) / 1024));
      if (files > 1) out_printf(&o, " %s", g.ops.v[i]);
    }
    out_putc(&o, '\n');
  }
  if (out_flush(&o) < 0) ok = 0;
  opts_free(&g);
  return tool_stop() ? 130 : ok ? 0 : 1;
}

/* }================================================================== */


/*
** {==================================================================
** base64 and base32
** ===================================================================
*/

static const char b64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char b32_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

/* the value of an alphabet character, -1 for others */
static int b64_val (int c) {
  const char *p = c ? strchr(b64_chars, c) : NULL;
  return p ? (int)(p - b64_chars) : -1;
}

static int b32_val (int c) {
  const char *p = c ? strchr(b32_chars, c) : NULL;
  return p ? (int)(p - b32_chars) : -1;
}


typedef struct Dec {	/* gnulib's base64_decode_context / base32_decode_context */
  int q;	/* 4 or 8 characters a quantum */
  int (*val) (int c);
  int (*quantum) (const unsigned char *in, size_t inlen, unsigned char **outp, size_t *outleft);
  size_t i;
  unsigned char buf[8];
} Dec;

#define PUT(v)	do { if (*outleft) { *out++ = (unsigned char)(v); --*outleft; } } while (0)
#define FAIL	do { *outp = out; return 0; } while (0)

static int dec_4 (const unsigned char *in, size_t inlen, unsigned char **outp, size_t *outleft) {
  unsigned char *out = *outp;
  int v0, v1, v2, v3;
  if (inlen < 2) return 0;
  v0 = b64_val(in[0]);
  v1 = b64_val(in[1]);
  if (v0 < 0 || v1 < 0) return 0;
  PUT((v0 << 2) | (v1 >> 4));
  if (inlen == 2) FAIL;
  if (in[2] == '=') {
    if (inlen != 4 || in[3] != '=') FAIL;
  }
  else {
    if ((v2 = b64_val(in[2])) < 0) FAIL;
    PUT(((v1 << 4) & 0xf0) | (v2 >> 2));
    if (inlen == 3) FAIL;
    if (in[3] == '=') {
      if (inlen != 4) FAIL;
    }
    else {
      if ((v3 = b64_val(in[3])) < 0) FAIL;
      PUT(((v2 << 6) & 0xc0) | v3);
    }
  }
  *outp = out;
  return 1;
}


static int dec_8 (const unsigned char *in, size_t inlen, unsigned char **outp, size_t *outleft) {
  unsigned char *out = *outp;
  int v[8], k;
  if (inlen < 8) return 0;
  for (k = 0; k < 8; k++) v[k] = b32_val(in[k]);
  if (v[0] < 0 || v[1] < 0) return 0;
  PUT((v[0] << 3) | (v[1] >> 2));
  if (in[2] == '=') {
    for (k = 3; k < 8; k++)
      if (in[k] != '=') FAIL;
  }
  else {
    if (v[2] < 0 || v[3] < 0) FAIL;
    PUT((v[1] << 6) | (v[2] << 1) | (v[3] >> 4));
    if (in[4] == '=') {
      for (k = 5; k < 8; k++)
        if (in[k] != '=') FAIL;
    }
    else {
      if (v[4] < 0) FAIL;
      PUT((v[3] << 4) | (v[4] >> 1));
      if (in[5] == '=') {
        if (in[6] != '=' || in[7] != '=') FAIL;
      }
      else {
        if (v[5] < 0 || v[6] < 0) FAIL;
        PUT((v[4] << 7) | (v[5] << 2) | (v[6] >> 3));
        if (in[7] != '=') {
          if (v[7] < 0) FAIL;
          PUT((v[6] << 5) | v[7]);
        }
      }
    }
  }
  *outp = out;
  return 1;
}

#undef PUT
#undef FAIL


/* the next quantum, newlines left out (gnulib's get_4 / get_8) */
static const unsigned char *dec_get (Dec *d, const unsigned char **in, const unsigned char *end,
                                     size_t *n) {
  const unsigned char *p;
  if (d->i == (size_t)d->q) d->i = 0;
  if (d->i == 0) {
    const unsigned char *t = *in;
    if (end - *in >= d->q && memchr(t, '\n', (size_t)d->q) == NULL) {
      *in += d->q;
      *n = (size_t)d->q;
      return t;
    }
  }
  for (p = *in; p < end;) {
    unsigned char c = *p++;
    if (c != '\n') {
      d->buf[d->i++] = c;
      if (d->i == (size_t)d->q) break;
    }
  }
  *in = p;
  *n = d->i;
  return d->buf;
}


/* gnulib's base64_decode_ctx: inlen 0 flushes; 0 means invalid input */
static int dec_ctx (Dec *d, const unsigned char *in, size_t inlen, unsigned char *out, size_t *outlen) {
  size_t outleft = *outlen, q = (size_t)d->q;
  int flush = inlen == 0;
  size_t ctx_i = d->i;
  for (;;) {
    size_t save = outleft;
    if (ctx_i == 0 && !flush) {
      for (;;) {
        save = outleft;
        if (!d->quantum(in, inlen, &out, &outleft)) break;
        in += q;
        inlen -= q;
      }
    }
    if (inlen == 0 && !flush) break;
    if (inlen && *in == '\n') {	/* lines wrapped at a multiple of the quantum */
      ++in;
      --inlen;
      continue;
    }
    out -= save - outleft;	/* undo what a partial quantum wrote */
    outleft = save;
    {
      const unsigned char *end = in + inlen, *non_nl = dec_get(d, &in, end, &inlen);
      if (inlen == 0 || (inlen < q && !flush)) {
        inlen = 0;
        break;
      }
      if (!d->quantum(non_nl, inlen, &out, &outleft)) break;
      inlen = (size_t)(end - in);
    }
  }
  *outlen -= outleft;
  return inlen == 0;
}


static int base_main (int b32, int argc, char **argv, int in, int out, int err) {
  const char *tool = b32 ? "base32" : "base64";
  LongOpt lo[] = {{"decode", 'd', 0}, {"ignore-garbage", 'i', 0}, {"wrap", 'w', 1},
                  {"version", OPT_VERSION, 0}, {NULL, 0, 0}};
  int c, decode = 0, garbage = 0, eof = 0, status = 0;
  long long wrap = 76;
  const char *name = "-";
  unsigned char *ibuf, *obuf;
  Opts g;
  Out o;
  In r;
  opts_init(&g, tool, argc, argv, err);
  while ((c = opts_next(&g, "diw:", lo)) != 0) {
    if (c == 'd') decode = 1;
    else if (c == 'i') garbage = 1;
    else if (c == 'w') {
      const char *a = g.arg;
      char *end;
      long long w;
      while (*a == ' ' || *a == '\t') a++;
      errno = 0;
      w = strtoll(a, &end, 10);
      if (end == a || *end != '\0' || w < 0 || errno == ERANGE) {
        if (errno == ERANGE && w > 0)
          tool_err(err, tool, "invalid wrap size: '%s': Value too large for defined data type", g.arg);
        else tool_err(err, tool, "invalid wrap size: '%s'", g.arg);
        opts_free(&g);
        return 1;
      }
      wrap = w;
    }
    else if (c == OPT_HELP) { opts_free(&g); return tool_help(out, tool); }
    else if (c == OPT_VERSION) { opts_free(&g); return tool_version(out, tool); }
    else { opts_free(&g); return 1; }
  }
  if (g.ops.n > 1) {
    tool_err(err, tool, "extra operand '%s'", g.ops.v[1]);
    opts_free(&g);
    return try_help(err, tool);
  }
  if (g.ops.n == 1) name = g.ops.v[0];
  if (strcmp(name, "-") != 0) {
    char *native = path_to_native(name);
    OsStat st;
    int isdir = os_stat(native, &st) == 0 && st.is_dir;
    free(native);
    if (isdir) {
      tool_err(err, tool, "read error: Is a directory");
      opts_free(&g);
      return 1;
    }
  }
  if (open_in(&r, tool, name, in, err, NULL, 0) != 0) {
    opts_free(&g);
    return 1;
  }
  out_init(&o, out);
  if (!decode) {	/* 30720 bytes at a time: a whole number of 3 and 5 byte groups */
    const size_t blk = 30720;
    const char *chars = b32 ? b32_chars : b64_chars;
    long long col = 0;
    size_t n, k, w, at;
    ibuf = (unsigned char *)xmalloc(blk);
    obuf = (unsigned char *)xmalloc(blk * 2);
    do {
      n = read_full(&r, ibuf, blk, &eof);
      if (n == 0) break;
      w = 0;
      if (!b32)
        for (k = 0; k < n; k += 3) {
          unsigned long v = (unsigned long)ibuf[k] << 16;
          if (k + 1 < n) v |= (unsigned long)ibuf[k + 1] << 8;
          if (k + 2 < n) v |= ibuf[k + 2];
          obuf[w++] = (unsigned char)chars[(v >> 18) & 63];
          obuf[w++] = (unsigned char)chars[(v >> 12) & 63];
          obuf[w++] = (unsigned char)(k + 1 < n ? chars[(v >> 6) & 63] : '=');
          obuf[w++] = (unsigned char)(k + 2 < n ? chars[v & 63] : '=');
        }
      else
        for (k = 0; k < n; k += 5) {
          size_t have = n - k < 5 ? n - k : 5, j;
          static const int outc[6] = {0, 2, 4, 5, 7, 8};	/* characters for 0..5 bytes */
          uint64_t v = 0;
          for (j = 0; j < 5; j++) v = (v << 8) | (j < have ? ibuf[k + j] : 0);
          for (j = 0; j < 8; j++)
            obuf[w++] = (unsigned char)((int)j < outc[have] ? chars[(v >> (35 - 5 * j)) & 31] : '=');
        }
      for (at = 0; at < w;) {	/* wrap_write */
        size_t left = w - at, take;
        if (wrap == 0) {
          out_putn(&o, (const char *)obuf + at, left);
          break;
        }
        take = (unsigned long long)(wrap - col) < left ? (size_t)(wrap - col) : left;
        if (take == 0) {
          out_putc(&o, '\n');
          col = 0;
        }
        else {
          out_putn(&o, (const char *)obuf + at, take);
          col += (long long)take;
          at += take;
        }
      }
    } while (!eof && !o.failed && !tool_stop());
    if (wrap > 0 && col > 0) out_putc(&o, '\n');
  }
  else {	/* gnulib's way, block for block: 4096 / 8192 characters at a time */
    const size_t cap = b32 ? 8192 : 4096, ocap = b32 ? 5120 : 3072;
    Dec d;
    size_t sum, n, k;
    int pass;
    memset(&d, 0, sizeof(d));
    d.q = b32 ? 8 : 4;
    d.val = b32 ? b32_val : b64_val;
    d.quantum = b32 ? dec_8 : dec_4;
    ibuf = (unsigned char *)xmalloc(cap);
    obuf = (unsigned char *)xmalloc(ocap);
    do {
      sum = 0;
      do {
        n = read_full(&r, ibuf + sum, cap - sum, &eof);
        if (garbage)
          for (k = 0; k < n;) {
            if (d.val(ibuf[sum + k]) >= 0 || ibuf[sum + k] == '=') k++;
            else {
              n--;
              memmove(ibuf + sum + k, ibuf + sum + k + 1, n - k);
            }
          }
        sum += n;
      } while (sum < cap && !eof);
      for (pass = 0; pass < 1 + eof; pass++) {
        size_t got = ocap;
        int ok;
        if (pass == 1 && d.i == 0) break;
        ok = dec_ctx(&d, ibuf, pass == 0 ? sum : 0, obuf, &got);
        out_putn(&o, (const char *)obuf, got);
        if (!ok) {
          out_flush(&o);
          tool_err(err, tool, "invalid input");
          status = 1;
          eof = 1;
          break;
        }
      }
    } while (!eof && !tool_stop());
  }
  free(ibuf);
  free(obuf);
  in_close(&r);
  if (out_flush(&o) < 0 && status == 0) status = 1;
  opts_free(&g);
  return tool_stop() ? 130 : status;
}


int t_base64 (int argc, char **argv, int in, int out, int err) {
  return base_main(0, argc, argv, in, out, err);
}

int t_base32 (int argc, char **argv, int in, int out, int err) {
  return base_main(1, argc, argv, in, out, err);
}

/* }================================================================== */
