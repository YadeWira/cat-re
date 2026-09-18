#!/usr/bin/env python3
"""Conformance harness: the ORIGINAL engine vs `catre`, measured file by file.

Claims like "we read ~90 % of the format" are worth exactly as much as the run that
produced them. This drives both implementations over the same inputs and reports what
actually happened, as a CSV plus a summary:

  for each input file
    1. the ENGINE compresses it            (harness/sfa.exe under Wine)
    2. we read the codec bytes it chose    (+0x18 image/stream, +0x19 image sub-codec,
                                            +0x1A stream family — docs/QCF_FORMAT_SPEC.md §5)
    3. `catre` lists and extracts that archive   -> decoded? byte-exact?
    4. `catre` compresses the same input
    5. the ENGINE decompresses OUR archive       -> byte-exact?

Images are lossy by design on both sides, so for them "byte-exact" is expected to be
false; the columns to read there are `we_decoded` and `engine_read_ours`.

Requirements (none of which ship in the public repo):
  * a 32-bit Wine prefix with the original backends registered (see
    docs/RE_verified.md §7 and the memory note on capturing real .qcf)
  * harness/sfa.exe and harness/dec.exe built (i686-w64-mingw32-gcc)
  * a built `catre`
It exits with a clear message when the engine side is missing, so it is safe to run
anywhere — it simply cannot produce engine columns without the engine.

Usage:
  scripts/engine_matrix.py --corpus /path/to/files --sample 200 --out matrix.csv
  scripts/engine_matrix.py --inputs a.doc b.png --out matrix.csv
"""
from __future__ import annotations
import argparse
import csv
import os
import random
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WINEPREFIX = os.path.expanduser("~/.wine-test")
WINEDIR_WIN = r"C:\qcfmatrix"                       # work dir as Windows sees it
WINEDIR = os.path.join(WINEPREFIX, "drive_c", "qcfmatrix")
CATRE = os.path.join(ROOT, "catre")

# The engine refuses (or takes minutes) on very large inputs; keep the harness brisk.
MAX_INPUT = 8 * 1024 * 1024


def die(msg: str) -> None:
    print(f"engine_matrix: {msg}", file=sys.stderr)
    sys.exit(2)


def check_env() -> None:
    if not os.path.isdir(WINEPREFIX):
        die(f"no Wine prefix at {WINEPREFIX} — see docs/RE_verified.md §7")
    for exe in ("sfa.exe", "dec.exe"):
        if not os.path.isfile(os.path.join(ROOT, "harness", exe)):
            die(f"harness/{exe} missing (build it with i686-w64-mingw32-gcc)")
    if not (os.path.isfile(CATRE) and os.access(CATRE, os.X_OK)):
        die("catre not built — run `make catre`")
    sysdir = os.path.join(WINEPREFIX, "drive_c", "windows", "system32")
    if not os.path.isfile(os.path.join(sysdir, "QCArch.dll")):
        die("the engine DLLs are not installed in the prefix (QCArch.dll missing in system32)")


