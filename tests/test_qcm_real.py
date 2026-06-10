"""Validate the QCM parser against REAL .qcf samples from the original engine.

Fixtures in tests/fixtures/real_qcf/ were produced by QCArch.dll under Wine
(see harness/sfa.c). These are ground truth — the parser must extract them
byte-exact.
"""
import os
import pytest
from qcf_tool.qcm import QcmArchive, CODEC_DEFLATE, dos_datetime_to_tuple

FIX = os.path.join(os.path.dirname(__file__), "fixtures", "real_qcf")


def _qcf(name):
    with open(os.path.join(FIX, name), "rb") as f:
        return f.read()


@pytest.mark.parametrize("qcf_name,orig_name,expect_size", [
    ("in.txt.qcf", "in.txt", 60),
    ("big.txt.qcf", "big.txt", 20000),
    ("rand.bin.qcf", None, 4096),
    ("real.jpg.qcf", None, 438466),
])
def test_parses_real_qcm(qcf_name, orig_name, expect_size):
    arc = QcmArchive.read(_qcf(qcf_name))
    assert len(arc.members) == 1
    m = arc.members[0]
    assert m.name == qcf_name[:-4]            # e.g. "in.txt"
    assert m.original_size == expect_size


def test_deflate_roundtrip_is_lossless():
    """Members compressed with deflate must inflate to the exact original."""
    for qcf_name, orig_name in [("in.txt.qcf", "in.txt"), ("big.txt.qcf", "big.txt")]:
        arc = QcmArchive.read(_qcf(qcf_name))
        m = arc.members[0]
        assert m.codec == CODEC_DEFLATE
        recovered = m.extract()
        assert recovered == _qcf(orig_name)
        assert len(recovered) == m.original_size


def test_image_member_is_detected_and_not_deflate():
    arc = QcmArchive.read(_qcf("real.jpg.qcf"))
    m = arc.members[0]
    assert m.codec != CODEC_DEFLATE          # image uses the JP2 codec
    assert m.codec_name == "image-jp2"
    # extract() returns the raw codestream (no data loss), shorter than original
    assert 0 < len(m.extract()) < m.original_size


def test_dos_datetime_decodes():
    arc = QcmArchive.read(_qcf("in.txt.qcf"))
    y, mo, da, hh, mm, ss = dos_datetime_to_tuple(arc.members[0].dos_datetime)
    assert 1980 <= y <= 2100 and 1 <= mo <= 12 and 1 <= da <= 31
    assert 0 <= hh < 24 and 0 <= mm < 60 and 0 <= ss < 60


from qcf_tool.qcm import build_qcm_deflate


def test_encoder_reproduces_real_sample_byte_exact():
    """Our QCM encoder must reproduce a real engine .qcf byte-for-byte."""
    raw = _qcf("in.txt")
    mine = build_qcm_deflate(raw, "in.txt", 0x5CCA22A4)
    assert mine == _qcf("in.txt.qcf")


def test_encoder_output_roundtrips_through_reader():
    """A .qcf we build must parse back and extract to the original bytes."""
    raw = b"contenido de prueba para el encoder QCM\n" * 30
    blob = build_qcm_deflate(raw, "prueba.txt")
    arc = QcmArchive.read(blob)
    assert arc.members[0].name == "prueba.txt"
    assert arc.members[0].original_size == len(raw)
    assert arc.members[0].extract() == raw


from qcf_tool.qcm import build_qcm_multi
import os as _os

MULTI = _os.path.join(_os.path.join(_os.path.dirname(__file__), "..", "OLD", "fixtures-multi"), "Choshuku.qcf")


def test_multifile_encoder_roundtrip():
    """Build a multi-file QCM and read all members back byte-exact."""
    files = [("a.txt", b"primer archivo\n" * 7),
             ("b.bin", bytes(range(256)) * 4),
             ("c.txt", b"tercero\n" * 50)]
    arc = QcmArchive.read(build_qcm_multi(files))
    assert len(arc.members) == 3
    got = {m.name: m.extract() for m in arc.members}
    assert got == dict(files)


@pytest.mark.skipif(not _os.path.exists(MULTI), reason="real multi-file sample absent")
def test_reads_real_multifile_archive():
    """Reader extracts all members of the real 5-file Choshuku.qcf."""
    arc = QcmArchive.read(open(MULTI, "rb").read())
    names = {m.name for m in arc.members}
    assert names == {"Choshuku.EXE", "Choshuku.cab", "README.md", "keygen.c", "keygen.exe"}
    for m in arc.members:
        assert len(m.extract()) == m.original_size      # deflate inflates to declared size


MULTI2 = _os.path.join(_os.path.join(_os.path.dirname(__file__), "..", "OLD", "fixtures-multi"), "ChoshukuV2.qcf")


@pytest.mark.skipif(not _os.path.exists(MULTI2), reason="folder multi-file sample absent")
def test_reads_nested_folders():
    """Reader reconstructs nested folder paths (type=0x00 folders, parent ptrs)."""
    arc = QcmArchive.read(open(MULTI2, "rb").read())
    names = {m.name for m in arc.members}
    assert "XD/nocreo.txt" in names          # file inside folder XD
    assert "XD/JAJA/jajaja.txt" in names      # file 2 levels deep
    for m in arc.members:                     # every file inflates to its declared size
        assert len(m.extract()) == m.original_size


from qcf_tool.qcm import build_qcm_office


def test_office_msoc21_roundtrip():
    """Our MSOC21 (whole-file) office writer round-trips through the reader."""
    raw = bytes.fromhex("d0cf11e0a1b11ae1") + b"\x00" * 400 + b"office stream\n" * 25
    arc = QcmArchive.read(build_qcm_office(raw, "doc.doc"))
    assert len(arc.members) == 1
    assert arc.members[0].codec_name == "office"
    assert arc.members[0].extract() == raw


def test_reads_real_engine_office_wholefile():
    """Reader extracts a real engine-made Office .qcf (whole-file MSOC21 variant)."""
    odir = _os.path.join(FIX, "..", "real_office")
    arc = QcmArchive.read(open(_os.path.join(odir, "wholefile.doc.qcf"), "rb").read())
    orig = open(_os.path.join(odir, "wholefile.doc"), "rb").read()
    assert arc.members[0].codec_name == "office"
    assert arc.members[0].extract() == orig
