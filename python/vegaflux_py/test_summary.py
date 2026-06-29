from __future__ import annotations

import argparse
import json
from datetime import datetime, timezone
from pathlib import Path

from vegaflux_py import SCHEMA_VERSION


TESTS = [
    "build_project",
    "contracts_smoke",
    "fixture_check",
    "python_smoke",
    "schema_compatibility",
    "schema_negative_reused_field",
    "schema_policy_negative",
    "manifest_component_schema",
    "manifest_smoke",
    "format_check",
    "lint_check",
    "static_check",
    "foundation_benchmark_smoke",
    "decoder_fixture_check",
    "decoder_cli_smoke",
    "decoder_tests",
    "decoder_benchmark_smoke",
    "replay_tests",
    "replay_cli_snapshot_smoke",
    "replay_benchmark_smoke",
    "feature_tests",
    "feature_dataset_cli_smoke",
    "feature_benchmark_smoke",
    "execution_tests",
    "execution_cli_smoke",
    "execution_benchmark_smoke",
    "options_tests",
    "options_cli_smoke",
    "options_benchmark_smoke",
    "policy_tests",
    "policy_cli_smoke",
    "policy_benchmark_smoke",
]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--status", choices=["pass", "fail"], required=True)
    parser.add_argument("--command", default="ctest --test-dir build/dev --output-on-failure")
    args = parser.parse_args(argv)

    summary = {
        "schema_version": SCHEMA_VERSION,
        "status": args.status,
        "command": args.command,
        "tests": [{"name": name, "result": args.status} for name in TESTS],
        "timestamp_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"output": str(args.output), "status": args.status}, sort_keys=True))
    return 0 if args.status == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
