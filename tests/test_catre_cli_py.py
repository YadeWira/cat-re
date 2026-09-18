"""The Python front-end's archive-editing commands, mirroring the C tool's.

The property that matters is the same one: `add` and `delete` carry existing members
over by copying their STORED STREAM, so a member in a codec this front-end cannot
decode survives byte-for-byte.
"""
import hashlib
import os
import shutil
import subprocess
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX = os.path.join(ROOT, "tests", "fixtures", "real_office")


def _run(*args):
    return subprocess.run([sys.executable, "-m", "qcf_tool.catre", *args],
                          cwd=ROOT, capture_output=True, text=True)


def _opaque_stream(path, packed=3417):
    """SHA-256 of the first member's stored stream (SampleSS.xls: office-ps)."""
    d = open(path, "rb").read()
    ext = d[8 + 0x1B]
    return hashlib.sha256(d[8:8 + 0x1C + ext + packed]).hexdigest()


def test_add_and_delete_preserve_an_opaque_member(tmp_path):
    arc = tmp_path / "op.qcf"
    shutil.copy(os.path.join(FIX, "SampleSS.xls.qcf"), arc)
    before = _opaque_stream(arc)

    extra = tmp_path / "extra.txt"
    extra.write_text("hello\n")
    assert _run("add", str(arc), str(extra)).returncode == 0
    assert _opaque_stream(arc) == before, "opaque member altered by add"

    from qcf_tool.qcm import QcmArchive
    names = [m.name for m in QcmArchive.read(arc.read_bytes()).members]
    assert names == ["SampleSS.xls", "extra.txt"], names

    assert _run("delete", str(arc), "extra.txt").returncode == 0
    assert _opaque_stream(arc) == before, "opaque member altered by delete"


def test_members_after_an_opaque_one_are_not_dropped(tmp_path):
    """The stream walk stops at an opaque member; the rest must still be listed."""
    from qcf_tool.qcm import QcmArchive
    arc = tmp_path / "op.qcf"
    shutil.copy(os.path.join(FIX, "SampleSS.xls.qcf"), arc)
    extra = tmp_path / "second.txt"
    extra.write_text("I come after the opaque one\n")
    assert _run("add", str(arc), str(extra)).returncode == 0

    members = QcmArchive.read(arc.read_bytes()).members
    assert len(members) == 2
    assert members[1].extract() == extra.read_bytes()


def test_folders_are_records_not_slashes_in_a_name(tmp_path):
    """Folders exist in the format as records with parent pointers."""
    from qcf_tool.qcm import QcmArchive
    root = tmp_path / "tree"
    (root / "sub" / "deep").mkdir(parents=True)
    (root / "empty").mkdir()
    (root / "sub" / "deep" / "f.txt").write_text("content\n")
    arc = str(tmp_path / "t.qcf")
    assert _run("compress", str(root), "-o", arc).returncode == 0

    a = QcmArchive.read(open(arc, "rb").read())
    assert [m.name for m in a.members] == ["tree/sub/deep/f.txt"]
    assert "tree/empty" in a.folders, a.folders          # the empty one survived

    out = tmp_path / "out"
    assert _run("extract", arc, "-o", str(out)).returncode == 0
    assert (out / "tree" / "empty").is_dir()
    assert (out / "tree" / "sub" / "deep" / "f.txt").read_text() == "content\n"