def wine(args: list[str], timeout: int = 120) -> int:
    env = dict(os.environ, WINEPREFIX=WINEPREFIX, WINEDEBUG="-all")
    try:
        p = subprocess.run(["wine"] + args, cwd=WINEDIR, env=env, timeout=timeout,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return p.returncode
    except subprocess.TimeoutExpired:
        return -1


def catre(args: list[str], timeout: int = 120):
    try:
        return subprocess.run([CATRE] + args, cwd=WINEDIR, capture_output=True,
                              text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(args, -1, "", "timeout")


def codec_bytes(path: str):
    """(+0x18, +0x19, +0x1A, first 4 payload bytes) of the first member, or None."""
    try:
        with open(path, "rb") as f:
            d = f.read(4096)
    except OSError:
        return None
    if d[:4] != b"QCM\x01" or d[8:12] != b"QCF\x01":
        return None
    h = 8
    ext = d[h + 0x1B]
    pay = h + 0x1C + ext
    return d[h + 0x18], d[h + 0x19], d[h + 0x1A], d[pay:pay + 4].hex()


def same_bytes(a: str, b: str) -> bool:
    try:
        with open(a, "rb") as f1, open(b, "rb") as f2:
            return f1.read() == f2.read()
    except OSError:
        return False


def run_one(src: str, quality: int) -> dict:
    """Full round of the matrix for one input file. Returns a CSV row."""
    name = os.path.basename(src)
    row = {"file": name, "size": os.path.getsize(src), "ext": os.path.splitext(name)[1].lower(),
           "engine_codec_bytes": "", "payload_magic": "", "catre_codec": "",
           "engine_compressed": False, "we_decoded": False, "we_byte_exact": False,
           "our_size": 0, "engine_size": 0, "engine_read_ours": False, "note": ""}

    for stale in ("eng.qcf", "our.qcf", "back.out"):
        p = os.path.join(WINEDIR, stale)
        if os.path.exists(p):
            os.remove(p)
    outdir = os.path.join(WINEDIR, "out")
    shutil.rmtree(outdir, ignore_errors=True)
    os.makedirs(outdir, exist_ok=True)

    work = os.path.join(WINEDIR, name)
    if os.path.abspath(src) != os.path.abspath(work):
        shutil.copy(src, work)

    # 1) the engine compresses
    wine(["sfa.exe", f"{WINEDIR_WIN}\\{name}", f"{WINEDIR_WIN}\\eng.qcf", str(quality), name])
    eng = os.path.join(WINEDIR, "eng.qcf")
    if os.path.isfile(eng) and os.path.getsize(eng) > 0:
        row["engine_compressed"] = True
        row["engine_size"] = os.path.getsize(eng)
        cb = codec_bytes(eng)
        if cb:
            row["engine_codec_bytes"] = f"{cb[0]:02x} {cb[1]:02x} {cb[2]:02x}"
            row["payload_magic"] = cb[3]
        # 2) what do WE call it, and can we read it?
        lst = catre(["list", "eng.qcf", "-v", "--no-progress"])
        for line in lst.stdout.splitlines()[2:]:
            parts = line.split()
            if len(parts) >= 4:
                row["catre_codec"] = parts[3]
                break
        ext = catre(["extract", "eng.qcf", "-o", "out", "--no-progress"])
        produced = []
        for base, _dirs, files in os.walk(outdir):
            produced += [os.path.join(base, f) for f in files]
        row["we_decoded"] = bool(produced)
        if produced:
            row["we_byte_exact"] = any(same_bytes(work, p) for p in produced)
        if "SKIP" in ext.stderr:
            row["note"] = "skipped"
        elif "FAILED" in ext.stderr:
            row["note"] = "decode failed"
    else:
        row["note"] = "engine produced nothing"

    # 3) we compress, 4) the engine reads it back
    ours = catre(["compress", name, "-o", "our.qcf", "-q", str(quality), "--no-progress"])
    our = os.path.join(WINEDIR, "our.qcf")
    if ours.returncode == 0 and os.path.isfile(our):
        row["our_size"] = os.path.getsize(our)
        wine(["dec.exe", f"{WINEDIR_WIN}\\our.qcf", f"{WINEDIR_WIN}\\back.out"])
        back = os.path.join(WINEDIR, "back.out")
        if os.path.isfile(back):
            row["engine_read_ours"] = same_bytes(work, back)
    return row


IMAGE_CODECS = {"image-jp2", "image-x", "lead-cmp"}


def summarize(rows: list[dict]) -> None:
    """Byte-exactness is meaningless for image members: both sides are lossy there by
    design, so those columns print as n/a rather than as a failure."""
    by_codec: dict[str, list[dict]] = {}
    for r in rows:
        by_codec.setdefault(r["catre_codec"] or "(none)", []).append(r)

    print()
    print(f"{'codec (as catre sees it)':<26} {'n':>5} {'leído':>7} {'byte-exacto':>12} "
          f"{'motor lee lo nuestro':>21}")
    print("-" * 76)
    for codec, rs in sorted(by_codec.items(), key=lambda kv: -len(kv[1])):
        n = len(rs)
        dec = sum(1 for r in rs if r["we_decoded"])
        if codec in IMAGE_CODECS:
            print(f"{codec:<26} {n:>5} {dec:>6}  {'n/a (lossy)':>11}  {'n/a (lossy)':>20}")
            continue
        exact = sum(1 for r in rs if r["we_byte_exact"])
        back = sum(1 for r in rs if r["engine_read_ours"])
        print(f"{codec:<26} {n:>5} {dec:>6}  {exact:>11}  {back:>20}")
    lossless = [r for r in rows if r["catre_codec"] not in IMAGE_CODECS]
    n = len(rows)
    eng_ok = sum(1 for r in rows if r["engine_compressed"])
    print("-" * 76)
    print(f"{'TOTAL':<26} {n:>5} {sum(1 for r in rows if r['we_decoded']):>6}  "
          f"{sum(1 for r in lossless if r['we_byte_exact']):>11}  "
          f"{sum(1 for r in lossless if r['engine_read_ours']):>20}"
          f"   (exactitud: solo los {len(lossless)} no-imagen)")
    print(f"\nel motor comprimió {eng_ok}/{n} entradas")
    unknown = [r for r in rows if r["engine_compressed"] and not r["we_decoded"]]
    if unknown:
        kinds: dict[str, int] = {}
        for r in unknown:
            kinds[r["engine_codec_bytes"]] = kinds.get(r["engine_codec_bytes"], 0) + 1
        print("no descifrado, por bytes de codec del motor:")
        for k, v in sorted(kinds.items(), key=lambda kv: -kv[1]):
            print(f"  {k or '(sin .qcf)':<12} {v:>5}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--corpus", help="directory to sample input files from")
    ap.add_argument("--sample", type=int, default=50, help="how many files to sample")
    ap.add_argument("--seed", type=int, default=1, help="sampling seed (reproducible runs)")
    ap.add_argument("--inputs", nargs="*", help="explicit input files instead of --corpus")
    ap.add_argument("--quality", type=int, default=100, help="lQuality passed to both sides")
    ap.add_argument("--out", default="engine_matrix.csv", help="CSV to write")
    args = ap.parse_args()

    check_env()
    os.makedirs(WINEDIR, exist_ok=True)
    for exe in ("sfa.exe", "dec.exe"):
        shutil.copy(os.path.join(ROOT, "harness", exe), os.path.join(WINEDIR, exe))

    files: list[str] = []
    if args.inputs:
        files = [f for f in args.inputs if os.path.isfile(f)]
    elif args.corpus:
        pool = []
        for base, _dirs, names in os.walk(args.corpus):
            for n in names:
                p = os.path.join(base, n)
                try:
                    if 0 < os.path.getsize(p) <= MAX_INPUT:
                        pool.append(p)
                except OSError:
                    pass
        random.Random(args.seed).shuffle(pool)
        files = pool[:args.sample]
    else:
        die("pass --corpus or --inputs")

    print(f"engine_matrix: {len(files)} file(s), quality={args.quality}")
    rows = []
    for i, f in enumerate(files, 1):
        print(f"\r[{i}/{len(files)}] {os.path.basename(f)[:48]:<48}", end="", flush=True)
        try:
            rows.append(run_one(f, args.quality))
        except Exception as e:                      # one bad file must not sink the run
            print(f"\n  ! {f}: {e}", file=sys.stderr)
    print()

    with open(args.out, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()) if rows else ["file"])
        w.writeheader()
        w.writerows(rows)
    print(f"CSV -> {args.out}")
    if rows:
        summarize(rows)


if __name__ == "__main__":
    main()
