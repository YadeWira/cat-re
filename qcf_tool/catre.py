#!/usr/bin/env python3
"""CAT RE v1.1 — command-line archiver for Choshuku/CAT `.qcf` files.

A free, reverse-engineered reimplementation of the Choshuku Professional
(超圧縮 / QuikCAT CAT) `.qcf` container. Reads and writes the real format
(single-file, multi-file and nested folders) with no dependency on the
original Windows DLLs. Compression backend: DEFLATE (zlib).

Command set and parameters mirror the original software's operations
(compress / extract / list / properties), exposed as a portable CLI.
"""
from __future__ import annotations
import argparse
import os
import sys
import time

from .qcm import (
    QcmArchive, build_qcm_multi, dos_datetime_to_tuple, QcmError,
)

VERSION = "1.1"
BANNER = r"""
  ____    _    _____   ____  _____
 / ___|  / \  |_   _| |  _ \| ____|   CAT RE v%s
| |     / _ \   | |   | |_) |  _|     Choshuku / CAT (.qcf) archiver
| |___ / ___ \  | |   |  _ <| |___    free reverse-engineered build
 \____/_/   \_\ |_|   |_| \_\_____|   (DEFLATE backend, no original DLLs)
""" % VERSION


# ---------------------------------------------------------------- helpers
def _now_dos() -> int:
    """Current local time packed as a DOS date+time DWORD."""
    t = time.localtime()
    yr = max(0, t.tm_year - 1980)
    date = (yr << 9) | (t.tm_mon << 5) | t.tm_mday
    tim = (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2)
    return (date << 16) | tim


def _gather(inputs):
    """Expand files/dirs into [(member_name, data)], preserving folder paths."""
    members = []
    for inp in inputs:
        if os.path.isdir(inp):
            base = os.path.dirname(inp.rstrip("/")) or "."
            for root, _dirs, fnames in os.walk(inp):
                for fn in sorted(fnames):
                    full = os.path.join(root, fn)
                    rel = os.path.relpath(full, base).replace(os.sep, "/")
                    with open(full, "rb") as f:
                        members.append((rel, f.read()))
        else:
            with open(inp, "rb") as f:
                members.append((os.path.basename(inp), f.read()))
    return members


def _fmt_size(n: int) -> str:
    units = ["B", "KB", "MB", "GB"]
    s = float(n)
    for u in units:
        if s < 1024 or u == "GB":
            return f"{int(n)} B" if u == "B" else f"{s:.1f} {u}"
        s /= 1024
    return f"{n} B"


# ---------------------------------------------------------------- commands
def cmd_compress(args):
    members = _gather(args.inputs)
    if not members:
        sys.exit("catre: no input files")
    blob = build_qcm_multi(members, dos_datetime=_now_dos())
    with open(args.output, "wb") as f:
        f.write(blob)
    total_in = sum(len(d) for _, d in members)
    ratio = len(blob) / total_in * 100 if total_in else 0
    if args.quality != 100:
        print(f"note: -q/--quality ({args.quality}) applies to the image (JPEG2000) "
              f"codec of the original engine; this build uses DEFLATE (lossless).")
    if args.verbose:
        for name, data in members:
            print(f"  + {name}  ({_fmt_size(len(data))})")
    print(f"Created {args.output}: {len(members)} file(s), "
          f"{_fmt_size(total_in)} -> {_fmt_size(len(blob))} ({ratio:.1f}%)")


def cmd_extract(args):
    arc = QcmArchive.read(open(args.archive, "rb").read())
    os.makedirs(args.output, exist_ok=True)
    n = 0
    for m in arc.members:
        if args.members and m.name not in args.members:
            continue
        data = m.extract()
        # image members come back as a raw JP2 codestream
        name = m.name if m.codec == 0 else m.name + ".jp2"
        target = os.path.join(args.output, name)
        os.makedirs(os.path.dirname(target) or ".", exist_ok=True)
        with open(target, "wb") as f:
            f.write(data)
        n += 1
        if args.verbose:
            kind = "deflate" if m.codec == 0 else "raw JP2 codestream"
            print(f"  -> {name}  ({_fmt_size(len(data))}, {kind})")
    print(f"Extracted {n} file(s) to {args.output}/")


