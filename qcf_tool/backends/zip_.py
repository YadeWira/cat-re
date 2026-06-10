"""ZIP backend - payload is a standard PKZIP archive."""
from __future__ import annotations
import io
import zipfile
from typing import Iterable
from .base import Backend, DecodedFile

class ZipBackend(Backend):
    name = "zip"

    def sniff(self, payload: bytes) -> bool:
        return payload.startswith(b"PK\x03\x04") or payload.startswith(b"PK\x05\x06")

    def decode(self, payload: bytes) -> list[DecodedFile]:
        out: list[DecodedFile] = []
        with zipfile.ZipFile(io.BytesIO(payload)) as zf:
            for info in zf.infolist():
                if info.is_dir():
                    continue
                out.append(DecodedFile(name=info.filename, data=zf.read(info.filename)))
        return out

    def encode(self, files: Iterable[DecodedFile]) -> bytes:
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            for f in files:
                zf.writestr(f.name, f.data)
        return buf.getvalue()
