"""The tests. CC picks the compiler (cc by default), which must take GCC's
flags and have AddressSanitizer and UndefinedBehaviorSanitizer.

- the decoder compiles freestanding, with no undefined symbols;
- the C encoder makes the same bytes as pixzip.py, raw and whole;
- the C decoder decodes them, in buffers of exactly the right size;
- the corpus goes through the CLI and back, against pixzip.py and the PNGs;
- damaged, truncated and random input gets the same verdict from the C
  and Python decoders;
- the corpus's files are the ones this version has always made."""
import atexit
import hashlib
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parent
sys.path.insert(0, str(root / "tests"))
import pixzip  # noqa: E402
import png  # noqa: E402

os.chdir(root)
rng = random.Random(4127)
cc = os.environ.get("CC", "cc")
work = Path(tempfile.mkdtemp(prefix="pixzip-"))
atexit.register(shutil.rmtree, work, True)
strict = ["-std=c11", "-Wall", "-Wextra", "-Werror"]
sanitized = strict + ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
# The digest of the corpus's packed files, in name order.
CORPUS = "644da41e601ab3162c99119c990eeac8fff86962581aa23b139d9e0b7f08df84"


def run(*command, **kw):
    return subprocess.run([str(c) for c in command], check=True, **kw)


# ---------------------------------------------------------------- build

run(cc, *strict, "-Os", "-ffreestanding", "-nostdlib", "-c", "pixzip.c", "-o", work / "free.o")
if shutil.which("nm"):
    undefined = run("nm", "-u", work / "free.o", capture_output=True, text=True).stdout.split()
    assert not undefined, f"the freestanding decoder needs {undefined}"
run(cc, *sanitized, "tests/native.c", "pixzip.c", "encode.c", "-o", work / "native")
run(cc, *sanitized, "main.c", "pixzip.c", "encode.c", "lodepng/lodepng.c", "-o", work / "pixzip")
cli = work / "pixzip"

# ---------------------------------------------------------------- cases

cases = bytearray()


def u32s(*v):
    return struct.pack(f"<{len(v)}I", *v)


def raw(stream, w, h, k):
    """The C decoder must agree with pixzip.py on this raw stream."""
    try:
        px = pixzip.unpack(stream, w, h, k)
        cases.extend(u32s(0, w, h, k, len(stream), 0) + stream + bytes(px))
    except ValueError:
        cases.extend(u32s(0, w, h, k, len(stream), 1) + stream)


def whole(data, capacity=None, count=None):
    """The C decoder must agree with pixzip.py on this file, given buffers
    of the sizes its header asks for unless capacity or count says less."""
    w, h, k = (struct.unpack_from("<HHB", data, 4) if len(data) >= 9 else (8, 8, 0))
    pixels, need = w * h or 1, pixzip_scratch(k + 1)
    short = capacity is not None or count is not None
    capacity, count = pixels if capacity is None else capacity, need if count is None else count
    try:
        px = pixzip.decode(data)[2]
        assert not short
        cases.extend(u32s(1, len(data), capacity, count, 0) + data + bytes(px))
    except (ValueError, AssertionError):
        cases.extend(u32s(1, len(data), capacity, count, 1) + data)


def pixzip_scratch(k):
    return len(pixzip.scratch(k))


def coded(px, w, h, k, palette=None, alpha=b""):
    """Both scan orders and the whole file, from pixzip.py; the C encoder
    and decoder must match them."""
    for reversed in (0, 1):
        stream = pixzip.encode(px, w, h, k, reversed)
        cases.extend(u32s(2, w, h, k, reversed, len(stream)) + bytes(px) + stream)
        cases.extend(u32s(0, w, h, k, len(stream), 0) + stream + bytes(px[::-1] if reversed else px))
    palette = palette or bytes(rng.randrange(256) for _ in range(3 * k))
    data = pixzip.pack(px, w, h, palette, alpha)
    cases.extend(u32s(3, w, h, k, len(alpha), len(data)) + bytes(px) + palette + alpha + data)
    whole(data)
    return data


def picture(w, h, k, kind):
    """Noise, flat runs, or pixel art's mix of both."""
    px = []
    for i in range(w * h):
        x, y = i % w, i // w
        if kind == "noise" or not px:
            px.append(rng.randrange(k))
        elif kind == "runs":
            px.append(px[-1] if x and rng.random() < 0.9 else px[i - w] if y else rng.randrange(k))
        else:
            r = rng.random()
            px.append(px[-1] if x and r < 0.6 else px[i - w] if y and r < 0.85
                      else px[i - w + 1] if y and x + 1 < w and r < 0.9 else rng.randrange(k))
    return px


