"""Test-set lock audit on a synthetic registry and data directory."""

import json
import pathlib
import sqlite3
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import audit  # noqa: E402

SPLITS = {"test": {"days": ["2025-12-12"]}, "files": {"2025-12-12": "S121225-v50.txt.gz", "2025-12-11": "S121125-v50.txt.gz"}}


def registry(d, rows, research=()):
    db = sqlite3.connect(pathlib.Path(d) / "registry.sqlite")
    db.execute("CREATE TABLE runs (name TEXT, config TEXT, test_access INTEGER)")
    db.execute("CREATE TABLE research_runs (script TEXT, inputs TEXT)")
    db.executemany("INSERT INTO runs VALUES (?, ?, ?)", rows)
    db.executemany("INSERT INTO research_runs VALUES (?, ?)", research)
    db.commit()


def test_clean_lab_passes():
    with tempfile.TemporaryDirectory() as d:
        registry(d, [("a", json.dumps({"days": ["2025-12-11"]}), 0)])
        (pathlib.Path(d) / "store-S121125-v50").mkdir()
        r = audit.audit(pathlib.Path(d), SPLITS)
        assert r["pass"], r


def test_each_kind_of_access_fails():
    for rows, research, store in (
            ([("a", json.dumps({"days": ["2025-12-12"]}), 1)], (), None),
            ([("a", json.dumps({"days": ["2025-12-12"]}), 0)], (), None),
            ([], (("x.py", "store-S121225-v50"),), None),
            ([], (), "store-S121225-v50")):
        with tempfile.TemporaryDirectory() as d:
            registry(d, rows, research)
            if store:
                (pathlib.Path(d) / store).mkdir()
            assert not audit.audit(pathlib.Path(d), SPLITS)["pass"]


if __name__ == "__main__":
    for name, f in list(globals().items()):
        if name.startswith("test_"):
            f()
            print(name, "ok")
