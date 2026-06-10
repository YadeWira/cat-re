"""Raw passthrough backend - the payload is a single file as-is."""
from __future__ import annotations
from typing import Iterable
from .base import Backend, DecodedFile

class RawBackend(Backend):
    """Treats the entire payload as a single file.

    The default filename is `payload.bin`. Callers can override by passing
    a `name` to the dispatcher.
    """
    name = "raw"

    def sniff(self, payload: bytes) -> bool:
        # Raw is the always-fallback. The dispatcher only routes here if
        # no other backend matched.
        return True

    def decode(self, payload: bytes) -> list[DecodedFile]:
        return [DecodedFile(name="payload.bin", data=payload)]

    def encode(self, files: Iterable[DecodedFile]) -> bytes:
        chunks = list(files)
        if not chunks:
            return b""
        if len(chunks) == 1:
            return chunks[0].data
        # Concatenate with a length prefix would change the format. For
        # raw we just concatenate - the consumer is expected to know the
        # layout.
        return b"".join(f.data for f in chunks)
