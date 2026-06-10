"""qcf-tool: read/write Choshuku (CAT) .qcf archives on modern systems.

The .qcf container is a small binary format defined by the Choshuku
Professional (超圧縮) compressor. It wraps a single inner stream in a
28-byte fixed header plus an optional extended-header block. The inner
stream is one of:

  * a JPEG2000 codestream (jp2)         - for image content
  * a ZIP archive                       - generic fallback
  * an OLE2 compound document           - for Office files

See docs/RE_notes.md for the reverse-engineering context.
"""
from .format import QcfFile, QcfHeader, MAGIC_QCF, MAGIC_QCM, MAGIC_ZIP

__all__ = ["QcfFile", "QcfHeader", "MAGIC_QCF", "MAGIC_QCM", "MAGIC_ZIP"]
__version__ = "0.1.0"
