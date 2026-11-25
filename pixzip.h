#ifndef PIXZIP_H
#define PIXZIP_H
#include <stdint.h>

/* Pixzip: lossless pictures of up to 256 palette entries (FORMAT.md).

   The decoder, pixzip.c, is freestanding C: no C library, no allocation,
   no static state, integer arithmetic only. Whatever the bytes, it reads
   nothing past the input and writes nothing past the picture. The caller
   gives it the picture's buffer and a scratch table of 16-bit
   probabilities, pixzip_scratch(colours) entries long. Calls on separate
   buffers may run at the same time. */

#define PIXZIP_HEADER 16

struct pixzip_info {
    uint32_t width, height;     /* each 1 to 65535 */
    uint32_t colours;           /* palette entries, 1 to 256 */
    uint32_t alphas;            /* entries with an alpha byte, 0 to colours; the rest are opaque */
    uint32_t reversed;          /* 1 when the stream runs in reverse raster order */
    uint32_t pixels;            /* width * height: the bytes the picture needs */
    uint32_t scratch;           /* pixzip_scratch(colours): the 16-bit entries the decoder needs */
    const uint8_t *palette;     /* colours * 3 bytes, red, green, blue, inside the file */
    const uint8_t *alpha;       /* alphas bytes, inside the file */
    const uint8_t *stream;      /* the coded pixels, inside the file */
    uint32_t stream_bytes;
};

/* The scratch entries (uint16_t) a picture of `colours` entries needs:
   128 + colours + colours * 2^bits, where bits is the fewest that hold
   colours - 1. 4,288 entries (8,576 bytes) for 64 colours, 65,920
   (131,840 bytes) for 256. Zero when colours is not 1 to 256. */
uint32_t pixzip_scratch(uint32_t colours);

/* Reads and checks a file's header and fills info. Zero means a valid
   header whose parts add up to exactly `bytes`. */
int pixzip_info(const uint8_t *src, uint32_t bytes, struct pixzip_info *info);

/* Decodes a whole file into px, one palette index a pixel, row by row
   from the top left. capacity is px's size in bytes, at least
   info.pixels; count is scratch's size in entries, at least info.scratch.
   Zero means success; on failure px holds no meaningful picture. */
int pixzip_decode(const uint8_t *src, uint32_t bytes, uint8_t *px, uint32_t capacity,
                  uint16_t *scratch, uint32_t count);

/* The low-level call: decodes a raw stream of `bytes` bytes into the
   width * height indices at px, in the order they were coded (reverse a
   reversed stream afterwards), using pixzip_scratch(colours) entries of
   scratch. It checks the stream but trusts the sizes: px must hold
   width * height bytes. Zero means success. */
int pixzip_unpack(const uint8_t *stream, uint32_t bytes, uint32_t width, uint32_t height,
                  uint32_t colours, uint8_t *px, uint16_t *scratch);

/* The encoder, encode.c, needs the C library. Both calls return a buffer
   from malloc, which the caller frees, and set *bytes to its length; they
   return 0 when an argument is out of range, an index is not below
   colours, or memory runs out. */

/* The raw stream of px, in raster order, or reverse raster order when
   reversed is non-zero. */
uint8_t *pixzip_encode(const uint8_t *px, uint32_t width, uint32_t height, uint32_t colours,
                       int reversed, uint32_t *bytes);

/* A whole file: both scan orders are tried and the smaller kept. palette
   is colours * 3 bytes; alpha is `alphas` bytes, or 0 when alphas is 0.
   The file is decoded with pixzip_decode and compared before it is
   returned. */
uint8_t *pixzip_pack(const uint8_t *px, uint32_t width, uint32_t height, const uint8_t *palette,
                     uint32_t colours, const uint8_t *alpha, uint32_t alphas, uint32_t *bytes);

#endif
