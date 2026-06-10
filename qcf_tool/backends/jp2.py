"""JPEG2000 (JP2) backend - decodes the payload as a single image.

The CAT engine uses JPEG2000 for image content; the on-disk codestream
may be either a raw .j2k codestream (starts with SOC marker 0xFF4F) or
a JP2 box container (starts with the JP2 signature box). Both forms
are accepted.
"""
from __future__ import annotations
import io
from typing import Iterable
from .base import Backend, DecodedFile

SOC = b"\xff\x4f"                                              # raw J2K codestream start
JP2_BOX = b"\x00\x00\x00\x0cjP  \x0d\x0a\x87\x0a"             # full JP2 signature box (12 bytes)

class Jp2Backend(Backend):
    name = "jp2"

    def sniff(self, payload: bytes) -> bool:
        return payload.startswith(SOC) or payload.startswith(JP2_BOX)

    def decode(self, payload: bytes) -> list[DecodedFile]:
        from PIL import Image
        img = Image.open(io.BytesIO(payload))
        # Lossless re-encode to PNG so the user gets a usable file out
        # without needing a JP2 viewer.
        out = io.BytesIO()
        img.save(out, format="PNG")
        return [DecodedFile(name="image.png", data=out.getvalue())]

    def encode(self, files: Iterable[DecodedFile]) -> bytes:
        from PIL import Image
        chunks = list(files)
        if not chunks:
            return b""
        if len(chunks) > 1:
            raise ValueError("jp2 backend expects exactly one input image")
        src = chunks[0]
        img = Image.open(io.BytesIO(src.data))
        out = io.BytesIO()
        try:
            img.save(out, format="JPEG2000", quality_mode="rates", quality_layers=[1.0])
        except Exception as exc:
            raise RuntimeError(f"JP2 encode failed: {exc}") from exc
        return out.getvalue()
