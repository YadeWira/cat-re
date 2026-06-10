"""Tests for the .qcf header parser/serializer."""
import io, pytest
from qcf_tool.format import (
    QcfHeader, QcfFile, MAGIC_QCF, MAGIC_QCM, MAGIC_ZIP,
    FIXED_HDR_SIZE,
)

def test_magic_constants():
    assert MAGIC_QCF == b"QCF\x01"
    assert MAGIC_QCM == b"QCM\x01"
    assert MAGIC_ZIP == b"PK\x03\x04"
    # LE: 'Q','C','F','\x01' -> 0x01464351
    assert int.from_bytes(MAGIC_QCF, "little") == 0x01464351
    assert int.from_bytes(MAGIC_QCM, "little") == 0x014D4351

def test_header_roundtrip():
    h = QcfHeader(magic=MAGIC_QCF, fields=b"\x00" * 23, ext_hdr_size=0)
    buf = h.to_bytes()
    assert len(buf) == FIXED_HDR_SIZE
    assert buf[:4] == MAGIC_QCF
    h2 = QcfHeader.parse_fixed(buf[:FIXED_HDR_SIZE]).with_ext_header(buf[FIXED_HDR_SIZE:])
    assert h2.magic == h.magic
    assert h2.fields == h.fields
    assert h2.ext_hdr_size == 0

def test_header_with_extended():
    ext = b"hello.txt"
    h = QcfHeader(magic=MAGIC_QCF, ext_hdr_size=len(ext), ext_header=ext)
    buf = h.to_bytes()
    assert len(buf) == FIXED_HDR_SIZE + len(ext)
    assert buf[FIXED_HDR_SIZE:] == ext
    h2 = QcfHeader.parse_fixed(buf[:FIXED_HDR_SIZE]).with_ext_header(buf[FIXED_HDR_SIZE:])
    assert h2.ext_header == ext

def test_header_field_size():
    with pytest.raises(ValueError):
        QcfHeader(fields=b"\x00" * 10)  # not 23
    with pytest.raises(ValueError):
        QcfHeader(ext_hdr_size=300)      # > 255
    with pytest.raises(ValueError):
        QcfHeader(ext_hdr_size=5, ext_header=b"abc")  # mismatch

def test_detect_qcf():
    assert QcfFile.detect(MAGIC_QCF + b"\x00" * 24) == "qcf"
    assert QcfFile.detect(MAGIC_QCM + b"\x00" * 24) == "qcm"
    assert QcfFile.detect(MAGIC_ZIP + b"\x00" * 24) == "zip"
    assert QcfFile.detect(b"RIFF" + b"\x00" * 24) == "unknown"
    assert QcfFile.detect(b"") == "unknown"

def test_qcffile_read_write():
    payload = b"some inner data"
    h = QcfHeader(magic=MAGIC_QCF, ext_hdr_size=0)
    qcf = QcfFile(header=h, payload=payload)
    buf = io.BytesIO()
    qcf.write(buf)
    buf.seek(0)
    qcf2 = QcfFile.read(buf)
    assert qcf2.header.magic == MAGIC_QCF
    assert qcf2.payload == payload

def test_qcffile_read_passthrough_zip():
    zip_payload = MAGIC_ZIP + b"\x14\x00" + b"\x00" * 20
    buf = io.BytesIO(zip_payload)
    qcf = QcfFile.read(buf)
    assert qcf.header.magic == MAGIC_ZIP
    assert qcf.payload == zip_payload  # whole blob is the payload

def test_qcffile_from_inner():
    qcf = QcfFile.from_inner(b"hi", ext_header=b"foo.txt")
    assert qcf.header.magic == MAGIC_QCF
    assert qcf.header.ext_header == b"foo.txt"
    assert qcf.header.ext_hdr_size == 7
    assert qcf.payload == b"hi"
