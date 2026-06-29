from __future__ import annotations

import argparse
import compileall
import json
import subprocess
import sys
import tomllib
from pathlib import Path

from vegaflux_py.schema_compat import compatibility_errors


TEXT_SUFFIXES = {
    ".cmake",
    ".cpp",
    ".csv",
    ".h",
    ".hpp",
    ".json",
    ".md",
    ".proto",
    ".py",
    ".toml",
    ".txt",
    ".yml",
    ".yaml",
}
FINAL_NEWLINE_EXEMPTIONS: set[str] = set()


def tracked_files(root: Path) -> list[Path]:
    completed = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard"],
        cwd=root,
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode != 0:
        return [path for path in root.rglob("*") if path.is_file() and ".git" not in path.parts]
    return [root / line for line in completed.stdout.splitlines() if line]


def text_files(root: Path) -> list[Path]:
    return [path for path in tracked_files(root) if path.suffix.lower() in TEXT_SUFFIXES]


def format_check(root: Path) -> list[str]:
    errors: list[str] = []
    for path in text_files(root):
        data = path.read_bytes()
        relative = path.relative_to(root).as_posix()
        if data and relative not in FINAL_NEWLINE_EXEMPTIONS and not data.endswith(b"\n"):
            errors.append(f"{path.relative_to(root)}: missing final newline")
        for index, line in enumerate(data.splitlines(keepends=True), start=1):
            body = line.rstrip(b"\r\n")
            if body.rstrip(b" \t") != body:
                errors.append(f"{path.relative_to(root)}:{index}: trailing whitespace")
    return errors


def lint_check(root: Path) -> list[str]:
    errors: list[str] = []
    if not compileall.compile_dir(root / "python", quiet=1):
        errors.append("python compileall failed")
    for path in text_files(root):
        try:
            if path.suffix == ".json":
                json.loads(path.read_text(encoding="utf-8-sig"))
            elif path.name == "pyproject.toml":
                tomllib.loads(path.read_text(encoding="utf-8"))
        except Exception as exc:  # noqa: BLE001 - stdlib lint command reports the path.
            errors.append(f"{path.relative_to(root)}: {exc}")
    return errors


def static_check(root: Path) -> list[str]:
    return compatibility_errors(
        root / "data_contracts/schemas/canonical_market.v0.1.proto",
        root / "proto/canonical_market.proto",
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--mode", choices=["format", "lint", "static"], required=True)
    args = parser.parse_args(argv)

    root = args.root.resolve()
    checks = {
        "format": format_check,
        "lint": lint_check,
        "static": static_check,
    }
    errors = checks[args.mode](root)
    status = "fail" if errors else "pass"
    payload = {"status": status, "mode": args.mode, "errors": errors}
    output = sys.stderr if errors else sys.stdout
    print(json.dumps(payload, indent=2, sort_keys=True), file=output)
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
