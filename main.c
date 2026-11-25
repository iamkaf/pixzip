/* pixzip: pack, unpack and compare palette pictures (README.md). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lodepng/lodepng.h"
#include "pixzip.h"

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#define QUIET " >NUL 2>&1"
#else
#include <unistd.h>
#define QUIET " >/dev/null 2>&1"
#endif

struct picture {
    uint32_t w, h, k, alphas;
    uint8_t *px;
    uint8_t rgb[768], alpha[256];
};

static void die(const char *what, const char *why)
{
    fprintf(stderr, "pixzip: %s: %s\n", what, why);
    exit(1);
}

static void *grab(size_t n)
{
    void *p = malloc(n ? n : 1);

    if (!p)
        die("memory", "out of memory");
    return p;
}

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b = 0;
    size_t cap = 0, got;

    *n = 0;
    if (!f)
        die(path, "cannot open");
    do {
        if (*n == cap) {
            cap = cap ? cap * 2 : 1 << 16;
            b = realloc(b, cap);
            if (!b)
                die(path, "out of memory");
        }
        got = fread(b + *n, 1, cap - *n, f);
        *n += got;
    } while (got);
    if (ferror(f))
        die(path, "cannot read");
    fclose(f);
    return b;
}

static void spill(const char *path, const uint8_t *b, size_t n)
{
    FILE *f = fopen(path, "wb");

    if (!f || fwrite(b, 1, n, f) != n || fclose(f))
        die(path, "cannot write");
}

/* An entry for each colour, in the order the colours first appear. */
static void index_colours(struct picture *p, const uint8_t *rgba, const char *path)
{
    uint32_t slot[1024], i, j, n = p->w * p->h, c;

    memset(slot, 0xff, sizeof slot);    /* entry + 1 in the low byte; 0xffffffff empty */
    p->k = 0;
    for (i = 0; i < n; i++, rgba += 4) {
        c = (uint32_t)rgba[0] << 24 | (uint32_t)rgba[1] << 16 | (uint32_t)rgba[2] << 8 | rgba[3];
        for (j = (c * 2654435761u) >> 22; slot[j] != 0xffffffffu; j = (j + 1) & 1023) {
            uint32_t e = slot[j];
            if (!memcmp(p->rgb + 3 * e, rgba, 3) && p->alpha[e] == rgba[3])
                break;
        }
        if (slot[j] == 0xffffffffu) {
            if (p->k == 256)
                die(path, "has more than 256 colours");
            slot[j] = p->k;
            memcpy(p->rgb + 3 * p->k, rgba, 3);
            p->alpha[p->k++] = rgba[3];
        }
        p->px[i] = (uint8_t)slot[j];
    }
}

static void load_png(struct picture *p, const char *path)
{
    size_t n;
    uint8_t *file = slurp(path, &n), *raw = 0;
    unsigned w, h, e, i, bits;
    LodePNGState st;

    lodepng_state_init(&st);
    e = lodepng_inspect(&w, &h, &st, file, n);
    if (e)
        die(path, lodepng_error_text(e));
    if (w > 65535 || h > 65535)
        die(path, "is wider or taller than 65535 pixels");
    p->w = w;
    p->h = h;
    p->px = grab((size_t)w * h);
    if (st.info_png.color.colortype == LCT_PALETTE) {
        st.decoder.color_convert = 0;
        e = lodepng_decode(&raw, &w, &h, &st, file, n);
        if (e)
            die(path, lodepng_error_text(e));
        bits = st.info_png.color.bitdepth;
        p->k = 1;
        for (i = 0; i < w * h; i++) {
            /* Rows below 8 bits a pixel come without padding. */
            unsigned at = i * bits, v = raw[at >> 3] >> (8 - bits - (at & 7)) & ((1u << bits) - 1);
            if (v >= st.info_png.color.palettesize)
                die(path, "uses an index past its palette");
            p->px[i] = (uint8_t)v;
            if (v >= p->k)
                p->k = v + 1;
        }
        for (i = 0; i < p->k; i++) {
            memcpy(p->rgb + 3 * i, st.info_png.color.palette + 4 * i, 3);
            p->alpha[i] = st.info_png.color.palette[4 * i + 3];
        }
    } else if (st.info_png.color.bitdepth == 16) {
        e = lodepng_decode_memory(&raw, &w, &h, file, n, LCT_RGBA, 16);
        if (e)
            die(path, lodepng_error_text(e));
        for (i = 0; i < 4 * w * h; i++) {
            if (raw[2 * i] != raw[2 * i + 1])
                die(path, "has colours that need 16 bits a channel");
            raw[i] = raw[2 * i];
        }
        index_colours(p, raw, path);
    } else {
        e = lodepng_decode_memory(&raw, &w, &h, file, n, LCT_RGBA, 8);
        if (e)
            die(path, lodepng_error_text(e));
        index_colours(p, raw, path);
    }
    for (p->alphas = p->k; p->alphas && p->alpha[p->alphas - 1] == 255; p->alphas--)
        ;
    lodepng_state_cleanup(&st);
    free(raw);
    free(file);
}

