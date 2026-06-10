"""`.qcf` container format - parser and serializer.

Layout
------
    +0x00  DWORD  magic           'QCF\\x01'  (0x01464351 LE)  or
                                  'QCM\\x01'  (0x014D4351 LE)  - multi-volume
                                  'PK\\x03\\x04'                - raw ZIP accepted
    +0x04  BYTE[23] fields       type-specific, opaque for now
    +0x1B  BYTE   ext_hdr_size    N bytes of extended metadata follow the header
    +0x1C  BYTE[] ext_header      N bytes - filename, etc.
    +0x1C+N       payload         inner stream (jp2, zip, ole2, ...)

NOTE (ground truth, 2026-06): the original engine's `CompressFile` actually
emits a **QCM** container whose *member streams* use this 28-byte QCF header
(inner+0x08 = compressed size, inner+0x18 = codec id), followed by a central
directory. The real, validated parser lives in `qcf_tool/qcm.py` and is tested
against `tests/fixtures/real_qcf/`. This module remains the low-level
header primitive; prefer `qcm.QcmArchive` for reading real archives.
See `docs/RE_verified.md` for the byte-exact layout.
"""
from __future__ import annotations
from dataclasses import dataclass
from typing import BinaryIO

MAGIC_QCF = b"QCF\x01"   # 0x01464351 LE
MAGIC_QCM = b"QCM\x01"   # 0x014D4351 LE  (multi-volume variant)
MAGIC_ZIP = b"PK\x03\x04"

# QCArch.dll @ 0x1001f828 does IStream::Read(p, 0x1C, &n) and validates
# that exactly 0x1C bytes were read. Then byte[0x1B] is the ext-hdr size.
FIXED_HDR_SIZE = 0x1C
EXT_HDR_FIELD_OFFSET = 0x1B
FIELDS_SIZE = 23   # +0x04..+0x1A inclusive


@dataclass
class QcfHeader:
    """The fixed 28-byte QCF header.

    The 23 middle bytes are kept opaque for now - their meaning is
    type-specific and will be filled in by the Wine harness.
    """
    magic: bytes = MAGIC_QCF
    fields: bytes = b"\x00" * FIELDS_SIZE
    ext_hdr_size: int = 0
    ext_header: bytes = b""

    def __post_init__(self):
        if len(self.fields) != FIELDS_SIZE:
            raise ValueError(f"fields must be {FIELDS_SIZE} bytes, got {len(self.fields)}")
        if not 0 <= self.ext_hdr_size <= 255:
            raise ValueError(f"ext_hdr_size out of range: {self.ext_hdr_size}")
        if len(self.ext_header) != self.ext_hdr_size:
            raise ValueError(
                f"ext_header length {len(self.ext_header)} != ext_hdr_size {self.ext_hdr_size}"
            )

    def to_bytes(self) -> bytes:
        return self.magic + bytes(self.fields) + bytes([self.ext_hdr_size]) + self.ext_header

    @classmethod
    def parse_fixed(cls, buf: bytes) -> "QcfHeader":
        """Parse the fixed 28-byte part. The ext_header is NOT included
        in `buf`; the caller must read it separately with `ext_hdr_size`.
        """
        if len(buf) < FIXED_HDR_SIZE:
            raise ValueError(f"buffer too small: {len(buf)} < {FIXED_HDR_SIZE}")
        magic = buf[:4]
        if magic not in (MAGIC_QCF, MAGIC_QCM, MAGIC_ZIP):
            raise ValueError(f"bad magic: {magic!r}")
        fields = bytes(buf[4:FIXED_HDR_SIZE - 1])
        ext_size = buf[EXT_HDR_FIELD_OFFSET]
        # Bypass __post_init__ - ext_header is set by the caller.
        h = cls.__new__(cls)
        h.magic = magic
        h.fields = fields
        h.ext_hdr_size = ext_size
        h.ext_header = b""
        return h

    def with_ext_header(self, ext: bytes) -> "QcfHeader":
        """Return a copy with the extended header attached."""
        if len(ext) != self.ext_hdr_size:
            raise ValueError(
                f"ext_header length {len(ext)} != ext_hdr_size {self.ext_hdr_size}"
            )
        h = QcfHeader(
            magic=self.magic,
            fields=self.fields,
            ext_hdr_size=self.ext_hdr_size,
            ext_header=bytes(ext),
        )
        return h


@dataclass
class QcfFile:
    """A complete .qcf archive: header + payload stream."""
    header: QcfHeader
    payload: bytes = b""

    @classmethod
    def detect(cls, buf: bytes) -> str:
        if len(buf) < 4:
            return "unknown"
        m = buf[:4]
        if m == MAGIC_QCF: return "qcf"
        if m == MAGIC_QCM: return "qcm"
        if m == MAGIC_ZIP: return "zip"
        return "unknown"

    @classmethod
    def read(cls, fp: BinaryIO) -> "QcfFile":
        head_buf = fp.read(FIXED_HDR_SIZE)
        if len(head_buf) < 4:
            raise ValueError("not a .qcf file (too short)")
        # Allow passthrough of plain ZIP
        if head_buf[:4] == MAGIC_ZIP:
            rest = fp.read()
            return cls(
                header=QcfHeader(magic=MAGIC_ZIP, ext_hdr_size=0),
                payload=head_buf + rest,
            )
        if len(head_buf) < FIXED_HDR_SIZE:
            raise ValueError("not a .qcf file (truncated header)")
        header = QcfHeader.parse_fixed(head_buf)
        ext = fp.read(header.ext_hdr_size)
        if len(ext) < header.ext_hdr_size:
            raise ValueError("truncated extended header")
        header = header.with_ext_header(ext)
        payload = fp.read()
        return cls(header=header, payload=payload)

    def write(self, fp: BinaryIO) -> None:
        fp.write(self.header.to_bytes())
        fp.write(self.payload)

    @classmethod
    def from_inner(cls, inner: bytes, *, magic: bytes = MAGIC_QCF, ext_header: bytes = b"") -> "QcfFile":
        if len(ext_header) > 255:
            raise ValueError("extended header > 255 bytes")
        return cls(
            header=QcfHeader(magic=magic, ext_hdr_size=len(ext_header), ext_header=ext_header),
            payload=inner,
        )
