/* NotaTV phonebox (S6, audit 9/11): opens what the phone page sends encrypted.
 *   phonebox open <keyfile>    stdin: base64 of nonce (24 bytes) + secretbox (XSalsa20-Poly1305), as made by
 *                              nacl.secretbox in the page (tweetnacl-js). stdout: the plain text. Exit 1 if the
 *                              box does not open (wrong key, changed on the way, damaged).
 *   phonebox newkey <keyfile>  a new random 32-byte key (0600, replaced atomically); prints it as base64url, for the
 *                              QR code's #k= (the part after # is never sent over the network).
 * The key is made by the TV and only shown as a QR code on its screen: whoever can open the box had the TV's QR
 * code, so the box is also the login. Runs as root (phone-agent); the key file is root-only. */
#include "vendor/tweetnacl.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_IN (1024 * 1024) /* the page sends at most 512 KB of form data; base64 + box on top */

void randombytes(unsigned char *x, unsigned long long n); /* tweetnacl wants it (only for keys made inside it) */
void randombytes(unsigned char *x, unsigned long long n) {
    while (n) {
        ssize_t r = getrandom(x, n > 256 ? 256 : (size_t)n, 0);
        if (r <= 0) abort();
        x += r;
        n -= (unsigned long long)r;
    }
}

static int b64val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}
/* standard or url alphabet, padding and whitespace ignored; -1 on a bad character */
static long b64dec(const char *s, size_t n, unsigned char *out) {
    unsigned v = 0;
    int bits = 0;
    long o = 0;
    for (size_t i = 0; i < n; i++) {
        int c = (unsigned char)s[i];
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int d = b64val(c);
        if (d < 0) return -1;
        v = (v << 6) | (unsigned)d;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = (unsigned char)(v >> bits); }
    }
    return o;
}

static int read_key(const char *path, unsigned char k[32]) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, k, 32);
    close(fd);
    return r == 32 ? 0 : -1;
}

static int cmd_open(const char *keyfile) {
    unsigned char key[32];
    if (read_key(keyfile, key)) { fprintf(stderr, "no key\n"); return 1; }
    char *in = malloc(MAX_IN + 1);
    size_t n = 0;
    ssize_t r;
    while (in && n < MAX_IN && (r = read(0, in + n, MAX_IN - n)) > 0) n += (size_t)r;
    if (!in || n == 0 || n >= MAX_IN) { fprintf(stderr, "bad input\n"); return 1; }
    unsigned char *raw = malloc(n);
    long rn = raw ? b64dec(in, n, raw) : -1;
    free(in);
    /* nonce + at least the 16-byte tag */
    if (rn < crypto_secretbox_NONCEBYTES + crypto_secretbox_BOXZEROBYTES) { fprintf(stderr, "bad input\n"); return 1; }
    size_t blen = (size_t)rn - crypto_secretbox_NONCEBYTES;
    /* tweetnacl's C API wants 16 zero bytes before the box and gives 32 zero bytes before the message */
    size_t clen = blen + crypto_secretbox_BOXZEROBYTES;
    unsigned char *c = calloc(1, clen), *m = calloc(1, clen);
    if (!c || !m) return 1;
    memcpy(c + crypto_secretbox_BOXZEROBYTES, raw + crypto_secretbox_NONCEBYTES, blen);
    int bad = crypto_secretbox_open(m, c, clen, raw, key);
    memset(key, 0, sizeof key);
    if (bad || clen < crypto_secretbox_ZEROBYTES) { fprintf(stderr, "does not open\n"); return 1; }
    fwrite(m + crypto_secretbox_ZEROBYTES, 1, clen - crypto_secretbox_ZEROBYTES, stdout);
    memset(m, 0, clen);
    return fflush(stdout) ? 1 : 0;
}

static int cmd_newkey(const char *keyfile) {
    unsigned char k[32];
    randombytes(k, sizeof k);
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.new", keyfile);
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0 || write(fd, k, sizeof k) != (ssize_t)sizeof k || fsync(fd) || close(fd) || rename(tmp, keyfile)) {
        fprintf(stderr, "cannot write the key\n");
        return 1;
    }
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    unsigned v = 0;
    int bits = 0;
    for (size_t i = 0; i < sizeof k; i++) {
        v = (v << 8) | k[i];
        bits += 8;
        while (bits >= 6) { bits -= 6; putchar(a[(v >> bits) & 63]); }
    }
    if (bits) putchar(a[(v << (6 - bits)) & 63]);
    putchar('\n');
    memset(k, 0, sizeof k);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "open")) return cmd_open(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "newkey")) return cmd_newkey(argv[2]);
    fprintf(stderr, "usage: phonebox open|newkey <keyfile>\n");
    return 2;
}
