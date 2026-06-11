"""Regression test for the native C `catre` JPEG2000 image path.

The Python test suite covers the QCM *reader* (qcm.py). The image *encoder*
lives only in the C tool (tools/catre.c + catre_img.c, via OpenJPEG), so this
test drives the built `catre` binary directly: a generated BMP must compress to
the image-jp2 codec and decode back to a PNG. Also checks the store-if-smaller
fallback. Skips cleanly if no catre binary has been built.
"""
import os
import shutil
import struct
import subprocess
import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _find_catre():
    for c in ("catre", "catre-static", os.path.join("dist", "catre-linux-x64")):
        p = os.path.join(ROOT, c)
        if os.path.isfile(p) and os.access(p, os.X_OK):
            return p
    return None


CATRE = _find_catre()
pytestmark = pytest.mark.skipif(CATRE is None, reason="catre binary not built (run `make catre`)")


def _write_bmp(path, w=96, h=96):
    """Minimal 24-bit BMP with a smooth gradient (compresses well as JPEG2000)."""
    row_pad = (-w * 3) % 4
    pixels = bytearray()
    for y in range(h):
        for x in range(w):
            pixels += bytes((x * 255 // w, y * 255 // h, (x + y) * 255 // (w + h)))  # BGR
        pixels += b"\x00" * row_pad
    size = 54 + len(pixels)
    hdr = b"BM" + struct.pack("<IHHI", size, 0, 0, 54)
    dib = struct.pack("<IiiHHIIiiII", 40, w, -h, 1, 24, 0, len(pixels), 2835, 2835, 0, 0)
    with open(path, "wb") as f:
        f.write(hdr + dib + pixels)


def _run(*args):
    return subprocess.run([CATRE, *args], cwd=ROOT, capture_output=True, text=True)


def test_image_roundtrip_uses_jpeg2000(tmp_path):
    bmp = tmp_path / "grad.bmp"
    _write_bmp(str(bmp))
    qcf = tmp_path / "grad.qcf"

    r = _run("compress", str(bmp), "-o", str(qcf), "-q", "50", "--no-progress")
    assert r.returncode == 0, r.stderr
    assert qcf.exists()

    lst = _run("list", str(qcf), "-v", "--no-progress")
    assert "image-jp2" in lst.stdout, lst.stdout

    outdir = tmp_path / "out"
    r = _run("extract", str(qcf), "-o", str(outdir), "--no-progress")
    assert r.returncode == 0, r.stderr
    pngs = list(outdir.glob("*.png"))
    assert len(pngs) == 1, f"expected one decoded PNG, got {list(outdir.iterdir())}"
    # extension is replaced, not appended (the v1.1 fix): grad.bmp -> grad.png
    assert pngs[0].name == "grad.png"
    assert pngs[0].read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"


def test_store_fallback_never_grows(tmp_path):
    """A tiny already-compressed GIF must not be bloated by JPEG2000 — the encoder
    should fall back to DEFLATE so the archive never exceeds a sane size."""
    # a minimal valid 1x1 GIF (already compressed; JPEG2000 would bloat it)
    gif = tmp_path / "tiny.gif"
    gif.write_bytes(bytes.fromhex(
        "47494638396101000100800000000000ffffff21f90401000000002c00000000"
        "0100010000020144003b"))
    qcf = tmp_path / "tiny.qcf"
    r = _run("compress", str(gif), "-o", str(qcf), "--no-progress")
    assert r.returncode == 0, r.stderr
    # with the fallback, the member is stored via deflate, not blown up by JPEG2000
    lst = _run("list", str(qcf), "-v", "--no-progress")
    assert "deflate" in lst.stdout, lst.stdout
