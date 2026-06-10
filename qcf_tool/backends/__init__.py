"""Backends for the inner payload of a .qcf archive.

Each backend knows how to:
  * detect whether a payload belongs to it (sniff magic bytes)
  * decode the payload into a list of named files
  * encode a list of files into a payload

Backends are stateless and reentrant.
"""
from .base import Backend, DecodedFile
from .raw import RawBackend
from .zip_ import ZipBackend
from .jp2 import Jp2Backend
from .ole2 import Ole2Backend

__all__ = [
    "Backend",
    "DecodedFile",
    "RawBackend",
    "ZipBackend",
    "Jp2Backend",
    "Ole2Backend",
]
