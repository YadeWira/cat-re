"""Real `.qcf` (QCM container) parser — built from GROUND TRUTH.

This module parses the container that the *original* Choshuku engine actually
emits, captured by driving `QCArch.dll!IQCSingleFileArch::CompressFile` under
Wine (see `harness/sfa.c`). Validated byte-exact against
`tests/fixtures/real_qcf/`.

Unlike the legacy `format.QcfHeader` (which modelled a single flat 28-byte
header), the real output is an *archive*:

    +0x00  QCM header
    +0x08  member stream  = embedded QCF header (28B) + ext header + payload
    ...    (more member streams, in principle)
    end    central directory ("TOP" root entry + one record per item)

QCM header (verified):
    +0x00  DWORD  magic = 0x014D4351  "QCM\\x01"
    +0x04  DWORD  central-directory offset MINUS 4  (dir starts at this+4)

Member QCF stream header (the 28-byte header from docs/RE_verified.md §1):
    inner+0x00  DWORD  magic "QCF\\x01"
    inner+0x08  DWORD  compressed payload size
    inner+0x18  BYTE   codec id   (0 = deflate/zlib, 1 = image/JPEG2000)
    inner+0x1B  BYTE   ext_hdr_size
    inner+0x1C  ext header (ext_hdr_size bytes)
    then        compressed payload (deflate streams start with 78 DA)

Central directory item record (after the "TOP" root marker):
    DWORD  dir_offset      (== end of payload region)
    DWORD  count           (observed 4)
    BYTE   item_type       (2 = file)
    DWORD  dos_datetime    (FileTimeToDosDateTime of compression time)
    DWORD  original_size
    BYTE   name_len  + 2 pad bytes
    char[name_len]  name   (UTF-8)
"""
from __future__ import annotations
from dataclasses import dataclass
import struct
import zlib

MAGIC_QCM = b"QCM\x01"
MAGIC_QCF = b"QCF\x01"

CODEC_DEFLATE = 0   # text / generic binary -> zlib
CODEC_IMAGE = 1     # images -> JPEG2000 codestream (CODEC4)
CODEC_OLE2 = 2      # Office/OLE2 -> MSOC21 (36-byte header + zlib of whole compound file)

_CODEC_NAMES = {CODEC_DEFLATE: "deflate", CODEC_IMAGE: "image-jp2", CODEC_OLE2: "office"}

# MSOC21 whole-file header tail (engine wants it present & non-zero; not content-validated)
_MSOC_TAIL = bytes.fromhex("def90b45711be40046cb1fe33400")


class QcmError(ValueError):
    pass


def dos_datetime_to_tuple(dt: int) -> tuple[int, int, int, int, int, int]:
    """(year, month, day, hour, minute, second) from a packed DOS date+time."""
    date, time = dt >> 16, dt & 0xFFFF
    return (
        ((date >> 9) & 0x7F) + 1980,
        (date >> 5) & 0x0F,
        date & 0x1F,
        (time >> 11) & 0x1F,
        (time >> 5) & 0x3F,
        (time & 0x1F) * 2,
    )


@dataclass
class QcmMember:
    name: str
    original_size: int
    compressed_size: int
    codec: int
    dos_datetime: int
    stream_offset: int          # file offset of the embedded QCF header
    payload_offset: int         # file offset of the compressed payload
    _payload: bytes             # the raw compressed payload bytes

    @property
    def codec_name(self) -> str:
        return _CODEC_NAMES.get(self.codec, f"codec{self.codec}")

    def extract(self) -> bytes:
        """Return the decompressed member bytes.

        Deflate members are inflated with zlib (lossless, verified). Image
        members carry a JPEG2000 codestream — we return it as-is so a caller
        with OpenJPEG (the jp2 backend) can decode it; raising would lose data.
        """
        if self.codec == CODEC_DEFLATE:
            try:
                return zlib.decompress(self._payload)
            except zlib.error as e:  # pragma: no cover - defensive
                raise QcmError(f"deflate inflate failed: {e}") from e
        if self.codec == CODEC_OLE2:
            # MSOC21 whole-file variant: skip the 36-byte header, inflate the OLE2 file.
            try:
                return zlib.decompress(self._payload[36:])
            except zlib.error as e:
                raise QcmError(f"office inflate failed: {e}") from e
        # Non-deflate (image/other): hand back the raw inner codestream.
        return self._payload


