"""Dispatcher - selects a backend for a .qcf payload by sniffing magic bytes.

Backend priority (first match wins):
  1. OLE2    (Office docs)
  2. JP2     (image content)
  3. ZIP     (generic archive fallback - also handles plain-ZIP passthrough)
  4. Raw     (last resort, uses ext_header as the filename)
"""
from __future__ import annotations
from .format import QcfFile, MAGIC_ZIP
from .backends import (
    Backend, RawBackend, ZipBackend, Jp2Backend, Ole2Backend, DecodedFile,
)

class UnknownPayloadError(ValueError):
    pass

class Dispatcher:
    def __init__(self) -> None:
        self._backends: list[Backend] = [
            Ole2Backend(),
            Jp2Backend(),
            ZipBackend(),
            RawBackend(),
        ]

    def identify(self, payload: bytes) -> str:
        for b in self._backends:
            if b.sniff(payload):
                return b.name
        return "raw"

    def backend_for(self, payload: bytes) -> Backend:
        name = self.identify(payload)
        for b in self._backends:
            if b.name == name:
                return b
        return RawBackend()

    def decode(self, qcf: QcfFile) -> list[DecodedFile]:
        # Plain-ZIP passthrough has no payload field - the whole "payload"
        # starts at offset 0.
        if qcf.header.magic == MAGIC_ZIP:
            return ZipBackend().decode(qcf.payload)
        b = self.backend_for(qcf.payload)
        files = b.decode(qcf.payload)
        # For raw payloads, fall back to the extended header for a name
        if b.name == "raw" and files and qcf.header.ext_header:
            name = self._decode_name(qcf.header.ext_header)
            if name:
                files[0] = DecodedFile(name=name, data=files[0].data)
        return files

    def _decode_name(self, ext_header: bytes) -> str | None:
        try:
            txt = ext_header.decode("utf-8").rstrip("\x00")
        except UnicodeDecodeError:
            return None
        if not txt or any(c in txt for c in "\x00\r\n"):
            return None
        return txt

    def encode(self, payload: bytes, *, name_hint: str | None = None) -> QcfFile:
        b = self.backend_for(payload)
        return QcfFile.from_inner(payload)
