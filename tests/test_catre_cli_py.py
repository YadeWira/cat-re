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


def test_ext_header_is_the_basename_initial(tmp_path):
    """The ext header holds the first letter of the BASENAME, not of the path.

    Checked against an engine-made archive: `XD/nocreo.txt` carries `n`, not `X`.
    """
    from qcf_tool.qcm import build_member_stream
    stream = build_member_stream("XD/nocreo.txt", b"content\n")
    ext_len = stream[0x1B]
    assert ext_len == 1
    assert stream[0x1C:0x1C + 1] == b"n"


def test_both_front_ends_write_the_same_archive(tmp_path):
    """C and Python produce identical bytes for the same input (bar the timestamp)."""
    catre_bin = os.path.join(ROOT, "catre")
    if not (os.path.isfile(catre_bin) and os.access(catre_bin, os.X_OK)):
        pytest.skip("catre binary not built")
    src = tmp_path / "tree"
    (src / "sub").mkdir(parents=True)
    (src / "empty").mkdir()
    (src / "a.txt").write_text("hello\n")
    (src / "sub" / "b.txt").write_text("world\n")

    c_arc, py_arc = tmp_path / "c.qcf", tmp_path / "py.qcf"
    assert subprocess.run([catre_bin, "compress", str(src), "-o", str(c_arc),
                           "--no-progress"], cwd=ROOT, capture_output=True).returncode == 0
    assert _run("compress", str(src), "-o", str(py_arc)).returncode == 0

    a, b = c_arc.read_bytes(), py_arc.read_bytes()
    assert len(a) == len(b)
    # only the DOS datetime fields may differ, and only if a second ticked over
    diff = [i for i, (x, y) in enumerate(zip(a, b)) if x != y]
    cdir = a.rfind(b"\x03\x00\x00TOP") - 17
    assert all(i >= cdir for i in diff), f"differences outside the directory: {diff[:8]}"
