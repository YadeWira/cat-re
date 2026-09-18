"""Archive-editing commands of the native `catre`: add, delete, -m, empty folders.

These close the functional gap with the original product, whose archive explorer could
add files to an existing `.qcf` and delete members from one. The interesting property
is that both rewrite the archive **from the stored streams**, so a member in a codec we
cannot decode (the engine's per-stream Office, LEAD images, …) survives byte-for-byte.
"""
import os
import shutil
import subprocess
import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX = os.path.join(ROOT, "tests", "fixtures", "real_office")


def _find_catre():
    for c in ("catre", "catre-static", os.path.join("dist", "catre-linux-x64")):
        p = os.path.join(ROOT, c)
        if os.path.isfile(p) and os.access(p, os.X_OK):
            return p
    return None


CATRE = _find_catre()
pytestmark = pytest.mark.skipif(CATRE is None, reason="catre binary not built (run `make catre`)")


def _run(*args, cwd=ROOT):
    return subprocess.run([CATRE, *args], cwd=cwd, capture_output=True, text=True)


def _names(arc):
    """File members only — `list` also prints folder records, with a trailing '/'."""
    r = _run("list", arc)
    return [l.strip() for l in r.stdout.splitlines()[1:]
            if l.strip() and not l.strip().endswith("/")]


def _folders(arc):
    r = _run("list", arc)
    return [l.strip().rstrip("/") for l in r.stdout.splitlines()[1:]
            if l.strip().endswith("/")]


def test_add_then_delete(tmp_path):
    a, b = tmp_path / "a.txt", tmp_path / "b.txt"
    a.write_text("first file\n" * 20)
    b.write_text("second file\n" * 20)
    arc = str(tmp_path / "x.qcf")

    assert _run("compress", str(a), "-o", arc, "--no-progress").returncode == 0
    assert _names(arc) == ["a.txt"]

    assert _run("add", arc, str(b), "--no-progress").returncode == 0
    assert _names(arc) == ["a.txt", "b.txt"]

    assert _run("delete", arc, "a.txt").returncode == 0
    assert _names(arc) == ["b.txt"]

    out = tmp_path / "out"
    assert _run("extract", arc, "-o", str(out), "--no-progress").returncode == 0
    assert (out / "b.txt").read_text() == b.read_text()


def test_add_preserves_a_member_we_cannot_decode(tmp_path):
    """The whole point: rewriting must not touch an opaque member's bytes."""
    arc = tmp_path / "op.qcf"
    shutil.copy(os.path.join(FIX, "SampleSS.xls.qcf"), arc)     # engine-made, office-ps
    before = arc.read_bytes()
    # the stored stream of member 1: QCF header at +8, through its payload
    ext = before[8 + 0x1B]
    payoff = 8 + 0x1C + ext
    stream_before = before[8:payoff + 3417]

    extra = tmp_path / "extra.txt"
    extra.write_text("hello\n")
    assert _run("add", str(arc), str(extra), "--no-progress").returncode == 0

    after = arc.read_bytes()
    assert after[8:payoff + 3417] == stream_before, "opaque member was altered"
    assert _names(str(arc)) == ["SampleSS.xls", "extra.txt"]


def test_delete_removes_a_whole_folder(tmp_path):
    root = tmp_path / "tree"
    (root / "keep").mkdir(parents=True)
    (root / "drop" / "deep").mkdir(parents=True)
    (root / "keep" / "k.txt").write_text("keep me\n")
    (root / "drop" / "deep" / "d.txt").write_text("drop me\n")
    arc = str(tmp_path / "t.qcf")

    assert _run("compress", str(root), "-o", arc, "--no-progress").returncode == 0
    assert sorted(_names(arc)) == ["tree/drop/deep/d.txt", "tree/keep/k.txt"]

    assert _run("delete", arc, "tree/drop").returncode == 0
    assert _names(arc) == ["tree/keep/k.txt"]


def test_extract_selected_members_only(tmp_path):
    root = tmp_path / "t"
    (root / "sub").mkdir(parents=True)
    (root / "one.txt").write_text("one\n")
    (root / "sub" / "two.txt").write_text("two\n")
    arc = str(tmp_path / "s.qcf")
    assert _run("compress", str(root), "-o", arc, "--no-progress").returncode == 0

    out = tmp_path / "just-sub"
    assert _run("extract", arc, "-o", str(out), "-m", "t/sub", "--no-progress").returncode == 0
    assert (out / "t" / "sub" / "two.txt").is_file()
    assert not (out / "t" / "one.txt").exists()


