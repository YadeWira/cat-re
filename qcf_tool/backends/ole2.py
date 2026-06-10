"""OLE2 / MS-CFB backend - payload is a Compound File Binary (Office docs).

CAT used the OLE2 + Deflate (MS-OFFCRYP) path for .doc/.xls/.ppt.
We use `olefile` to enumerate streams. The deflate decompression is
handled transparently by olefile for compressed streams.
"""
from __future__ import annotations
import io
import olefile
from typing import Iterable
from .base import Backend, DecodedFile

OLE2_MAGIC = b"\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1"

class Ole2Backend(Backend):
    name = "ole2"

    def sniff(self, payload: bytes) -> bool:
        return payload.startswith(OLE2_MAGIC)

    def decode(self, payload: bytes) -> list[DecodedFile]:
        out: list[DecodedFile] = []
        ole = olefile.OleFileIO(io.BytesIO(payload))
        try:
            for stream_path in ole.listdir(streams=True, storages=False):
                # `listdir` returns paths as lists of components
                name = "/".join(stream_path)
                data = ole.openstream(stream_path).read()
                out.append(DecodedFile(name=name, data=data))
        finally:
            ole.close()
        return out

    def encode(self, files: Iterable[DecodedFile]) -> bytes:
        # Round-tripping an OLE2 file from individual streams is not
        # supported without rebuilding the CFB layout. Callers that need
        # this should go through `python-oletools` or `compoundfiles`.
        raise NotImplementedError("OLE2 re-encoding not implemented; use raw OLE2 blob")