static uint8_t *pack(const struct picture *p, uint32_t *n, const char *path)
{
    uint8_t *out = pixzip_pack(p->px, p->w, p->h, p->rgb, p->k, p->alpha, p->alphas, n);

    if (!out)
        die(path, "failed to pack");
    return out;
}

/* An indexed PNG with the picture's own palette and indices, at the
   fewest bits a pixel that hold them. */
static uint8_t *png(const uint8_t *px, uint32_t w, uint32_t h, const uint8_t *rgb, uint32_t k,
                    const uint8_t *alpha, uint32_t alphas, size_t *n)
{
    unsigned bits = k <= 2 ? 1 : k <= 4 ? 2 : k <= 16 ? 4 : 8, e;
    uint8_t *raw = grab(((size_t)w * h * bits + 7) / 8), *out = 0;
    size_t i, at;
    LodePNGState st;

    memset(raw, 0, ((size_t)w * h * bits + 7) / 8);
    for (i = 0; i < (size_t)w * h; i++) {
        at = i * bits;
        raw[at >> 3] |= (uint8_t)(px[i] << (8 - bits - (at & 7)));
    }
    lodepng_state_init(&st);
    st.encoder.auto_convert = 0;
    st.encoder.zlibsettings.windowsize = 32768;
    st.info_raw.colortype = st.info_png.color.colortype = LCT_PALETTE;
    st.info_raw.bitdepth = st.info_png.color.bitdepth = bits;
    for (i = 0; i < k; i++) {
        uint8_t a = i < alphas ? alpha[i] : 255;
        lodepng_palette_add(&st.info_raw, rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2], a);
        lodepng_palette_add(&st.info_png.color, rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2], a);
    }
    e = lodepng_encode(&out, n, raw, w, h, &st);
    if (e)
        die("png", lodepng_error_text(e));
    lodepng_state_cleanup(&st);
    free(raw);
    return out;
}

static void unpack(const char *from, const char *to)
{
    size_t n, size;
    uint8_t *file = slurp(from, &n), *px, *out;
    uint16_t *scratch;
    struct pixzip_info f;

    if (n > 0xffffffffu || pixzip_info(file, (uint32_t)n, &f))
        die(from, "is not a Pixzip file");
    px = grab(f.pixels);
    scratch = grab(f.scratch * sizeof *scratch);
    if (pixzip_decode(file, (uint32_t)n, px, f.pixels, scratch, f.scratch))
        die(from, "is damaged");
    out = png(px, f.width, f.height, f.palette, f.colours, f.alpha, f.alphas, &size);
    spill(to, out, size);
    free(out);
    free(scratch);
    free(px);
    free(file);
}

/* --- compare --- */

static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n = -1;

    if (f && !fseek(f, 0, SEEK_END))
        n = ftell(f);
    if (f)
        fclose(f);
    return n;
}

static const char *temp(int turn, const char *ext)
{
    static char path[3][4200];
    const char *dir = getenv("TMPDIR");

    if (!dir)
        dir = getenv("TEMP");
#ifdef _WIN32
    if (!dir)
        dir = ".";
#else
    if (!dir)
        dir = "/tmp";
#endif
    snprintf(path[turn], sizeof path[turn], "%s/pixzip-%d-%d.%s", dir, (int)getpid(), turn, ext);
    remove(path[turn]);
    return path[turn];
}