def test_empty_folder_survives_a_round_trip(tmp_path):
    """Only a folder record can express an empty folder; the writer must emit one."""
    root = tmp_path / "e"
    (root / "empty").mkdir(parents=True)
    (root / "full").mkdir()
    (root / "full" / "f.txt").write_text("content\n")
    arc = str(tmp_path / "e.qcf")
    assert _run("compress", str(root), "-o", arc, "--no-progress").returncode == 0

    out = tmp_path / "out"
    assert _run("extract", arc, "-o", str(out), "--no-progress").returncode == 0
    assert (out / "e" / "empty").is_dir(), "the empty folder was dropped"
    assert (out / "e" / "full" / "f.txt").is_file()


def test_more_members_than_the_old_fixed_cap(tmp_path):
    """No silent truncation: the reader used to stop at 4096 members (v1.9).

    Writing 5000 files produced a valid archive from which `list` and `extract`
    returned only 4095 — 905 files lost with no error at all.
    """
    src = tmp_path / "many"
    src.mkdir()
    for i in range(4200):
        (src / f"f{i:05d}.txt").write_text(f"x{i}\n")
    arc = str(tmp_path / "many.qcf")
    assert _run("compress", str(src), "-o", arc, "--no-progress").returncode == 0
    assert len(_names(arc)) == 4200

    out = tmp_path / "out"
    assert _run("extract", arc, "-o", str(out), "--no-progress").returncode == 0
    assert len(list((out / "many").iterdir())) == 4200


@pytest.mark.parametrize("cut", [8, 40, 100, 0.5, 0.9])
def test_truncated_archive_is_rejected_not_crashed(tmp_path, cut):
    """Hostile input must not take the reader out of bounds (found by fuzzing).

    A directory record is 20 bytes before its name; the loop guard said 16 and read
    past the buffer on a short file.
    """
    src = open(os.path.join(FIX, "SampleDoc.doc.qcf"), "rb").read()
    n = int(len(src) * cut) if isinstance(cut, float) else cut
    bad = tmp_path / "cut.qcf"
    bad.write_bytes(src[:n])
    for cmd in (["list", str(bad), "-v"], ["test", str(bad)], ["info", str(bad)],
                ["extract", str(bad), "-o", str(tmp_path / "o"), "--no-progress"]):
        r = _run(*cmd)
        assert r.returncode in (0, 1, 2), f"{cmd[0]} crashed with {r.returncode}"


def test_list_shows_folders_including_empty_ones(tmp_path):
    """An archive of empty folders is not an empty archive (v1.9)."""
    root = tmp_path / "onlydirs"
    (root / "a" / "b").mkdir(parents=True)
    (root / "c").mkdir()
    arc = str(tmp_path / "od.qcf")
    assert _run("compress", str(root), "-o", arc, "--no-progress").returncode == 0

    r = _run("list", arc)
    assert "0 file(s), 4 folder(s)" in r.stdout, r.stdout
    assert sorted(_folders(arc)) == ["onlydirs", "onlydirs/a", "onlydirs/a/b", "onlydirs/c"]
    assert "folders:        4" in _run("info", arc).stdout

    out = tmp_path / "out"
    assert _run("extract", arc, "-o", str(out), "--no-progress").returncode == 0
    assert (out / "onlydirs" / "a" / "b").is_dir()
    assert (out / "onlydirs" / "c").is_dir()


def test_delete_an_empty_folder(tmp_path):
    """Deleting an empty folder removes a folder record and nothing else (v1.9)."""
    root = tmp_path / "tree"
    (root / "keep").mkdir(parents=True)
    (root / "gone").mkdir()
    (root / "keep" / "f.txt").write_text("stay\n")
    arc = str(tmp_path / "t.qcf")
    assert _run("compress", str(root), "-o", arc, "--no-progress").returncode == 0
    assert "tree/gone" in _folders(arc)

    r = _run("delete", arc, "tree/gone", "-v")
    assert r.returncode == 0, r.stderr
    assert "tree/gone" not in _folders(arc)
    assert _names(arc) == ["tree/keep/f.txt"]
