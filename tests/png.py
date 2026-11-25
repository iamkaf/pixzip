"""Just enough PNG for the tests: read any non-interlaced PNG of up to 8
bits a channel, and write indexed and RGBA ones. Standard library only."""
import struct
import zlib


def read(path):
    """(w, h, rgba, indexed): rgba is a list of (r, g, b, a) a pixel;
    indexed is (indices, palette of (r, g, b, a)) for an indexed PNG,
    else None."""
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", f"{path}: not a PNG"
    at, idat, pal, trns = 8, b"", [], b""
    while at < len(data):
        n, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + n]
        at += 12 + n
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            pal = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif kind == b"tRNS":
            trns = body
        elif kind == b"IDAT":
            idat += body
    assert not interlace and depth <= 8, f"{path}: interlaced or 16 bits a channel"
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bpp = max(1, channels * depth // 8)
    stride = (w * channels * depth + 7) // 8
    raw, prev, rows = zlib.decompress(idat), bytearray(stride), []
    for y in range(h):
        ft, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if ft == 1:
                line[i] = (line[i] + a) & 255
            elif ft == 2:
                line[i] = (line[i] + b) & 255
            elif ft == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    samples = []
    for line in rows:
        if depth == 8:
            samples += line[:w * channels]
        else:
            per = 8 // depth
            samples += [line[i // per] >> ((per - 1 - i % per) * depth) & ((1 << depth) - 1)
                        for i in range(w * channels)]
    if ctype == 3:
        palette = [c + ((trns[i] if i < len(trns) else 255),) for i, c in enumerate(pal)]
        return w, h, [palette[v] for v in samples], (samples, palette)
    scale = 255 // ((1 << depth) - 1)
    key = struct.unpack(">" + "H" * (len(trns) // 2), trns) if trns else None
    out = []
    for i in range(w * h):
        s = samples[i * channels:(i + 1) * channels]
        if ctype in (0, 4):
            pixel = (s[0] * scale,) * 3 + ((s[1],) if ctype == 4 else (0 if key and s[0] == key[0] else 255,))
        else:
            pixel = tuple(s[:3]) + ((s[3],) if ctype == 6 else (0 if key and tuple(s) == key else 255,))
        out.append(pixel)
    return w, h, out, None


def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))


def write(path, w, h, rows, ctype, depth=8, palette=None):
    """rows: each row's samples, already packed for depth (bytes). palette:
    (r, g, b, a) tuples for an indexed PNG."""
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, 0))
    if palette:
        out += chunk(b"PLTE", bytes(c for p in palette for c in p[:3]))
        if any(p[3] != 255 for p in palette):
            out += chunk(b"tRNS", bytes(p[3] for p in palette))
    out += chunk(b"IDAT", zlib.compress(b"".join(b"\0" + bytes(r) for r in rows), 9))
    open(path, "wb").write(out + chunk(b"IEND", b""))


def write_rgba(path, w, h, pixels):
    write(path, w, h, [bytes(c for p in pixels[y * w:(y + 1) * w] for c in p) for y in range(h)], 6)


def write_indexed(path, w, h, indices, palette):
    write(path, w, h, [bytes(indices[y * w:(y + 1) * w]) for y in range(h)], 3, 8, palette)
