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
# Codecs we recognize but cannot always decode (the original engine's own):
CODEC_OFFICE_PS = 3  # MSOC21 per-stream: multi-mode, only its whole-file mode is decodable
CODEC_LEAD = 4       # image sub-codec 0x09 — LEAD CMP/CMW (TIFF, some PNG), third-party
CODEC_IMAGE_X = 5    # image sub-codec that is not JPEG2000 (0x02: GIF, paletted/gray PNG)
CODEC_PDF = 6        # PdfProc: structural PDF payload, not zlib-of-file

_CODEC_NAMES = {CODEC_DEFLATE: "deflate", CODEC_IMAGE: "image-jp2", CODEC_OLE2: "office",
                CODEC_OFFICE_PS: "office-ps", CODEC_LEAD: "lead-cmp",
                CODEC_IMAGE_X: "image-x", CODEC_PDF: "pdf-proc"}

# MSOC21 whole-file header tail (engine wants it present & non-zero; not content-validated)
_MSOC_TAIL = bytes.fromhex("def90b45711be40046cb1fe33400")


class QcmError(ValueError):
    pass


class QcmOpaqueCodec(QcmError):
    """The member uses a codec only the original engine can decode.

    Raised instead of a confusing zlib error, so callers can skip the member and
    carry on with the rest of the archive (that is what `catre extract` does).
    """


