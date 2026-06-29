from __future__ import annotations

import argparse
import re
from pathlib import Path


FIELD_RE = re.compile(r"message\s+ExperimentManifest\s*\{(?P<body>.*?)\}", re.DOTALL)
COMPONENT_RE = re.compile(r"\bstring\s+component_id\s*=\s*2\s*;")


def check_schema(path: Path) -> str | None:
    text = path.read_text(encoding="utf-8")
    match = FIELD_RE.search(text)
    if not match:
        return f"{path}: missing ExperimentManifest"
    if not COMPONENT_RE.search(match.group("body")):
        return f"{path}: ExperimentManifest.component_id must remain field 2"
    return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("schemas", nargs="+", type=Path)
    args = parser.parse_args(argv)

    errors = [error for schema in args.schemas if (error := check_schema(schema))]
    if errors:
        for error in errors:
            print(error)
        return 1
    print("manifest component schema check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
