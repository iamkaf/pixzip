/* encode.c: the encoder (FORMAT.md), held to pixzip.py's. It needs the C
   library, for malloc. */

#include <stdlib.h>
#include <string.h>

#include "pixzip.h"

struct coder {
    uint64_t low;
    uint32_t range, cache, pending;
    uint8_t *out;
    size_t n, cap;
    int failed;
};

static void put(struct coder *c, uint32_t byte)
{
    if (c->n == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 4096;
        uint8_t *out = realloc(c->out, cap);

        if (!out) {
            c->failed = 1;
            return;
        }
        c->out = out;
        c->cap = cap;
    }
    c->out[c->n++] = (uint8_t)byte;
}

/* The top byte of low leaves; a run of 0xff bytes waits until a carry
   into it is settled. */
static void shift(struct coder *c)
{
    if (c->low < 0xff000000u || c->low >> 32) {
        uint32_t carry = (uint32_t)(c->low >> 32), t = c->cache;

        do {
            put(c, t + carry);
            t = 0xff;
        } while (--c->pending);
        c->cache = (uint32_t)(c->low >> 24) & 0xff;
    }
    c->pending++;
    c->low = (c->low & 0x00ffffffu) << 8;
}

static void bit(struct coder *c, uint16_t *q, int b)
{
    uint32_t bound = (c->range >> 11) * *q;

    if (b) {
        c->low += bound;
        c->range -= bound;
        *q -= *q >> 4;
    } else {
        c->range = bound;
        *q += (2048 - *q) >> 4;
    }
    while (c->range < 1u << 24) {
        c->range <<= 8;
        shift(c);
    }
}

uint8_t *pixzip_encode(const uint8_t *px, uint32_t w, uint32_t h, uint32_t k, int reversed, uint32_t *bytes)
{
    uint32_t size = pixzip_scratch(k), n = w * h, bits = 0, x, y, i, j, m, v, cw, cn, nw, ne, pat;
    struct coder c = { 0, 0xffffffffu, 0, 1, 0, 0, 0, 0 };
    uint16_t *prob, *tree;
    uint8_t *p;

    if (!size || w - 1 > 65534 || h - 1 > 65534)
        return 0;
    for (i = 0; i < n; i++)
        if (px[i] >= k)
            return 0;
    while (1u << bits < k)
        bits++;
    prob = malloc(size * sizeof *prob);
    p = malloc(n);
    if (!prob || !p) {
        free(prob);
        free(p);
        return 0;
    }
    for (i = 0; i < n; i++)
        p[i] = px[reversed ? n - 1 - i : i];
    for (i = 0; i < size; i++)
        prob[i] = i - 128 < k ? 1536 : 1024;
    tree = prob + 128 + k;
    for (y = 0, i = 0; y < h; y++)
        for (x = 0; x < w; x++, i++) {
            cw = x ? p[i - 1] : y ? p[i - w] : 0;
            cn = y ? p[i - w] : cw;
            nw = x && y ? p[i - w - 1] : cn;
            ne = y && x + 1 < w ? p[i - w + 1] : cn;
            pat = (cw == cn) | (cw == nw) << 1 | (cn == ne) << 2 | (cn == nw) << 3 | (cw == ne) << 4
                | (x & 1) << 5;
            v = p[i];
            bit(&c, prob + pat, v == cw);
            if (v == cw)
                continue;
            if (cn != cw) {
                bit(&c, prob + 64 + pat, v == cn);
                if (v == cn)
                    continue;
            }
            if (ne != cw && ne != cn) {
                bit(&c, prob + 128 + cn, v == ne);
                if (v == ne)
                    continue;
            }
            for (m = 1, j = bits; j--; m = m * 2 + (v >> j & 1))
                bit(&c, tree + (cn << bits) + m, v >> j & 1);
        }
    for (i = 0; i < 5; i++)
        shift(&c);
    free(prob);
    free(p);
    if (c.failed || c.n > 0xffffffffu) {
        free(c.out);
        return 0;
    }
    *bytes = (uint32_t)c.n;
    return c.out;
}

static void le(uint8_t *p, uint32_t v, int n)
{
    while (n--)
        *p++ = (uint8_t)v, v >>= 8;
}

uint8_t *pixzip_pack(const uint8_t *px, uint32_t w, uint32_t h, const uint8_t *palette, uint32_t k,
                     const uint8_t *alpha, uint32_t alphas, uint32_t *bytes)
{
    uint32_t n[2], size, table = 3 * k + alphas, rev;
    uint8_t *s[2], *out, *check;
    uint16_t *scratch;
    struct pixzip_info f;
    int bad;

    if (k - 1 > 255 || alphas > k)
        return 0;
    s[0] = pixzip_encode(px, w, h, k, 0, &n[0]);
    s[1] = s[0] ? pixzip_encode(px, w, h, k, 1, &n[1]) : 0;
    rev = s[1] && n[1] < n[0];
    out = s[1] && n[rev] <= 0xffffffffu - PIXZIP_HEADER - table ? malloc(PIXZIP_HEADER + table + n[rev]) : 0;
    if (out) {
        size = PIXZIP_HEADER + table + n[rev];
        memcpy(out, "PXZ\1", 4);
        le(out + 4, w, 2);
        le(out + 6, h, 2);
        out[8] = (uint8_t)(k - 1);
        out[9] = (uint8_t)rev;
        le(out + 10, alphas, 2);
        le(out + 12, n[rev], 4);
        memcpy(out + PIXZIP_HEADER, palette, 3 * k);
        if (alphas)
            memcpy(out + PIXZIP_HEADER + 3 * k, alpha, alphas);
        memcpy(out + PIXZIP_HEADER + table, s[rev], n[rev]);
        /* Every file is decoded before it is handed out. */
        check = malloc((size_t)w * h);
        scratch = malloc(pixzip_scratch(k) * sizeof *scratch);
        bad = !check || !scratch || pixzip_info(out, size, &f)
              || pixzip_decode(out, size, check, w * h, scratch, f.scratch)
              || memcmp(check, px, (size_t)w * h);
        free(check);
        free(scratch);
        if (bad) {
            free(out);
            out = 0;
        } else
            *bytes = size;
    }
    free(s[0]);
    free(s[1]);
    return out;
}
