"""`qcf-tool` command-line interface."""
from __future__ import annotations
import sys
import json
import click
from pathlib import Path

from .format import QcfFile, QcfHeader, MAGIC_QCF, MAGIC_QCM, MAGIC_ZIP
from .dispatch import Dispatcher, UnknownPayloadError
from .qcm import QcmArchive, dos_datetime_to_tuple

@click.group()
@click.version_option(package_name="qcf-tool")
def main() -> None:
    """qcf-tool - read/write Choshuku (CAT) .qcf archives."""

@main.command()
@click.argument("path", type=click.Path(exists=True, dir_okay=False, path_type=Path))
def info(path: Path) -> None:
    """Print header and detected inner format."""
    data = path.read_bytes()
    kind = QcfFile.detect(data)
    click.echo(f"file:       {path}")
    click.echo(f"detected:   {kind}")
    click.echo(f"size:       {len(data):,} bytes")
    if kind == "unknown":
        click.echo("Not a QCF/QCM/ZIP file.", err=True)
        sys.exit(1)
    if kind == "zip":
        click.echo("(raw ZIP passthrough - no QCF header)")
        return
    if kind == "qcm":
        # Real engine output: QCM container with embedded member streams.
        arc = QcmArchive.read(data)
        click.echo(f"container:  QCM (real engine format)")
        click.echo(f"cdir@:      0x{arc.cdir_offset:x}")
        for i, m in enumerate(arc.members):
            y, mo, da, hh, mn, ss = dos_datetime_to_tuple(m.dos_datetime)
            ratio = (m.compressed_size / m.original_size * 100) if m.original_size else 0
            click.echo(f"  [{i}] {m.name}")
            click.echo(f"      codec:      {m.codec_name} (id={m.codec})")
            click.echo(f"      original:   {m.original_size:,} bytes")
            click.echo(f"      compressed: {m.compressed_size:,} bytes ({ratio:.1f}%)")
            click.echo(f"      modified:   {y:04d}-{mo:02d}-{da:02d} {hh:02d}:{mn:02d}:{ss:02d}")
        return
    qcf = QcfFile.read(path.open("rb"))
    click.echo(f"magic:      {qcf.header.magic!r}")
    click.echo(f"hdr fields: {qcf.header.fields.hex()}")
    click.echo(f"ext size:   {qcf.header.ext_hdr_size}")
    if qcf.header.ext_header:
        click.echo(f"ext data:   {qcf.header.ext_header.hex()}")
        try:
            click.echo(f"ext as txt: {qcf.header.ext_header.decode('utf-8')!r}")
        except UnicodeDecodeError:
            pass
    d = Dispatcher()
    inner = d.identify(qcf.payload)
    click.echo(f"inner:      {inner}")
    click.echo(f"payload:    {len(qcf.payload):,} bytes")

@main.command()
@click.argument("path", type=click.Path(exists=True, dir_okay=False, path_type=Path))
@click.option("-o", "--out", type=click.Path(file_okay=False, path_type=Path), default=Path("."),
              help="Output directory (default: current).")
@click.option("-v", "--verbose", is_flag=True, help="Verbose output.")
def extract(path: Path, out: Path, verbose: bool) -> None:
    """Extract a .qcf archive."""
    data = path.read_bytes()
    out.mkdir(parents=True, exist_ok=True)

    if QcmArchive.is_qcm(data):
        # Real engine format: extract each member. Deflate members are
        # lossless; image members yield the raw JP2 codestream (caller can
        # decode with the jp2 backend).
        arc = QcmArchive.read(data)
        n = 0
        for m in arc.members:
            payload = m.extract()
            name = m.name if m.codec == 0 else m.name + ".jp2"
            target = out / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(payload)
            n += 1
            if verbose:
                lossless = "lossless" if m.codec == 0 else "raw JP2 codestream"
                click.echo(f"  {name}  ({len(payload):,} bytes, {lossless})  -> {target}")
        click.echo(f"extracted {n} member(s) to {out}")
        return

    qcf = QcfFile.read(path.open("rb"))
    d = Dispatcher()
    files = d.decode(qcf)
    for f in files:
        target = out / f.name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(f.data)
        if verbose:
            click.echo(f"  {f.name}  ({len(f.data):,} bytes)  -> {target}")
    click.echo(f"extracted {len(files)} file(s) to {out}")

@main.command()
@click.argument("inputs", type=click.Path(exists=True, path_type=Path), nargs=-1, required=True)
@click.option("-o", "--out", type=click.Path(dir_okay=False, path_type=Path), required=True,
              help="Output .qcf file path.")
@click.option("-n", "--name", default="payload.bin", help="Logical filename inside the archive.")
def pack(inputs: tuple[Path, ...], out: Path, name: str) -> None:
    """Pack one or more files into a .qcf archive.

    If a single input is given, its bytes become the payload directly. If
    multiple inputs are given, they are wrapped in a ZIP container (the
    CAT engine's generic fallback).
    """
    if len(inputs) == 1:
        payload = inputs[0].read_bytes()
    else:
        from .backends import ZipBackend, DecodedFile
        payload = ZipBackend().encode(DecodedFile(name=p.name, data=p.read_bytes()) for p in inputs)
    qcf = QcfFile.from_inner(payload, magic=MAGIC_QCF, ext_header=name.encode("utf-8"))
    out.write_bytes(qcf.header.to_bytes() + qcf.payload)
    click.echo(f"wrote {out}  ({len(qcf.header.to_bytes()) + len(qcf.payload):,} bytes)")

@main.command()
@click.argument("path", type=click.Path(exists=True, dir_okay=False, path_type=Path))
def sniff(path: Path) -> None:
    """Sniff the inner format of a .qcf archive (JSON output)."""
    qcf = QcfFile.read(path.open("rb"))
    d = Dispatcher()
    click.echo(json.dumps({
        "magic": qcf.header.magic.hex(),
        "ext_hdr_size": qcf.header.ext_hdr_size,
        "inner": d.identify(qcf.payload),
        "payload_size": len(qcf.payload),
    }, indent=2))

if __name__ == "__main__":
    main()