def build_qcm_deflate(raw: bytes, name: str, dos_datetime: int = 0x5CCA22A4) -> bytes:
    """Build a single-file QCM archive (deflate codec) from `raw`.

    VALIDATED END-TO-END: a `.qcf` produced by this function is decompressed
    byte-identically by the *original* Choshuku engine (see harness/dec.c).
    Also reproduces `tests/fixtures/real_qcf/in.txt.qcf` byte-for-byte.

    Layout per docs/QCF_FORMAT_SPEC.md: QCM header + embedded QCF stream header
    (28B, codec=deflate) + ext byte (first char of name) + zlib payload +
    central directory ("TOP" root entry + one item record).
    """
    comp = zlib.compress(raw, 9)                 # zlib stream (78 DA ...)
    cs = len(comp)
    ext = name.encode("utf-8")[:1]               # engine stores 1st char of name
    inner = (
        MAGIC_QCF + b"\x00" * 4 + struct.pack("<I", cs) + b"\x00" * 4
        + struct.pack("<I", 0x0011001E)
        + bytes.fromhex("01000400") + bytes([CODEC_DEFLATE, 0x05, 0x04, len(ext)])
    )
    assert len(inner) == 0x1C
    payload_end = 8 + 0x1C + len(ext) + cs
    qcm = MAGIC_QCM + struct.pack("<I", payload_end - 4)
    body = qcm + inner + ext + comp
    nm = name.encode("utf-8")
    trailing = (
        b"\x00" * 9
        + struct.pack("<I", dos_datetime) + b"\x00" * 4
        + bytes([3, 0, 0]) + b"TOP" + struct.pack("<I", payload_end)   # root "TOP" entry
        + struct.pack("<I", 4) + bytes([2]) + struct.pack("<I", dos_datetime)
        + struct.pack("<I", len(raw)) + bytes([len(nm)]) + b"\x00\x00" + nm
    )
    return body + trailing


def build_qcm_office(raw: bytes, name: str, dos_datetime: int = 0x5CCA22A4) -> bytes:
    """Build a single-file Office (MSOC21 whole-file) QCM archive from an OLE2 file.

    VALIDATED: the *original* engine decompresses this byte-exact. Payload = 36-byte
    MSOC21 header + zlib(whole OLE2 file); inner QCF header carries the source size,
    comp_size=0, office tail bytes.
    """
    z = zlib.compress(raw, 9)
    hdr36 = (bytes.fromhex("320112000000") + bytes.fromhex("3302")
             + struct.pack("<I", len(z)) + b"\x00" * 6
             + bytes.fromhex("040a0005") + _MSOC_TAIL)
    payload = hdr36 + z
    ext = name.encode("utf-8")[:1]
    inner = (
        MAGIC_QCF + struct.pack("<I", len(raw)) + struct.pack("<I", 0) + b"\x00" * 4
        + struct.pack("<I", 0x0011001E)
        + bytes.fromhex("0100040000000201")        # office tail (codec byte 0, +19=0, +1a=2)
    )
    assert len(inner) == 0x1C
    payload_end = 8 + 0x1C + len(ext) + len(payload)
    body = MAGIC_QCM + struct.pack("<I", payload_end - 4) + inner + ext + payload
    nm = name.encode("utf-8")
    trailing = (
        b"\x00" * 9 + struct.pack("<I", dos_datetime) + b"\x00" * 4
        + bytes([3, 0, 0]) + b"TOP" + struct.pack("<I", payload_end)
        + struct.pack("<I", 4) + bytes([2]) + struct.pack("<I", dos_datetime)
        + struct.pack("<I", len(raw)) + bytes([len(nm)]) + b"\x00\x00" + nm
    )
    return body + trailing


def build_qcm_multi(files: list, dos_datetime: int = 0x5CCA22A4) -> bytes:
    """Build a multi-file QCM archive (deflate codec). `files` = [(name, raw), ...].

    VALIDATED: parses back through QcmArchive.read() and the layout matches the
    real 5-file Choshuku.qcf (stream prefixes, directory records, offsets).
    """
    out = bytearray(b"QCM\x01\x00\x00\x00\x00")   # QCM header; [+04] patched below
    stream_offsets = []                            # (hdr-4) per file, for dir records
    comps = []
    for i, (name, raw) in enumerate(files):
        comp = zlib.compress(raw, 9)
        comps.append(comp)
        ext = name.encode("utf-8")[:1]
        inner = (
            MAGIC_QCF + b"\x00" * 4 + struct.pack("<I", len(comp)) + b"\x00" * 4
            + struct.pack("<I", 0x0011001E)
            + bytes.fromhex("01000400") + bytes([CODEC_DEFLATE, 0x05, 0x04, len(ext)])
        )
        chunk = inner + ext + comp
        if i == 0:
            stream_offsets.append(len(out) - 4)     # hdr-4 → 0x04
            out += chunk
            struct.pack_into("<I", out, 0x04, len(out) - 4)  # QCM[+04] = end(stream1)-4
        else:
            prefix = 4 + len(chunk)
            stream_offsets.append(len(out))         # prefix position = hdr-4
            out += struct.pack("<I", prefix) + chunk

    cdir_off = len(out)
    out += b"\x00" * 9 + struct.pack("<I", dos_datetime) + b"\x00" * 4
    out += bytes([3, 0, 0]) + b"TOP"
    for (name, raw), so in zip(files, stream_offsets):
        nm = name.encode("utf-8")
        out += struct.pack("<I", cdir_off) + struct.pack("<I", so) + bytes([2])
        out += struct.pack("<I", dos_datetime) + struct.pack("<I", len(raw))
        out += bytes([len(nm)]) + b"\x00\x00" + nm
    return bytes(out)