# The sizes and palettes at every edge: one pixel, one row, one column,
# 1 to 256 entries, every literal width.
small = []
for k in (1, 2, 3, 4, 5, 8, 9, 16, 17, 32, 33, 63, 64, 65, 127, 128, 129, 200, 255, 256):
    for w, h in ((1, 1), (1, 9), (9, 1), (2, 2), (17, 5), (33, 31)):
        for kind in ("noise", "runs", "art"):
            px = picture(w, h, k, kind)
            alpha = bytes(rng.choice((0, 128, 255)) for _ in range(rng.randrange(k + 1)))
            data = coded(px, w, h, k, alpha=alpha)
            if w * h <= 64 and kind == "art" and k in (2, 17, 256):
                small.append(data)
coded(picture(300, 200, 48, "art"), 300, 200, 48)
coded([0] * 65535, 65535, 1, 1)
coded(picture(1, 65535, 3, "runs"), 1, 65535, 3)

# Damage: every truncation and a flipped bit in every byte, at either end.
for data in small:
    for i in range(len(data)):
        whole(data[:i])
        for flip in (1, 0x80):
            damaged = bytearray(data)
            damaged[i] ^= flip
            whole(bytes(damaged))
    whole(data + b"\0")
    whole(data, capacity=len(pixzip.decode(data)[2]) - 1)
    whole(data, count=pixzip_scratch(data[8] + 1) - 1)
# One alpha byte more than there are entries, with sizes that add up.
data = pixzip.pack(picture(4, 4, 3, "art"), 4, 4, bytes(9), bytes(3))
whole(data[:10] + struct.pack("<H", 4) + data[12:28] + b"\0" + data[28:])
# Random streams after the zero byte every stream starts with.
for _ in range(3000):
    w, h, k = rng.randint(1, 12), rng.randint(1, 12), rng.choice((1, 2, 3, 7, 64, 65, 256))
    raw(bytes([0]) + rng.randbytes(rng.randrange(12)), w, h, k)
for k in (0, 257):
    cases.extend(u32s(0, 2, 2, k, 5, 1) + bytes(5))
raw(b"", 2, 2, 2)
raw(b"\1" + bytes(4), 2, 2, 2)

# ---------------------------------------------------------------- the corpus

digest = hashlib.sha256()
for path in sorted(Path("corpus").glob("*.png")):
    packed, back = work / (path.stem + ".pxz"), work / (path.stem + ".png")
    run(cli, "pack", path, packed)
    data = packed.read_bytes()
    digest.update(data)
    w, h, rgba, indexed = png.read(path)
    W, H, px, palette, alpha = pixzip.decode(data)
    k = len(palette) // 3
    colours = [tuple(palette[3 * i:3 * i + 3]) + (alpha[i] if i < len(alpha) else 255,) for i in range(k)]
    assert (W, H) == (w, h) and [colours[v] for v in px] == rgba, f"{path}: packed wrong"
    if indexed:
        assert px == indexed[0] and colours == indexed[1][:k], f"{path}: the palette was not kept"
    assert pixzip.pack(px, w, h, palette, alpha) == data, f"{path}: the CLI's file differs from pixzip.py's"
    whole(data)
    run(cli, "unpack", packed, back)
    assert png.read(back)[2] == rgba, f"{path}: unpacked wrong"
    assert png.read(back)[3][0] == px, f"{path}: unpacked with other indices"
assert digest.hexdigest() == CORPUS, f"the corpus packs to {digest.hexdigest()}"

# ---------------------------------------------------------------- the CLI


def refuses(*command):
    got = subprocess.run([str(c) for c in command], capture_output=True)
    assert got.returncode and got.stderr, f"accepted {command}"


many = [(i & 255, i >> 8, 0, 255) for i in range(257)]
png.write_rgba(work / "many.png", 257, 1, many)
refuses(cli, "pack", work / "many.png", work / "x.pxz")
png.write_rgba(work / "few.png", 257, 1, many[:256] + [many[0]])
run(cli, "pack", work / "few.png", work / "few.pxz")
assert pixzip.decode((work / "few.pxz").read_bytes())[2][-1] == 0
png.write(work / "deep.png", 2, 1, [bytes([1, 1, 2, 3, 3, 3, 4, 4, 9, 9, 9, 9, 7, 7, 7, 7])], 6, 16)
refuses(cli, "pack", work / "deep.png", work / "x.pxz")
png.write(work / "wide.png", 2, 1, [bytes([1, 1, 2, 2, 3, 3, 255, 255, 9, 9, 9, 9, 7, 7, 0, 0])], 6, 16)
run(cli, "pack", work / "wide.png", work / "wide.pxz")
assert pixzip.decode((work / "wide.pxz").read_bytes())[3:] == (bytes([1, 2, 3, 9, 9, 7]), b"\xff\0")
refuses(cli, "pack", "README.md", work / "x.pxz")
(work / "bad.pxz").write_bytes(small[0][:-1])
refuses(cli, "unpack", work / "bad.pxz", work / "x.png")
refuses(cli, "pack", work / "missing.png", work / "x.pxz")
refuses(cli)

# ---------------------------------------------------------------- run them

(work / "cases").write_bytes(cases)
run(work / "native", work / "cases")
print("all tests passed")
