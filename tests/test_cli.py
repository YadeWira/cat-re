"""Smoke tests for the CLI."""
import io, zipfile
from pathlib import Path
from click.testing import CliRunner
from qcf_tool.cli import main
from qcf_tool.format import QcfFile, MAGIC_QCF, MAGIC_ZIP

def make_zip(items: dict[str, bytes]) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        for name, data in items.items():
            zf.writestr(name, data)
    return buf.getvalue()

def test_info_qcf(tmp_path: Path):
    payload = make_zip({"a.txt": b"hi"})
    qcf = QcfFile.from_inner(payload, ext_header=b"a.txt")
    p = tmp_path / "test.qcf"
    p.write_bytes(qcf.header.to_bytes() + qcf.payload)

    r = CliRunner().invoke(main, ["info", str(p)])
    assert r.exit_code == 0
    assert "test.qcf" in r.output
    assert "magic" in r.output
    assert "inner:" in r.output
    assert "zip" in r.output

def test_info_raw_zip(tmp_path: Path):
    payload = make_zip({"a.txt": b"hi"})
    p = tmp_path / "plain.zip"
    p.write_bytes(payload)
    r = CliRunner().invoke(main, ["info", str(p)])
    assert r.exit_code == 0
    assert "raw ZIP passthrough" in r.output

def test_info_unknown(tmp_path: Path):
    p = tmp_path / "weird.bin"
    p.write_bytes(b"random bytes here")
    r = CliRunner().invoke(main, ["info", str(p)])
    assert r.exit_code != 0
    assert "Not a QCF" in r.output

def test_extract_qcf(tmp_path: Path):
    payload = make_zip({"a.txt": b"hello", "b.txt": b"world"})
    qcf = QcfFile.from_inner(payload)
    src = tmp_path / "in.qcf"
    src.write_bytes(qcf.header.to_bytes() + qcf.payload)
    out = tmp_path / "out"
    r = CliRunner().invoke(main, ["extract", str(src), "-o", str(out), "-v"])
    assert r.exit_code == 0
    assert (out / "a.txt").read_bytes() == b"hello"
    assert (out / "b.txt").read_bytes() == b"world"

def test_pack_single(tmp_path: Path):
    src = tmp_path / "hello.txt"
    src.write_bytes(b"hello world")
    out = tmp_path / "test.qcf"
    r = CliRunner().invoke(main, ["pack", str(src), "-o", str(out), "-n", "hello.txt"])
    assert r.exit_code == 0
    assert out.exists()
    # Verify the produced file decodes back
    qcf = QcfFile.read(out.open("rb"))
    assert qcf.header.magic == MAGIC_QCF
    assert qcf.payload == b"hello world"
    assert qcf.header.ext_header == b"hello.txt"

def test_pack_multi_uses_zip(tmp_path: Path):
    a = tmp_path / "a.txt"; a.write_bytes(b"A")
    b = tmp_path / "b.txt"; b.write_bytes(b"B")
    out = tmp_path / "test.qcf"
    r = CliRunner().invoke(main, ["pack", str(a), str(b), "-o", str(out)])
    assert r.exit_code == 0
    # payload should start with PK
    qcf = QcfFile.read(out.open("rb"))
    assert qcf.payload.startswith(b"PK\x03\x04")

def test_sniff_json(tmp_path: Path):
    payload = make_zip({"a.txt": b"hi"})
    qcf = QcfFile.from_inner(payload)
    p = tmp_path / "t.qcf"
    p.write_bytes(qcf.header.to_bytes() + qcf.payload)
    r = CliRunner().invoke(main, ["sniff", str(p)])
    assert r.exit_code == 0
    assert "\"inner\"" in r.output
    assert "\"zip\"" in r.output
