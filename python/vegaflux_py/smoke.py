from __future__ import annotations

import argparse
import json
from pathlib import Path

from vegaflux_py import SCHEMA_VERSION
from vegaflux_py.fixtures import GOLDEN_FIXTURE, fixture_texts


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path.cwd())
    args = parser.parse_args(argv)

    fixture_path = args.root / GOLDEN_FIXTURE
    rows = [json.loads(line) for line in fixture_path.read_text(encoding="utf-8").splitlines() if line]
    if len(rows) != 5:
        raise SystemExit(f"expected 5 golden events, got {len(rows)}")
    if [row["sequence_number"] for row in rows] != [1, 2, 3, 4, 5]:
        raise SystemExit("golden sequence numbers are not deterministic")
    if any(row["schema_version"] != SCHEMA_VERSION for row in rows):
        raise SystemExit("golden schema version mismatch")
    if fixture_path.read_text(encoding="utf-8") != fixture_texts()[1]:
        raise SystemExit("golden fixture does not match generator output")
    print(json.dumps({"fixture": str(fixture_path), "events": len(rows), "status": "pass"}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
