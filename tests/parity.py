"""Holds Pixzip's raw streams to those of the project it was taken from,
byte for byte, on every picture that project packs.

    python3 tests/parity.py SOURCE [--sizes]
    PIXZIP_SOURCE=SOURCE python3 tests/parity.py

SOURCE is a checkout of that project. Without one, the script says so and
exits cleanly, so it can run anywhere. With one, it runs the project's own
picture packer in this process, with the working directory at SOURCE,
records every picture the packer codes, and checks that pixzip.py and the
C encoder make the same stream from the same indices, choosing the same
scan order. Nothing in SOURCE is written: the packer's caches are copied to
a temporary directory first, and its pieces run one after another rather
than in parallel. --sizes also prints `pixzip compare` totals for the set,
which needs a built ./pixzip and whichever tools it measures."""
import multiprocessing
import os
import runpy
import shutil
import struct
import subprocess
import sys
import tempfile
import types
from pathlib import Path

sys.dont_write_bytecode = True
root = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(root))
sys.path.insert(0, str(root / "tests"))
import pixzip  # noqa: E402
import png  # noqa: E402

args = [a for a in sys.argv[1:] if not a.startswith("--")]
given = args[0] if args else os.environ.get("PIXZIP_SOURCE")
packer = Path("tools/make/pack")
if not given or not (Path(given).expanduser() / packer / "pictures.py").is_file():
    print("parity: skipped, no source checkout (pass its path, or set PIXZIP_SOURCE)")
    sys.exit(0)
source = Path(given).expanduser().resolve()


def capture():
    """[(name, w, h, k, px, reversed, stream)] for each picture the
    packer codes, in its order."""
    os.chdir(source)
    sys.path.insert(0, str(source / packer))
    # Forked workers inherit the patches below; spawned ones would not.
    multiprocessing.set_start_method("fork", force=True)
    import apart
    import pictures
    import portraits
    import things
    build = runpy.run_path(str(packer / "build"))

    keep = Path(tempfile.mkdtemp(prefix="pixzip-parity-"))
    store = portraits.store()
    for name, there in (("portraits", store), ("fits", store and os.path.join(os.path.dirname(store), "fits"))):
        if there and os.path.isdir(there):
            shutil.copytree(there, keep / name)
    portraits.store = lambda: str(keep / "portraits")
    things.kept_dir = lambda: str(keep / "fits")
    apart.apart = lambda calls: [f() for f in calls]

    got, coded = [], pictures.coded

    def record(path, kind, key, w, h, px, cols, mix, clear):
        rec, n = coded(path, kind, key, w, h, px, cols, mix, clear)
        palette = [c for col in cols for c in col]
        for a, b, q in mix:     # a mix of two colours, as one colour for the measure
            palette += [(cols[a][i] * (4 - q) + cols[b][i] * q + 2) // 4 for i in range(3)]
        got.append((os.path.basename(path), w, h, len(cols) + len(mix), list(px), bool(rec[3] & 0x40),
                    bytes(rec[len(rec) - n:]), bytes(palette), clear))
        return rec, n

    pictures.coded = record
    a = types.SimpleNamespace(named=build["named"]("content/words/loop.txt"), report=[])
    for flag, default, *_ in pictures.OPTIONS:
        setattr(a, flag[2:], default() if callable(default) else default)
    pictures.pack(a)
    shutil.rmtree(keep)
    os.chdir(root)
    return got


got = capture()
if not got:
    sys.exit("parity: the packer coded no pictures")
work = Path(tempfile.mkdtemp(prefix="pixzip-"))
cases, bad = bytearray(), 0
for name, w, h, k, px, reversed, stream, _, _ in got:
    ours = pixzip.encode(px, w, h, k, reversed)
    other = pixzip.encode(px, w, h, k, not reversed)
    if ours != stream:
        print(f"parity: {name}: pixzip.py's stream differs")
        bad += 1
    elif len(other) < len(ours) or len(other) == len(ours) and reversed:
        print(f"parity: {name}: pixzip.py chooses the other scan order")
        bad += 1
    cases += struct.pack("<6I", 2, w, h, k, reversed, len(stream)) + bytes(px) + stream
(work / "cases").write_bytes(cases)
cc = os.environ.get("CC", "cc")
subprocess.run([cc, "-std=c11", "-O2", str(root / "tests/native.c"), str(root / "pixzip.c"),
                str(root / "encode.c"), "-o", str(work / "native")], check=True)
if subprocess.run([str(work / "native"), str(work / "cases")]).returncode:
    print("parity: the C encoder's streams differ")
    bad += 1
distinct = len({(w, h, bytes(px)) for _, w, h, _, px, _, _, _, _ in got})
print(f"parity: {len(got) - bad} of {len(got)} pictures match byte for byte ({distinct} distinct), "
      f"{sum(len(s) for *_, s, _, _ in got):,} stream bytes, palettes of {min(g[3] for g in got)} to {max(g[3] for g in got)} entries")

if "--sizes" in sys.argv[1:] and not bad:
    files = []
    for i, (name, w, h, k, px, reversed, stream, palette, clear) in enumerate(got):
        path = work / f"{i:03}.png"
        png.write_indexed(path, w, h, px, [tuple(palette[3 * j:3 * j + 3]) + ((0 if clear and not j else 255),)
                                           for j in range(k)])
        files.append(str(path))
    table = subprocess.run([str(root / "pixzip"), "compare", *files], capture_output=True, text=True,
                           check=True).stdout.splitlines()
    print(table[0])
    print(table[-1])
shutil.rmtree(work)
sys.exit(1 if bad else 0)
