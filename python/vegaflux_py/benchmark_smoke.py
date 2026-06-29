from __future__ import annotations

import argparse
import hashlib
import json
import statistics
import time
from pathlib import Path

from vegaflux_py import DEFAULT_SEED, SCHEMA_VERSION


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--dataset", type=Path, default=Path("data_contracts/fixtures/normalized_events.golden.jsonl"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    args = parser.parse_args(argv)

    root = args.root.resolve()
    dataset = (root / args.dataset).resolve() if not args.dataset.is_absolute() else args.dataset
    output = (root / args.output).resolve() if not args.output.is_absolute() else args.output
    raw = dataset.read_bytes()
    text = raw.decode("utf-8")
    durations_ns: list[int] = []
    event_count = 0
    for _ in range(7):
        start = time.perf_counter_ns()
        rows = [json.loads(line) for line in text.splitlines() if line]
        digest = hashlib.sha256(raw).hexdigest()
        event_count = len(rows)
        durations_ns.append(time.perf_counter_ns() - start)
    summary = {
        "schema_version": SCHEMA_VERSION,
        "benchmark_id": "fixture_parse_smoke",
        "status": "pass",
        "seed": args.seed,
        "dataset": str(dataset),
        "dataset_checksum": digest,
        "events": event_count,
        "iterations": len(durations_ns),
        "median_parse_ns": int(statistics.median(durations_ns)),
        "min_parse_ns": min(durations_ns),
        "note": "smoke wiring only; not a performance claim",
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"output": str(output), "status": "pass"}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
