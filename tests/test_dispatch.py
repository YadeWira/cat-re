"""Tests for the dispatcher / backend routing."""
import io, zipfile
import pytest
from qcf_tool.dispatch import Dispatcher
from qcf_tool.format import QcfFile, MAGIC_QCF, MAGIC_ZIP
from qcf_tool.backends import DecodedFile

def make_zip(items: dict[str, bytes]) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        for name, data in items.items():
            zf.writestr(name, data)
    return buf.getvalue()

def make_ole2() -> bytes:
    # Real OLE2 magic but invalid; just tests sniffing.
    return b"\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1" + b"\x00" * 100

def test_identify_zip():
    d = Dispatcher()
    assert d.identify(b"PK\x03\x04rest") == "zip"

def test_identify_jp2_soc():
    d = Dispatcher()
    # j2k codestream starts with SOC 0xFF4F
    assert d.identify(b"\xff\x4f\xff\x51" + b"\x00" * 30) == "jp2"

def test_identify_jp2_signature():
    d = Dispatcher()
    # JP2 box container
    jp2 = b"\x00\x00\x00\x0cjP  \x0d\x0a\x87\x0a" + b"\x00" * 200
    assert d.identify(jp2) == "jp2"

def test_identify_ole2():
    d = Dispatcher()
    assert d.identify(make_ole2()) == "ole2"

def test_identify_raw_fallback():
    d = Dispatcher()
    assert d.identify(b"just some text") == "raw"
    assert d.identify(b"") == "raw"

def test_decode_zip_payload():
    payload = make_zip({"a.txt": b"AAA", "b.txt": b"BBB"})
    qcf = QcfFile(header=QcfFile.from_inner(b"").header, payload=payload)
    files = Dispatcher().decode(qcf)
    assert {f.name for f in files} == {"a.txt", "b.txt"}
    assert {f.data for f in files} == {b"AAA", b"BBB"}

def test_decode_passthrough_zip():
    payload = make_zip({"a.txt": b"AAA"})
    qcf = QcfFile(
        header=__import__("qcf_tool.format", fromlist=["QcfHeader"]).QcfHeader(
            magic=MAGIC_ZIP, ext_hdr_size=0
        ),
        payload=payload,
    )
    files = Dispatcher().decode(qcf)
    assert {f.name for f in files} == {"a.txt"}

def test_decode_raw_uses_ext_header():
    """For a raw payload, the ext_header should provide the filename."""
    from qcf_tool.format import QcfHeader, QcfFile
    qcf = QcfFile(
        header=QcfHeader(magic=MAGIC_QCF, ext_hdr_size=8, ext_header=b"data.csv"),
        payload=b"a,b\n1,2\n",
    )
    files = Dispatcher().decode(qcf)
    assert len(files) == 1
    assert files[0].name == "data.csv"
    assert files[0].data == b"a,b\n1,2\n"