def classify_codec(data: bytes, hdr: int) -> int:
    """Codec of the member whose 28-byte QCF header starts at `hdr`.

    Mirrors the C tool (tools/catre.c `classify_codec`), measured against the engine:
    +0x18 tells image from stream; for an image, +0x19 is the sub-codec (0x01 =
    JPEG2000, the only one with an FF4F codestream; 0x02 = GIF / paletted or gray
    PNG; 0x09 = TIFF and some PNG, the LEAD path); for a stream, +0x1A is the family
    (0x04 deflate, 0x02 office, 0x05 PDF), and an office payload of `32 01 12 00` is
    the decodable whole-file mode while any other `32 01 xx 00` is per-stream.
    """
    if hdr + 0x1C > len(data):
        return CODEC_DEFLATE
    c0, c1, c2, ext = data[hdr + 0x18], data[hdr + 0x19], data[hdr + 0x1A], data[hdr + 0x1B]
    pay = hdr + 0x1C + ext
    if c0 == 0x01:
        if c1 == 0x01:
            return CODEC_IMAGE
        return CODEC_LEAD if c1 == 0x09 else CODEC_IMAGE_X
    if c2 == 0x05:
        return CODEC_PDF
    if data[pay:pay + 2] == b"\x32\x01":
        return CODEC_OLE2 if data[pay:pay + 4] == b"\x32\x01\x12\x00" else CODEC_OFFICE_PS
    return CODEC_DEFLATE


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
    inner: bytes = b""          # the member's stored stream (QCF header + ext + payload),
                                # copied verbatim by `add`/`delete` so that members in a
                                # codec we cannot decode survive a rewrite untouched

    @property
    def codec_name(self) -> str:
        return _CODEC_NAMES.get(self.codec, f"codec{self.codec}")

    @property
    def decodable(self) -> bool:
        """False for members only the original engine can decode.

        office-ps is a maybe: one of its modes is plain zlib of the whole file, so
        that one *is* decodable — `extract` finds out by trying.
        """
        return self.codec in (CODEC_DEFLATE, CODEC_IMAGE, CODEC_OLE2, CODEC_OFFICE_PS, CODEC_PDF)

    def extract(self) -> bytes:
        """Return the decompressed member bytes.

        Deflate members are inflated with zlib (lossless, verified). Image
        members carry a JPEG2000 codestream — we return it as-is so a caller
        with OpenJPEG (the jp2 backend) can decode it; raising would lose data.
        Members in a codec we cannot decode raise `QcmOpaqueCodec` so the caller
        can skip them instead of dying on a bogus zlib error.
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
        if self.codec in (CODEC_OFFICE_PS, CODEC_PDF):
            whole = self._wholefile_zlib()
            if whole is not None:
                return whole
            raise QcmOpaqueCodec(
                f"{self.name}: {self.codec_name} in a structural mode — needs the original "
                "Choshuku engine (which does not restore it byte-exact either)")
        if self.codec == CODEC_LEAD:
            raise QcmOpaqueCodec(
                f"{self.name}: LEAD CMP/CMW (third-party) — needs the original Choshuku engine")
        if self.codec == CODEC_IMAGE_X:
            raise QcmOpaqueCodec(
                f"{self.name}: image member without a JPEG2000 codestream (engine image "
                "codec) — needs the original Choshuku engine")
        # Image: hand back the raw inner codestream for an OpenJPEG-capable caller.
        return self._payload

    def _wholefile_zlib(self) -> bytes | None:
        """Decode the whole-file mode of a structural codec, or None if not that mode.

        Both the office per-stream codec and PdfProc have a mode that stores the entire
        original file as one zlib stream inside their payload, so we scan for a zlib
        header and accept the inflate only when it yields exactly `original_size` bytes
        (docs/QCF_FORMAT_SPEC.md §5).
        """
        pay = self._payload
        for z in range(len(pay) - 1):
            # zlib header: CMF 0x78 (deflate, 32K window) + the RFC 1950 FCHECK rule
            # (the CMF/FLG pair is a multiple of 31). Keeps us from inflating noise.
            if pay[z] != 0x78 or ((pay[z] << 8) | pay[z + 1]) % 31:
                continue
            try:
                out = zlib.decompressobj().decompress(pay[z:], self.original_size + 1)
            except zlib.error:
                continue
            if len(out) == self.original_size:
                return out
        return None


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


def build_member_stream(name: str, raw: bytes) -> bytes:
    """One member's stored stream: QCF header + ext byte + deflate payload.

    The ext header holds the first letter of the member's BASENAME — checked against
    an engine-made archive: `XD/nocreo.txt` carries `n`, not `X`.
    """
    comp = zlib.compress(raw, 9)
    ext = name.rsplit("/", 1)[-1].encode("utf-8")[:1]
    inner = (
        MAGIC_QCF + b"\x00" * 4 + struct.pack("<I", len(comp)) + b"\x00" * 4
        + struct.pack("<I", 0x0011001E)
        + bytes.fromhex("01000400") + bytes([CODEC_DEFLATE, 0x05, 0x04, len(ext)])
    )
    assert len(inner) == 0x1C
    return inner + ext + comp


def write_qcm(members: list, dirs=(), dos_datetime: int = 0x5CCA22A4) -> bytes:
    """Assemble an archive from member streams plus folder records.

    `members` = [{"name", "orig", "dt", "inner"}], where `inner` is the member's
    stored stream — built from a file, or copied verbatim out of another archive so
    that a member in a codec we cannot decode survives untouched.

    Folders are real records (type 0x00) with the parent pointers the format uses,
    not slashes inside a file name: that is how the engine writes them, and it is
    the only way an EMPTY folder can exist at all. Record order is depth first —
    a folder's files, then each subfolder followed by its contents — matching the
    engine's own archives.
    """
    out = bytearray(b"QCM\x01\x00\x00\x00\x00")   # QCM header; [+04] patched below
    stream_offsets = []
    for i, m in enumerate(members):
        chunk = m["inner"]
        if i == 0:
            stream_offsets.append(len(out) - 4)                  # hdr-4 → 0x04
            out += chunk
            struct.pack_into("<I", out, 0x04, len(out) - 4)      # [+04] = end(stream1)-4
        else:
            stream_offsets.append(len(out))                      # prefix position = hdr-4
            out += struct.pack("<I", 4 + len(chunk)) + chunk

    cdir_off = len(out)
    out += b"\x00" * 9 + struct.pack("<I", dos_datetime) + b"\x00" * 4
    out += bytes([3, 0, 0]) + b"TOP"

    all_dirs = []
    def note_dir(d):
        if d and d not in all_dirs:
            all_dirs.append(d)
    for m in members:
        parts = m["name"].split("/")[:-1]
        for i in range(len(parts)):
            note_dir("/".join(parts[:i + 1]))
    for d in dirs:
        parts = d.split("/")
        for i in range(len(parts)):
            note_dir("/".join(parts[:i + 1]))

    def emit(prefix, parent):
        for m, so in zip(members, stream_offsets):
            head, _, base = m["name"].rpartition("/")
            if head != prefix:
                continue
            nm = base.encode("utf-8")
            out.extend(struct.pack("<II", parent, so) + bytes([2])
                       + struct.pack("<I", m["dt"]) + struct.pack("<I", m["orig"])
                       + bytes([len(nm)]) + b"\x00\x00" + nm)
        for d in all_dirs:
            head, _, base = d.rpartition("/")
            if head != prefix:
                continue
            myoff = len(out)
            nm = base.encode("utf-8")
            out.extend(struct.pack("<II", parent, 0) + bytes([0])
                       + struct.pack("<I", dos_datetime) + struct.pack("<I", 0)
                       + bytes([len(nm)]) + b"\x00\x00" + nm)
            emit(d, myoff)

    emit("", cdir_off)
    return bytes(out)


def build_qcm_multi(files: list, dos_datetime: int = 0x5CCA22A4, dirs=()) -> bytes:
    """Build a multi-file QCM archive (deflate codec). `files` = [(name, raw), ...].

    VALIDATED: parses back through QcmArchive.read() and the layout matches the
    real 5-file Choshuku.qcf (stream prefixes, directory records, offsets).
    """
    members = [{"name": name, "orig": len(raw), "dt": dos_datetime,
                "inner": build_member_stream(name, raw)} for name, raw in files]
    return write_qcm(members, dirs, dos_datetime)


@dataclass
class QcmArchive:
    cdir_offset: int
    members: list[QcmMember]
    folders: list = None        # folder records, so an EMPTY folder survives a rewrite

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
            ext_size = data[hdr + 0x1B]
            payload_off = hdr + 0x1C + ext_size
            codec = classify_codec(data, hdr)
            # MSOC21 whole-file office member: comp_size field is 0, real size lives
            # in the 36-byte payload header at +8 (== zlib size); total = 36 + that.
            if codec == CODEC_OLE2 and comp_size == 0:
                comp_size = 36 + struct.unpack_from("<I", data, payload_off + 8)[0]
            if comp_size:
                payload = data[payload_off:payload_off + comp_size]
                if len(payload) != comp_size:
                    raise QcmError("truncated member payload")
            else:
                # Opaque codec (office-ps / LEAD): the header does not tell us the
                # packed size, so keep the rest of the file and let the codec decide.
                payload = data[payload_off:]
            streams[hdr - 4] = dict(codec=codec, comp_size=comp_size,
                                    payload_off=payload_off, hdr=hdr, payload=payload)
            off = payload_off + comp_size
            first = False
        cdir_off = off

        # The stream walk stops at the first member whose packed size the header does
        # not record (the opaque codecs), so where it stopped is not necessarily the
        # directory. Trust it only when it landed on the "TOP" record; otherwise find
        # the last one, exactly as the C reader does.
        marker = b"\x03\x00\x00TOP"
        landed = data[cdir_off + 17:cdir_off + 17 + len(marker)] == marker
        if not landed:
            found = data.rfind(marker)
            if found >= 17:
                cdir_off = found - 17

        # --- parse the central directory ("TOP" root + N item records) ---
        # When the walk stopped early, the members after the opaque one were never
        # walked, so their records must be accepted on their own and their header read
        # at the offset the record points to. Rejecting them (what this reader used to
        # do) silently dropped every member that followed an undecodable one.
        members, folders = cls._parse_directory(data, cdir_off, streams, strict=landed)

        # Stream extents: members sit back to back before the directory, so each one
        # ends where the next begins. That gives an exact length even for codecs whose
        # header does not record the packed size — and the bytes `add`/`delete` copy.
        starts = sorted({m.stream_offset for m in members})
        for m in members:
            nxt = min((s for s in starts if s > m.stream_offset), default=None)
            end = (nxt - 4) if nxt is not None and nxt - 4 > m.stream_offset else cdir_off
            m.inner = data[m.stream_offset:end]
            if not m.compressed_size:
                ext = data[m.stream_offset + 0x1B]
                m.compressed_size = max(0, len(m.inner) - 0x1C - ext)
                m._payload = data[m.payload_offset:m.payload_offset + m.compressed_size]
        return cls(cdir_offset=cdir_off, members=members, folders=folders)

    @staticmethod
    def _parse_directory(data: bytes, cdir_off: int, streams: dict, strict: bool = True):
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
            if strict and item_type == 0x02 and stream_off not in streams:
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

        members, folders = [], []
        for rec_off in order:
            r = records[rec_off]
            if r["type"] != 0x02:
                folders.append(full_path(rec_off))          # folders carry no payload
                continue
            st = streams.get(r["stream_off"])
            if st is None:                                  # derive it from the record
                hdr = r["stream_off"] + 4
                if hdr + 0x1C > len(data):
                    continue
                ext = data[hdr + 0x1B]
                st = dict(codec=classify_codec(data, hdr), comp_size=0,
                          payload_off=hdr + 0x1C + ext, hdr=hdr, payload=b"")
            members.append(QcmMember(
                name=full_path(rec_off), original_size=r["orig"],
                compressed_size=st["comp_size"], codec=st["codec"], dos_datetime=r["dt"],
                stream_offset=st["hdr"], payload_offset=st["payload_off"], _payload=st["payload"],
            ))
        return members, folders