def cmd_list(args):
    arc = QcmArchive.read(open(args.archive, "rb").read())
    print(f"Archive: {args.archive}  ({len(arc.members)} file(s))")
    if args.verbose:
        print(f"{'size':>11}  {'packed':>11}  {'ratio':>6}  {'codec':<9}  {'modified':<19}  name")
        print("-" * 80)
    for m in arc.members:
        if args.verbose:
            y, mo, da, hh, mn, ss = dos_datetime_to_tuple(m.dos_datetime)
            ratio = m.compressed_size / m.original_size * 100 if m.original_size else 0
            dt = f"{y:04d}-{mo:02d}-{da:02d} {hh:02d}:{mn:02d}:{ss:02d}"
            print(f"{m.original_size:>11}  {m.compressed_size:>11}  {ratio:>5.1f}%  "
                  f"{m.codec_name:<9}  {dt:<19}  {m.name}")
        else:
            print(f"  {m.name}")


def cmd_info(args):
    data = open(args.archive, "rb").read()
    if not QcmArchive.is_qcm(data):
        print(f"{args.archive}: not a QCM/.qcf container (magic={data[:4]!r})")
        sys.exit(1)
    arc = QcmArchive.read(data)
    total_in = sum(m.original_size for m in arc.members)
    print(f"file:           {args.archive}")
    print(f"format:         QCM container (Choshuku/CAT .qcf)")
    print(f"archive size:   {_fmt_size(len(data))} ({len(data)} bytes)")
    print(f"members:        {len(arc.members)}")
    print(f"central dir @:  0x{arc.cdir_offset:x}")
    codecs = sorted({m.codec_name for m in arc.members})
    print(f"codecs used:    {', '.join(codecs)}")
    print(f"uncompressed:   {_fmt_size(total_in)}")
    ratio = len(data) / total_in * 100 if total_in else 0
    print(f"overall ratio:  {ratio:.1f}%")


def cmd_test(args):
    arc = QcmArchive.read(open(args.archive, "rb").read())
    ok = bad = 0
    for m in arc.members:
        try:
            data = m.extract()
            good = (m.codec != 0) or (len(data) == m.original_size)
        except Exception:
            good = False
        if good:
            ok += 1
        else:
            bad += 1
            print(f"  FAILED: {m.name}")
        if args.verbose and good:
            print(f"  OK: {m.name}")
    print(f"Tested {ok + bad} member(s): {ok} OK, {bad} failed.")
    sys.exit(1 if bad else 0)


# ---------------------------------------------------------------- parser
def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="catre",
        description=f"CAT RE v{VERSION} — Choshuku/CAT (.qcf) archiver (free build).",
        epilog="Run 'catre <command> -h' for command-specific help.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("-V", "--version", action="version",
                   version=f"CAT RE v{VERSION}")
    sub = p.add_subparsers(dest="command", metavar="<command>", required=True)

    c = sub.add_parser("compress", aliases=["c"], help="Compress files/folders into a .qcf archive")
    c.add_argument("inputs", nargs="+", metavar="FILE|DIR", help="files or folders to add")
    c.add_argument("-o", "--output", required=True, metavar="ARCHIVE.qcf", help="output archive path")
    c.add_argument("-q", "--quality", type=int, default=100, metavar="0-100",
                   help="image quality (original engine's lQuality; this build uses DEFLATE)")
    c.add_argument("-v", "--verbose", action="store_true", help="list files as they are added")
    c.set_defaults(func=cmd_compress)

    x = sub.add_parser("extract", aliases=["x"], help="Extract files from a .qcf archive")
    x.add_argument("archive", metavar="ARCHIVE.qcf")
    x.add_argument("-o", "--output", default=".", metavar="DIR", help="output directory (default: .)")
    x.add_argument("-m", "--members", nargs="*", metavar="NAME", help="only extract these members")
    x.add_argument("-v", "--verbose", action="store_true")
    x.set_defaults(func=cmd_extract)

    l = sub.add_parser("list", aliases=["l"], help="List archive contents")
    l.add_argument("archive", metavar="ARCHIVE.qcf")
    l.add_argument("-v", "--verbose", action="store_true", help="show sizes, ratio, codec and date")
    l.set_defaults(func=cmd_list)

    i = sub.add_parser("info", aliases=["i"], help="Show archive header and codec details")
    i.add_argument("archive", metavar="ARCHIVE.qcf")
    i.set_defaults(func=cmd_info)

    t = sub.add_parser("test", aliases=["t"], help="Verify archive integrity")
    t.add_argument("archive", metavar="ARCHIVE.qcf")
    t.add_argument("-v", "--verbose", action="store_true")
    t.set_defaults(func=cmd_test)
    return p


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if not argv or argv[0] in ("-h", "--help"):
        print(BANNER)
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        args.func(args)
    except (QcmError, FileNotFoundError, IsADirectoryError) as e:
        sys.exit(f"catre: {e}")


if __name__ == "__main__":
    main()