@dataclass
class QcmArchive:
    cdir_offset: int
    members: list[QcmMember]

    @classmethod
    def is_qcm(cls, data: bytes) -> bool:
        return data[:4] == MAGIC_QCM

    @classmethod
    def read(cls, data: bytes) -> "QcmArchive":
        """Parse a QCM container (single- OR multi-file). VALIDATED against a
        real 5-file archive (Choshuku.qcf). Layout: stream 1 at +0x08 (no
        prefix); streams 2..N each preceded by a 4-byte chunk size; central
        directory at the end ("TOP" root + one record per item).
        """
        if data[:4] != MAGIC_QCM:
            raise QcmError(f"not a QCM container: magic={data[:4]!r}")
        if len(data) < 0x08 + 0x1C:
            raise QcmError("truncated QCM header")

        # --- walk member streams sequentially ---
        # stream 1 header at +0x08; subsequent streams have a 4-byte size prefix.
        streams: dict[int, dict] = {}          # keyed by stream_offset (= hdr-4)
        off, first = 0x08, True
        while off + 0x1C <= len(data):
            hdr = off if first else off + 4
            if data[hdr:hdr + 4] != MAGIC_QCF:
                break                          # reached the central directory
            comp_size = struct.unpack_from("<I", data, hdr + 0x08)[0]
            codec = data[hdr + 0x18]
            ext_size = data[hdr + 0x1B]
            payload_off = hdr + 0x1C + ext_size
            # MSOC21 whole-file office member: comp_size field is 0, real size lives
            # in the 36-byte payload header at +8 (== zlib size); total = 36 + that.
            if comp_size == 0 and codec == 0 and data[payload_off:payload_off + 4] == b"\x32\x01\x12\x00":
                comp_size = 36 + struct.unpack_from("<I", data, payload_off + 8)[0]
                codec = CODEC_OLE2
            payload = data[payload_off:payload_off + comp_size]
            if len(payload) != comp_size:
                raise QcmError("truncated member payload")
            streams[hdr - 4] = dict(codec=codec, comp_size=comp_size,
                                    payload_off=payload_off, hdr=hdr, payload=payload)
            off = payload_off + comp_size
            first = False
        cdir_off = off

        # --- parse the central directory ("TOP" root + N item records) ---
        members = cls._parse_directory(data, cdir_off, streams)
        return cls(cdir_offset=cdir_off, members=members)

    @staticmethod
    def _parse_directory(data: bytes, cdir_off: int, streams: dict) -> list:
        p = cdir_off
        # TOP root entry: 9 zeros + datetime(4) + 4 zeros + [namelen=3][00 00]"TOP"
        try:
            p += 9
            p += 4                                          # datetime (root)
            p += 4                                          # zeros
            top_namelen = data[p]; p += 3                   # namelen + 2 pad
            if data[p:p + top_namelen] != b"TOP":
                # fall back: locate TOP if layout differs
                t = data.find(b"TOP", cdir_off)
                if t < 0:
                    raise QcmError("'TOP' root entry not found")
                p = t
                top_namelen = 3
            p += top_namelen                                # past "TOP"
        except IndexError as e:
            raise QcmError(f"malformed directory header: {e}") from e

        # item/folder record:
        #   parent_off(4) stream_off(4) type(1) datetime(4) origsize(4)
        #   namelen(1) pad(2) name(namelen)
        # type 0x02 = file (stream_off -> a member stream); 0x00 = folder (no stream).
        # parent_off = file offset of the parent folder's record; root parent = dir start.
        records = {}                                        # rec_off -> dict
        order = []
        root_off = cdir_off                                 # the "TOP" entry's offset
        while p + 16 <= len(data):
            rec_off = p
            try:
                parent_off = struct.unpack_from("<I", data, p)[0]; p += 4
                stream_off = struct.unpack_from("<I", data, p)[0]; p += 4
                item_type = data[p]; p += 1
                dt = struct.unpack_from("<I", data, p)[0]; p += 4
                orig_size = struct.unpack_from("<I", data, p)[0]; p += 4
                name_len = data[p]; p += 1
                p += 2                                      # pad
                name = data[p:p + name_len].decode("utf-8", "replace"); p += name_len
            except (struct.error, IndexError):
                break
            if item_type == 0x02 and stream_off not in streams:
                break                                       # not a valid file record
            records[rec_off] = dict(parent=parent_off, stream_off=stream_off,
                                    type=item_type, dt=dt, orig=orig_size, name=name)
            order.append(rec_off)
        if not records:
            raise QcmError("no valid directory records parsed")

        def full_path(rec_off):
            parts, cur, seen = [], rec_off, set()
            while cur in records and cur not in seen:
                seen.add(cur)
                parts.append(records[cur]["name"])
                cur = records[cur]["parent"]
            return "/".join(reversed(parts))

        members = []
        for rec_off in order:
            r = records[rec_off]
            if r["type"] != 0x02:
                continue                                    # folders carry no payload
            st = streams[r["stream_off"]]
            members.append(QcmMember(
                name=full_path(rec_off), original_size=r["orig"],
                compressed_size=st["comp_size"], codec=st["codec"], dos_datetime=r["dt"],
                stream_offset=st["hdr"], payload_offset=st["payload_off"], _payload=st["payload"],
            ))
        return members
