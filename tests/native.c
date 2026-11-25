/* The test harness: runs the cases test.py writes, each in buffers of
   exactly the size it needs, so that AddressSanitizer sees any access past
   them. Every number is a little-endian u32.

   0  raw:    w h k n outcome, the stream, then w * h indices if outcome is 0
   1  file:   n capacity count outcome, the file, then capacity indices if 0
   2  encode: w h k reversed n, w * h indices, then the n-byte stream
   3  pack:   w h k alphas n, w * h indices, palette, alpha, the n-byte file */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../pixzip.h"

static uint8_t *data;
static size_t at, size;

static uint32_t u32(void)
{
    uint32_t v;

    if (size - at < 4) {
        fprintf(stderr, "native: truncated cases\n");
        exit(2);
    }
    v = (uint32_t)data[at] | (uint32_t)data[at + 1] << 8 | (uint32_t)data[at + 2] << 16
        | (uint32_t)data[at + 3] << 24;
    at += 4;
    return v;
}

/* A buffer of exactly n bytes; malloc(0) may give none. */
static void *alloc(size_t n)
{
    return malloc(n ? n : 1);
}

/* n bytes of the cases, in a buffer of their own. */
static uint8_t *take(size_t n)
{
    uint8_t *p = alloc(n);

    if (!p || size - at < n) {
        fprintf(stderr, "native: truncated cases\n");
        exit(2);
    }
    memcpy(p, data + at, n);
    at += n;
    return p;
}

static int fail(int i, const char *what)
{
    fprintf(stderr, "case %d: %s\n", i, what);
    return 1;
}

int main(int argc, char **argv)
{
    FILE *f = argc > 1 ? fopen(argv[1], "rb") : 0;
    int i, bad = 0;

    if (!f || fseek(f, 0, SEEK_END) || (long)(size = (size_t)ftell(f)) < 0 || fseek(f, 0, SEEK_SET)
        || !(data = malloc(size + 1)) || fread(data, 1, size, f) != size) {
        fprintf(stderr, "native: cannot read the cases\n");
        return 2;
    }
    fclose(f);
    for (i = 0; at < size; i++) {
        uint32_t op = u32();

        if (op == 0) {
            uint32_t w = u32(), h = u32(), k = u32(), n = u32(), outcome = u32();
            uint8_t *s = take(n), *px = alloc((size_t)w * h), *want = 0;
            uint16_t *scratch = alloc(pixzip_scratch(k) * sizeof *scratch);
            int got;

            if (!outcome)
                want = take((size_t)w * h);
            got = pixzip_unpack(s, n, w, h, k, px, scratch);
            if (got != (int)outcome)
                bad |= fail(i, got ? "raw stream refused" : "raw stream accepted");
            else if (!got && memcmp(px, want, (size_t)w * h))
                bad |= fail(i, "raw stream decoded wrong");
            free(s), free(px), free(scratch), free(want);
        } else if (op == 1) {
            uint32_t n = u32(), capacity = u32(), count = u32(), outcome = u32();
            uint8_t *s = take(n), *px = alloc(capacity), *want = 0;
            uint16_t *scratch = alloc(count * sizeof *scratch);
            struct pixzip_info info;
            int got;

            if (!outcome)
                want = take(capacity);
            got = pixzip_decode(s, n, px, capacity, scratch, count);
            if (got != (int)outcome)
                bad |= fail(i, got ? "file refused" : "file accepted");
            else if (!got && (pixzip_info(s, n, &info) || info.pixels != capacity || memcmp(px, want, capacity)))
                bad |= fail(i, "file decoded wrong");
            free(s), free(px), free(scratch), free(want);
        } else if (op == 2 || op == 3) {
            uint32_t w = u32(), h = u32(), k = u32(), extra = u32(), n = u32(), got_n = 0;
            uint8_t *px = take((size_t)w * h), *rgb = op == 3 ? take(3 * (size_t)k) : 0;
            uint8_t *alpha = op == 3 ? take(extra) : 0, *want = take(n), *got;

            got = op == 2 ? pixzip_encode(px, w, h, k, (int)extra, &got_n)
                          : pixzip_pack(px, w, h, rgb, k, extra ? alpha : 0, extra, &got_n);
            if (!got)
                bad |= fail(i, "the encoder refused");
            else if (got_n != n || memcmp(got, want, n))
                bad |= fail(i, "the encoder's output differs from the reference");
            free(px), free(rgb), free(alpha), free(want), free(got);
        } else {
            fprintf(stderr, "native: unknown case %u\n", op);
            return 2;
        }
    }
    printf("%d cases\n", i);
    free(data);
    return bad;
}
