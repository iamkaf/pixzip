# Pixzip file format, version 1

This is the format that `pixzip pack` and `pixzip_pack` write and that `pixzip_decode` and `pixzip.py` read. It holds one picture of up to 256 palette entries. Version 1 is frozen: the bytes below always mean what they mean here, and any incompatible change takes a new version number. All integers are little-endian.

## The file

| Offset | Size | Field |
|---:|---:|---|
| 0 | 3 | `PXZ` |
| 3 | 1 | Version: 1 |
| 4 | 2 | Width `w`, 1 to 65535 |
| 6 | 2 | Height `h`, 1 to 65535 |
| 8 | 1 | Palette entries less one, so `k` is 1 to 256 |
| 9 | 1 | Scan order: 0 for raster order, 1 for reverse raster order |
| 10 | 2 | Alpha entries `a`, 0 to `k` |
| 12 | 4 | Stream length `n` |
| 16 | 3`k` | The palette: red, green and blue for each entry |
| 16 + 3`k` | `a` | The alpha of entries 0 to `a` − 1. Entries from `a` on are opaque (255) |
| 16 + 3`k` + `a` | `n` | The stream |

The file ends where the stream ends. A reader refuses a file whose header has another tag or version, a scan order other than 0 or 1, a zero width or height, more alpha entries than palette entries, or sizes that don't add up to exactly the file's length. The palette and alpha bytes may hold any values, and entries may repeat.

The picture is `w` × `h` palette indices, row by row from the top left. The stream codes them in raster order, or, with scan order 1, in reverse raster order: from the bottom right, right to left, bottom to top. A reader decodes the stream as if it were in raster order and then reverses the `w` × `h` indices.

## The range decoder

The stream is read by an adaptive binary range decoder, the one LZMA uses. Its state is two 32-bit unsigned numbers, `range` and `code`. Bytes past the end of the stream read as 0.

To start, set `range` to 0xFFFFFFFF and `code` to 0, then five times set `code = code << 8 | next byte`. The first byte is shifted out again, and the encoder always writes it as 0.

A probability `q` is the chance, in 2048ths, that the next bit is 0. To decode a bit with `q`:

1. `bound = (range >> 11) × q`.
2. If `code < bound`, the bit is 0: `range = bound`, and `q += (2048 − q) >> 4`.
3. Otherwise the bit is 1: `range −= bound`, `code −= bound`, and `q −= q >> 4`.
4. If `range < 2^24`: `range <<= 8` and `code = code << 8 | next byte`.

All arithmetic is modulo 2^32. A probability can't leave the range 15 to 2033, so `range` never falls below 2^16 and one step in 4 is enough.

## The probabilities

Let `b` be the fewest bits that hold `k` − 1: 0 for one entry, 1 for two, 6 for 33 to 64, and 8 for 129 to 256. The decoder keeps 128 + `k` + `k` × 2^`b` probabilities:

| Index | Asks | Context | Starts at |
|---|---|---|---:|
| 0 to 63 | Is it the west colour? | The pattern | 1024 |
| 64 to 127 | Is it the north colour? | The pattern | 1024 |
| 128 + `c` | Is it the north-east colour? | The north colour `c` | 1536 |
| 128 + `k` + `c` × 2^`b` + `m` | Literal bit, at node `m` (1 to 2^`b` − 1) | The north colour `c` | 1024 |

The layout is just a way to number the contexts. Any decoder that keeps one probability for each context, with the same starting values, decodes the same pictures.

## The pixels

For each pixel in coding order, at column `x` and row `y`, take its neighbours from the indices already decoded. Off the picture, a neighbour falls back on the nearest one that is on it:

- `W` is the pixel to the left; at the start of a row, the pixel above; at the first pixel, 0.
- `N` is the pixel above, or `W` on the first row.
- `NW` is the pixel above and to the left, or `N` where there is none.
- `NE` is the pixel above and to the right, or `N` where there is none.

The pattern is a 6-bit number:

```
(W == N) | (W == NW) << 1 | (N == NE) << 2 | (N == NW) << 3 | (W == NE) << 4 | (x & 1) << 5
```

Then decode:

1. A bit with probability `pattern`. If it is 1, the pixel is `W`.
2. Otherwise, if `N` ≠ `W`, a bit with probability 64 + `pattern`. If it is 1, the pixel is `N`.
3. Otherwise, if `NE` ≠ `W` and `NE` ≠ `N`, a bit with probability 128 + `N`. If it is 1, the pixel is `NE`.
4. Otherwise the pixel is a literal. Set `m` = 1, then `b` times set `m` = 2`m` + a bit with probability 128 + `k` + `N` × 2^`b` + `m`. The pixel is `m` − 2^`b`. Its bits come most significant first.

A step that is skipped decodes no bit.

## When a stream is valid

A reader refuses a stream when:

1. it is empty, or its first byte is not 0;
2. a pixel's index is `k` or more;
3. a literal equals `W`, `N` or `NE`, which the encoder never writes;
4. or bytes are left over after the last pixel. The decoder reads five bytes to start and one at each step 4 above, and that must cover the whole stream.

The format has no checksum. These rules catch most damage, but a damaged file can still decode to a wrong picture. Whatever the bytes, a reader never needs to read past the stream or write past the picture.

## The encoder

The encoder is the range encoder that matches the decoder, as LZMA writes it. Its state is `low`, a number of at least 33 bits, `range`, a byte `cache` and a count `pending`. It starts with `low` = 0, `range` = 0xFFFFFFFF, `cache` = 0 and `pending` = 1.

To encode a bit with `q`, take `bound = (range >> 11) × q`. For a 0, `range = bound`. For a 1, `low += bound` and `range −= bound`. Update `q` as the decoder does. Then, while `range < 2^24`, set `range <<= 8` and shift.

To shift: if `low < 0xFF000000` or `low ≥ 2^32`, write `cache + (low >> 32)`, then `pending` − 1 bytes of `0xFF + (low >> 32)`, all modulo 256; set `pending` to 0 and `cache` to bits 24 to 31 of `low`. Either way, add 1 to `pending` and set `low` to `(low & 0x00FFFFFF) << 8`.

The encoder asks the same questions as the decoder, in the same order, and answers each from the true pixel. It skips the questions the decoder skips. After the last pixel it shifts five times. A file uses reverse raster order only when that stream is strictly shorter than the raster one.

## An example

A 4 × 3 picture with three entries, black, white and red, where entry 0 is clear:

```
0 0 1 1
0 2 2 1
0 0 2 1
```

packs to these 33 bytes:

```
50 58 5a 01 04 00 03 00 02 00 01 00 07 00 00 00   header
00 00 00 ff ff ff c8 28 28                        palette
00                                                alpha of entry 0
00 cd 89 b3 89 d0 00                              stream
```

The reverse stream would also take seven bytes, so the file keeps raster order.
