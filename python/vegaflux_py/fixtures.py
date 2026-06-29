from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from vegaflux_py import DEFAULT_SEED, SCHEMA_VERSION


SOURCE_FIXTURE = Path("data_contracts/fixtures/synthetic_market_events.jsonl")
GOLDEN_FIXTURE = Path("data_contracts/fixtures/normalized_events.golden.jsonl")


def _jsonl(rows: list[dict]) -> str:
    return "\n".join(json.dumps(row, sort_keys=True, separators=(",", ":")) for row in rows) + "\n"


def source_events(seed: int = DEFAULT_SEED) -> list[dict]:
    base_ts = 1_700_000_000_000_000_000
    events = [
        ("add", "BID", 10000, 10, 1001, 0),
        ("add", "ASK", 10005, 12, 2001, 0),
        ("execute", "BID", 10000, 4, 1001, 1001),
        ("cancel", "ASK", 10005, 2, 2001, 2001),
        ("add", "BID", 9999, 7, 1002, 0),
    ]
    rows: list[dict] = []
    for index, (action, side, price, qty, order_id, ref_id) in enumerate(events, start=1):
        rows.append(
            {
                "action": action,
                "channel": "SYNTH-A",
                "event_ts_ns": base_ts + index * 1_000,
                "order_id": order_id,
                "price_ticks": price,
                "quantity": qty,
                "receive_lag_ns": 2_500,
                "reference_order_id": ref_id,
                "seed": seed,
                "sequence_number": index,
                "session": "SYNTH-0001",
                "side": side,
                "symbol": "VGFX",
                "venue": "SYNTH",
            }
        )
    return rows


def normalize_event(row: dict) -> dict:
    event_type = {"add": "ADD", "execute": "EXECUTE", "cancel": "CANCEL"}[row["action"]]
    return {
        "channel": row["channel"],
        "event_ts_ns": row["event_ts_ns"],
        "event_type": event_type,
        "order_id": row["order_id"],
        "price_ticks": row["price_ticks"],
        "quantity": row["quantity"],
        "raw_payload": "",
        "receive_ts_ns": row["event_ts_ns"] + row["receive_lag_ns"],
        "reference_order_id": row["reference_order_id"],
        "schema_version": SCHEMA_VERSION,
        "sequence_number": row["sequence_number"],
        "session": row["session"],
        "side": row["side"],
        "symbol": row["symbol"],
        "tags": {"fixture": "foundation", "seed": str(row["seed"]), "source": "synthetic"},
        "venue": row["venue"],
    }


def golden_events(seed: int = DEFAULT_SEED) -> list[dict]:
    return [normalize_event(row) for row in source_events(seed)]


def fixture_texts(seed: int = DEFAULT_SEED) -> tuple[str, str]:
    return _jsonl(source_events(seed)), _jsonl(golden_events(seed))


def checksum_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def write(root: Path, seed: int = DEFAULT_SEED) -> None:
    source_text, golden_text = fixture_texts(seed)
    (root / SOURCE_FIXTURE).parent.mkdir(parents=True, exist_ok=True)
    (root / SOURCE_FIXTURE).write_bytes(source_text.encode("utf-8"))
    (root / GOLDEN_FIXTURE).write_bytes(golden_text.encode("utf-8"))


def check(root: Path, seed: int = DEFAULT_SEED) -> None:
    source_text, golden_text = fixture_texts(seed)
    expected = {SOURCE_FIXTURE: source_text, GOLDEN_FIXTURE: golden_text}
    for relative_path, expected_text in expected.items():
        actual_path = root / relative_path
        if not actual_path.exists():
            raise SystemExit(f"missing fixture: {actual_path}")
        actual_text = actual_path.read_text(encoding="utf-8")
        if actual_text != expected_text:
            raise SystemExit(f"fixture drift: {actual_path}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)

    if args.write:
        write(args.root, args.seed)
    if args.check:
        check(args.root, args.seed)
    if not args.write and not args.check:
        _, golden_text = fixture_texts(args.seed)
        print(json.dumps({"events": len(golden_events(args.seed)), "sha256": checksum_text(golden_text)}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
