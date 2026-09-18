"""Regression tests for the native C `catre`: Office per-stream decode + honest `test`.

Two things the shipping tool got wrong before v1.5 and must not regress:

  1. `extract` skipped EVERY per-stream Office member. But that codec is multi-mode:
     one of its modes stores the whole original file as a single zlib stream and is
     losslessly decodable (docs/QCF_FORMAT_SPEC.md §5). Bug49919.doc.qcf is such an
     archive, produced by the ORIGINAL engine, so it is ground truth.
  2. `test` reported "OK" for members it never decoded — including a corrupted
     JPEG2000 payload. An integrity command that cannot fail is worse than none.

Both paths live only in the C tool, so these drive the built binary directly.
Skips cleanly if no catre binary has been built.
"""
import os
import shutil
import subprocess
import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX = os.path.join(ROOT, "tests", "fixtures", "real_office")
QCF_FIX = os.path.join(ROOT, "tests", "fixtures", "real_qcf")


def _find_catre():
    for c in ("catre", "catre-static", os.path.join("dist", "catre-linux-x64")):
        p = os.path.join(ROOT, c)
        if os.path.isfile(p) and os.access(p, os.X_OK):
            return p
    return None


CATRE = _find_catre()
pytestmark = pytest.mark.skipif(CATRE is None, reason="catre binary not built (run `make catre`)")


def _run(*args):
    return subprocess.run([CATRE, *args], cwd=ROOT, capture_output=True, text=True)


def test_office_per_stream_wholefile_extracts_bit_exact(tmp_path):
    """The whole-file mode of office-ps decodes to the byte-exact original."""
    arc = os.path.join(FIX, "Bug49919.doc.qcf")
    out = tmp_path / "out"
    r = _run("extract", arc, "-o", str(out), "--no-progress")
    assert r.returncode == 0, r.stderr
    # the member name inside the archive is the one passed to the engine at
    # compression time ("o.doc"); the fixture keeps its Apache POI name.
    got = out / "o.doc"
    assert got.is_file(), r.stdout + r.stderr
    with open(os.path.join(FIX, "Bug49919.doc"), "rb") as f:
        assert got.read_bytes() == f.read()


def test_office_per_stream_opaque_mode_is_skipped_not_failed(tmp_path):
    """The structural/sparse modes stay opaque — skipped with a message, archive OK."""
    arc = os.path.join(FIX, "SampleSS.xls.qcf")
    out = tmp_path / "out"
    r = _run("extract", arc, "-o", str(out), "--no-progress")
    assert r.returncode == 0
    assert "SKIP" in r.stderr and "office per-stream" in r.stderr


def test_test_command_decodes_and_reports_ok():
    """A healthy archive of each decodable codec verifies OK."""
    for arc in ("real.jpg.qcf", "big.txt.qcf"):
        r = _run("test", os.path.join(QCF_FIX, arc), "-v")
        assert r.returncode == 0, r.stdout
        assert "OK:" in r.stdout and "failed" in r.stdout
        assert "0 failed" in r.stdout


def test_test_command_detects_a_corrupted_image(tmp_path):
    """The regression that matters: a damaged JPEG2000 payload must FAIL, not pass."""
    src = os.path.join(QCF_FIX, "real.jpg.qcf")
    bad = tmp_path / "corrupt.qcf"
    shutil.copy(src, bad)
    data = bytearray(bad.read_bytes())
    for pos in (100, 5000):                      # inside the codestream, not the header
        data[pos:pos + 4] = b"\xde\xad\xbe\xef"
    bad.write_bytes(bytes(data))

    r = _run("test", str(bad), "-v")
    assert r.returncode != 0, r.stdout
    assert "FAILED" in r.stdout


def test_test_command_skips_what_it_cannot_decode():
    """Opaque proprietary members are reported as skipped, never as OK."""
    r = _run("test", os.path.join(FIX, "SampleSS.xls.qcf"), "-v")
    assert "SKIP" in r.stdout
    assert "1 skipped" in r.stdout
    assert "0 OK" in r.stdout
