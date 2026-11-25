"""The reference Pixzip coder (FORMAT.md), which the C encoder and decoder
are held to. Standard library only.

    encode(px, w, h, k, reversed=False) -> the raw stream
    unpack(stream, w, h, k) -> the indices, in the order they were coded
    pack(px, w, h, palette, alpha=b"") -> a whole file, the smaller scan order
    decode(data) -> (w, h, px, palette, alpha)

px is a sequence of w * h palette indices, row by row from the top left;
palette is 3 bytes an entry. Damaged input raises ValueError."""
import struct

MAGIC = b"PXZ\x01"


def bits_for(k):
    return (k - 1).bit_length()


def scratch(k):
    """The probabilities a picture of k entries needs: west and north by
    pattern, north-east by colour, then a literal tree for each colour."""
    return [1024] * 128 + [1536] * k + [1024] * (k << bits_for(k))


def neighbours(p, w, x, y, at):
    cw = p[at - 1] if x else p[at - w] if y else 0
    cn = p[at - w] if y else cw
    nw = p[at - w - 1] if x and y else cn
    ne = p[at - w + 1] if y and x + 1 < w else cn
    pat = ((cw == cn) | (cw == nw) << 1 | (cn == ne) << 2 | (cn == nw) << 3
           | (cw == ne) << 4 | (x & 1) << 5)
    return cw, cn, ne, pat


class Coder:
    """The encoding side of the binary range coder."""

    def __init__(self):
        self.low, self.range, self.cache, self.size, self.out = 0, 0xFFFFFFFF, 0, 1, bytearray()

    def shift(self):
        if self.low < 0xFF000000 or self.low >> 32:
            carry, t = self.low >> 32, self.cache
            while True:
                self.out.append((t + carry) & 255)
                t = 0xFF
                self.size -= 1
                if not self.size:
                    break
            self.cache = (self.low >> 24) & 255
        self.size += 1
        self.low = (self.low & 0x00FFFFFF) << 8

    def bit(self, prob, i, b):
        q = prob[i]
        bound = (self.range >> 11) * q
        if b:
            self.low += bound
            self.range -= bound
            prob[i] = q - (q >> 4)
        else:
            self.range = bound
            prob[i] = q + ((2048 - q) >> 4)
        while self.range < 1 << 24:
            self.range = (self.range << 8) & 0xFFFFFFFF
            self.shift()

    def finish(self):
        for _ in range(5):
            self.shift()
        return bytes(self.out)


def check(w, h, k):
    if not (1 <= w <= 65535 and 1 <= h <= 65535 and 1 <= k <= 256):
        raise ValueError("width and height must be 1 to 65535, and colours 1 to 256")


def encode(px, w, h, k, reversed=False):
    check(w, h, k)
    if len(px) != w * h or any(not 0 <= v < k for v in px):
        raise ValueError(f"need {w * h} indices, each below {k}")
    px = list(px)[::-1] if reversed else list(px)
    bits, prob, tree = bits_for(k), scratch(k), 128 + k
    c = Coder()
    for y in range(h):
        for x in range(w):
            at = y * w + x
            cw, cn, ne, pat = neighbours(px, w, x, y, at)
            v = px[at]
            c.bit(prob, pat, v == cw)
            if v == cw:
                continue
            if cn != cw:
                c.bit(prob, 64 + pat, v == cn)
                if v == cn:
                    continue
            if ne != cw and ne != cn:
                c.bit(prob, 128 + cn, v == ne)
                if v == ne:
                    continue
            m = 1
            for i in range(bits - 1, -1, -1):
                b = v >> i & 1
                c.bit(prob, tree + (cn << bits) + m, b)
                m = m * 2 + b
    return c.finish()


def unpack(s, w, h, k):
    check(w, h, k)
    if not s or s[0]:
        raise ValueError("a stream starts with a zero byte")
    bits, prob, tree = bits_for(k), scratch(k), 128 + k
    at, rng, code = 5, 0xFFFFFFFF, int.from_bytes(s[:5].ljust(5, b"\0"), "big") & 0xFFFFFFFF

    def bit(i):
        nonlocal at, rng, code
        q = prob[i]
        bound = (rng >> 11) * q
        b = code >= bound
        if b:
            rng -= bound
            code -= bound
            prob[i] = q - (q >> 4)
        else:
            rng = bound
            prob[i] = q + ((2048 - q) >> 4)
        if rng < 1 << 24:
            rng = (rng << 8) & 0xFFFFFFFF
            code = ((code << 8) | (s[at] if at < len(s) else 0)) & 0xFFFFFFFF
            at += 1
        return b

    p = [0] * (w * h)
    for y in range(h):
        for x in range(w):
            i = y * w + x
            cw, cn, ne, pat = neighbours(p, w, x, y, i)
            if bit(pat):
                v = cw
            elif cn != cw and bit(64 + pat):
                v = cn
            elif ne != cw and ne != cn and bit(128 + cn):
                v = ne
            else:
                m = 1
                for _ in range(bits):
                    m = m * 2 + bit(tree + (cn << bits) + m)
                v = m - (1 << bits)
                if v in (cw, cn, ne):
                    raise ValueError(f"a literal {v} repeats a neighbour")
            if v >= k:
                raise ValueError(f"index {v} is not below {k}")
            p[i] = v
    if at < len(s):
        raise ValueError("the stream has bytes the decoder never reads")
    return p


def pack(px, w, h, palette, alpha=b""):
    k = len(palette) // 3
    if len(palette) != 3 * k or len(alpha) > k:
        raise ValueError("the palette is 3 bytes an entry, with at most one alpha byte an entry")
    forward, backward = encode(px, w, h, k), encode(px, w, h, k, True)
    reverse = len(backward) < len(forward)
    stream = backward if reverse else forward
    return (MAGIC + struct.pack("<HHBBHI", w, h, k - 1, reverse, len(alpha), len(stream))
            + bytes(palette) + bytes(alpha) + stream)


def decode(data):
    if len(data) < 16 or data[:4] != MAGIC or data[9] > 1:
        raise ValueError("not a version 1 Pixzip file")
    w, h, k, reverse, alphas, n = struct.unpack_from("<HHBBHI", data, 4)
    k += 1
    if not w or not h or alphas > k or 16 + 3 * k + alphas + n != len(data):
        raise ValueError("the header's sizes do not add up to the file")
    palette, alpha = data[16:16 + 3 * k], data[16 + 3 * k:16 + 3 * k + alphas]
    px = unpack(data[16 + 3 * k + alphas:], w, h, k)
    return w, h, px[::-1] if reverse else px, bytes(palette), bytes(alpha)
