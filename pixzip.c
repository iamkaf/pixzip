/* pixzip.c: the decoder (FORMAT.md). Freestanding: it uses no C library
   and keeps no state between calls.

   A pixel tries its west, north and north-east neighbours before it codes
   a literal palette index. The first two questions are asked in a
   context made of the equality of nearby colours and the column's
   parity; the third and the literal's bits in a context of the north
   colour. An adaptive binary range coder codes every answer with an
   11-bit probability that moves 1/16 of the way to each bit it sees. */

#include "pixzip.h"

struct coder {
    const uint8_t *in, *end;
    uint32_t range, code;
};

static unsigned bit(struct coder *c, uint16_t *q)
{
    uint32_t bound = (c->range >> 11) * *q;
    unsigned b = c->code >= bound;

    if (b) {
        c->range -= bound;
        c->code -= bound;
        *q -= *q >> 4;
    } else {
        c->range = bound;
        *q += (2048 - *q) >> 4;
    }
    /* One step is enough: a probability stays within 15 to 2033, so the
       range never falls below 2^16. */
    if (c->range < 1u << 24) {
        c->range <<= 8;
        c->code = c->code << 8 | (c->in < c->end ? *c->in++ : 0);
    }
    return b;
}

static uint32_t bits_for(uint32_t colours)
{
    uint32_t bits = 0;

    while (1u << bits < colours)
        bits++;
    return bits;
}

uint32_t pixzip_scratch(uint32_t colours)
{
    return colours - 1 < 256 ? 128 + colours + (colours << bits_for(colours)) : 0;
}

int pixzip_unpack(const uint8_t *s, uint32_t bytes, uint32_t w, uint32_t h, uint32_t k, uint8_t *p,
                  uint16_t *prob)
{
    struct coder c;
    uint32_t x, y, i, m, v, cw, cn, nw, ne, pat, bits, size = pixzip_scratch(k);
    uint16_t *tree;

    if (!size || !bytes || s[0])
        return 1;
    bits = bits_for(k);
    tree = prob + 128 + k;
    c.in = s;
    c.end = s + bytes;
    c.range = 0xffffffffu;
    c.code = 0;
    for (i = 0; i < 5; i++)
        c.code = c.code << 8 | (c.in < c.end ? *c.in++ : 0);
    /* West and north by pattern, 64 each; north-east by the north colour,
       starting at 1536 of 2048 towards "not"; then a literal tree for
       each north colour. */
    for (i = 0; i < size; i++)
        prob[i] = i - 128 < k ? 1536 : 1024;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++, p++) {
            /* The neighbours; off the picture, the nearest one on it. */
            cw = x ? p[-1] : y ? *(p - w) : 0;
            cn = y ? *(p - w) : cw;
            nw = x && y ? *(p - w - 1) : cn;
            ne = y && x + 1 < w ? *(p - w + 1) : cn;
            pat = (cw == cn) | (cw == nw) << 1 | (cn == ne) << 2 | (cn == nw) << 3 | (cw == ne) << 4
                | (x & 1) << 5;
            if (bit(&c, prob + pat))
                v = cw;
            else if (cn != cw && bit(&c, prob + 64 + pat))
                v = cn;
            else if (ne != cw && ne != cn && bit(&c, prob + 128 + cn))
                v = ne;
            else {
                for (m = 1, i = 0; i < bits; i++)
                    m = m * 2 + bit(&c, tree + (cn << bits) + m);
                v = m - (1u << bits);
                /* The encoder never spells out a colour it could have
                   named by a neighbour. */
                if (v == cw || v == cn || v == ne)
                    return 1;
            }
            if (v >= k)
                return 1;
            *p = (uint8_t)v;
        }
    /* Every byte of the stream must have been read. */
    return c.in != c.end;
}

static uint32_t le(const uint8_t *p, int n)
{
    uint32_t v = 0;

    while (n--)
        v = v << 8 | p[n];
    return v;
}

int pixzip_info(const uint8_t *s, uint32_t bytes, struct pixzip_info *f)
{
    uint32_t rest = bytes - PIXZIP_HEADER, table;

    if (bytes < PIXZIP_HEADER || s[0] != 'P' || s[1] != 'X' || s[2] != 'Z' || s[3] != 1 || s[9] > 1)
        return 1;
    f->width = le(s + 4, 2);
    f->height = le(s + 6, 2);
    f->colours = s[8] + 1u;
    f->reversed = s[9];
    f->alphas = le(s + 10, 2);
    f->stream_bytes = le(s + 12, 4);
    f->pixels = f->width * f->height;
    f->scratch = pixzip_scratch(f->colours);
    table = 3 * f->colours + f->alphas;
    if (!f->pixels || f->alphas > f->colours || table > rest || rest - table != f->stream_bytes)
        return 1;
    f->palette = s + PIXZIP_HEADER;
    f->alpha = f->palette + 3 * f->colours;
    f->stream = f->alpha + f->alphas;
    return 0;
}

int pixzip_decode(const uint8_t *src, uint32_t bytes, uint8_t *px, uint32_t capacity, uint16_t *scratch,
                  uint32_t count)
{
    struct pixzip_info f;
    uint8_t *lo, *hi, t;

    if (pixzip_info(src, bytes, &f) || f.pixels > capacity || f.scratch > count
        || pixzip_unpack(f.stream, f.stream_bytes, f.width, f.height, f.colours, px, scratch))
        return 1;
    if (f.reversed)
        for (lo = px, hi = px + f.pixels - 1; lo < hi;) {
            t = *lo;
            *lo++ = *hi;
            *hi-- = t;
        }
    return 0;
}