/* Runs a tool that writes `out`, and gives the size it wrote, or -1 when
   the tool is missing or fails. */
static long run(const char *out, const char *format, const char *a, const char *b)
{
    char command[9000];
    long n;

    snprintf(command, sizeof command - sizeof QUIET, format, a, b);
    strcat(command, QUIET);
    n = system(command) == 0 ? file_size(out) : -1;
    return n;
}

#define COLUMNS 7
static const char *column[COLUMNS] = { "PNG", "oxipng", "optipng", "zopflipng", "WebP", "GIF", "Pixzip" };

static void measure(const char *path, long *total)
{
    struct picture p;
    uint8_t *packed, *indexed;
    uint32_t n, i, clear = 0, partial = 0;
    size_t pn;
    const char *in = temp(0, "png"), *out = temp(1, "out"), *gif = temp(2, "gif");
    long s[COLUMNS];
    int t;

    load_png(&p, path);
    packed = pack(&p, &n, path);
    indexed = png(p.px, p.w, p.h, p.rgb, p.k, p.alpha, p.alphas, &pn);
    spill(in, indexed, pn);
    s[0] = (long)pn;
    s[1] = run(out, "oxipng -q -o max -Z --strip safe --out \"%s\" \"%s\"", out, in);
    remove(out);
    s[2] = run(out, "optipng -quiet -o7 -strip all -out \"%s\" \"%s\"", out, in);
    remove(out);
    s[3] = run(out, "zopflipng -m -y \"%s\" \"%s\"", in, out);
    remove(out);
    s[4] = run(out, "cwebp -quiet -lossless -z 9 -exact \"%s\" -o \"%s\"", in, out);
    remove(out);
    /* A GIF keeps one clear colour and no partial alpha. */
    for (i = 0; i < p.alphas; i++) {
        clear += !p.alpha[i];
        partial += p.alpha[i] && p.alpha[i] != 255;
    }
    s[5] = -1;
    if (clear <= 1 && !partial && run(gif, "magick \"%s\" \"%s\"", in, gif) >= 0) {
        s[5] = run(out, "gifsicle -O3 \"%s\" -o \"%s\"", gif, out);
        if (s[5] < 0)
            s[5] = file_size(gif);
        remove(out);
        remove(gif);
    }
    s[6] = (long)n;
    remove(in);
    printf("%-32s %7u %4u", path, p.w * p.h, p.k);
    for (t = 0; t < COLUMNS; t++) {
        if (s[t] >= 0)
            printf(" %9ld", s[t]);
        else
            printf(" %9s", "-");
        total[t] = s[t] < 0 || total[t] < 0 ? -1 : total[t] + s[t];
    }
    printf("\n");
    free(indexed);
    free(packed);
    free(p.px);
}

static void compare(int count, char **paths)
{
    long total[COLUMNS] = { 0 };
    int i, t;

    printf("%-32s %7s %4s", "file", "pixels", "cols");
    for (t = 0; t < COLUMNS; t++)
        printf(" %9s", column[t]);
    printf("\n");
    for (i = 0; i < count; i++)
        measure(paths[i], total);
    if (count > 1) {
        printf("%-32s %7s %4s", "total", "", "");
        for (t = 0; t < COLUMNS; t++) {
            if (total[t] >= 0)
                printf(" %9ld", total[t]);
            else
                printf(" %9s", "-");
        }
        printf("\n");
    }
}

static void usage(void)
{
    fputs("usage: pixzip pack INPUT.png OUTPUT.pxz\n"
          "       pixzip unpack INPUT.pxz OUTPUT.png\n"
          "       pixzip compare INPUT.png...\n", stderr);
    exit(2);
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "";

    if (!strcmp(cmd, "compare") && argc > 2)
        compare(argc - 2, argv + 2);
    else if (!strcmp(cmd, "pack") && argc == 4) {
        struct picture p;
        uint32_t n;
        uint8_t *out;

        load_png(&p, argv[2]);
        out = pack(&p, &n, argv[2]);
        spill(argv[3], out, n);
        free(out);
        free(p.px);
    } else if (!strcmp(cmd, "unpack") && argc == 4)
        unpack(argv[2], argv[3]);
    else
        usage();
    return 0;
}
