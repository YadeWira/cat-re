"""Backend protocol shared by all inner-format decoders."""
from __future__ import annotations
from dataclasses import dataclass
from typing import Iterable

@dataclass
class DecodedFile:
    """A file recovered from a .qcf payload."""
    name: str          # logical name (relative path inside the archive)
    data: bytes        # raw file bytes

class Backend:
    """Abstract base for payload backends."""
    name: str = "abstract"

    def sniff(self, payload: bytes) -> bool:
        """Return True if this backend can handle `payload`."""
        raise NotImplementedError

    def decode(self, payload: bytes) -> list[DecodedFile]:
        """Decode `payload` into one or more files."""
        raise NotImplementedError

    def encode(self, files: Iterable[DecodedFile]) -> bytes:
        """Encode a sequence of files into a payload."""
        raise NotImplementedError
