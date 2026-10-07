"""Test-set lock audit (DESIGN.md MS11): no registry run flagged or configured with a locked test
day, no research run whose inputs name a test-day store, and no test-day store ingested.

Usage: python research/audit.py [<data-dir>]
"""

import json
import pathlib
import sqlite3
import sys
import tomllib

ROOT = pathlib.Path(__file__).resolve().parents[1]


def audit(data, splits):
    test_days = set(splits["test"]["days"])
    stores = {"store-" + splits["files"][d].split(".")[0] for d in test_days if d in splits["files"]}
    found = {"flagged": [], "configured": [], "research_inputs": [], "stores": []}
    db_path = data / "registry.sqlite"
    if db_path.exists():
        with sqlite3.connect(db_path) as db:
            for name, cfg, acc in db.execute("SELECT name, config, test_access FROM runs"):
                if acc:
                    found["flagged"].append(name)
                days = set(json.loads(cfg).get("days", []))
                if days & test_days:
                    found["configured"].append(name)
            for script, inputs in db.execute("SELECT script, inputs FROM research_runs"):
                if any(s in (inputs or "") for s in stores):
                    found["research_inputs"].append(script)
    found["stores"] = sorted(s for s in stores if (data / s).exists())
    return {"pass": not any(found.values()), "test_days": sorted(test_days), **found}


def main():
    data = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path.home() / "data"
    splits = tomllib.loads((ROOT / "configs/splits.toml").read_text())
    r = audit(data, splits)
    print(json.dumps(r, indent=1))
    sys.exit(0 if r["pass"] else 1)


if __name__ == "__main__":
    main()
