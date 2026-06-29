from __future__ import annotations

import csv
import gzip
import hashlib
import json
import math
import random
import statistics
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
ART = ROOT / "artifacts" / "report"
FIG = ART / "figures"
MIN_BOOTSTRAP_N = 10
BOOTSTRAP_ITERATIONS = 2000
SEED = 424242
ACCESS_DATE = "2026-06-28"
PUBLIC_IEX_GZ = ROOT / "data_contracts" / "fixtures" / "public_iex" / "20180127_IEXTP1_TOPS1.6.pcap.gz"
PUBLIC_IEX_URL = "https://www.googleapis.com/download/storage/v1/b/iex/o/data%2Ffeeds%2F20180127%2F20180127_IEXTP1_TOPS1.6.pcap.gz?alt=media&generation=1517101257197858"
PUBLIC_IEX_SHA256 = "ecfcef16491d3d6591b869e0db21164ed0fb9d2a491067f87fde40336f842d3b"
PUBLIC_IEX_UNCOMPRESSED_SHA256 = "09fbcf83bb847650bd0c866b4406c07eff4d893bcb4d6c51ff24d53eadd2cf72"


def rel(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


def read_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def read_jsonl(path: Path) -> list[dict[str, Any]]:
    if not path.exists() or path.stat().st_size == 0:
        return []
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def write_csv(path: Path, rows: list[dict[str, Any]], fieldnames: list[str] | None = None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if fieldnames is None:
        keys: list[str] = []
        for row in rows:
            keys.extend(k for k in row if k not in keys)
        fieldnames = keys
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def write_json(path: Path, data: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def row_count(path: Path) -> int:
    if path.suffix == ".jsonl":
        return len(read_jsonl(path))
    if path.suffix == ".csv":
        return len(read_csv(path))
    if path.suffix == ".json":
        data = read_json(path)
        if isinstance(data.get("tests"), list):
            return len(data["tests"])
        if isinstance(data.get("rows"), list):
            return len(data["rows"])
        if isinstance(data.get("fits"), list):
            return len(data["fits"])
    return 1


def configure_matplotlib_svg(matplotlib: Any) -> None:
    matplotlib.rcParams["svg.hashsalt"] = "vegaflux-report"


def save_svg(fig: Any, out: Path) -> None:
    fig.savefig(out, format="svg", metadata={"Date": None})
    lines = out.read_text(encoding="utf-8").splitlines()
    out.write_text("\n".join(line.rstrip() for line in lines) + "\n", encoding="utf-8")


def number(value: Any) -> float | None:
    if value in (None, "", "null", "not_enough_data"):
        return None
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if math.isfinite(result) else None


def bootstrap_mean(values: list[float]) -> tuple[float, float, float]:
    return bootstrap_stat(values, statistics.fmean)


def bootstrap_stat(values: list[float], stat) -> tuple[float, float, float]:
    rng = random.Random(SEED)
    estimates = []
    n = len(values)
    for _ in range(BOOTSTRAP_ITERATIONS):
        sample = [values[rng.randrange(n)] for _ in range(n)]
        estimates.append(stat(sample))
    estimates.sort()
    return (
        stat(values),
        estimates[int(0.025 * (BOOTSTRAP_ITERATIONS - 1))],
        estimates[int(0.975 * (BOOTSTRAP_ITERATIONS - 1))],
    )


def maybe_plot_bar(rows: list[dict[str, Any]], label: str, value: str, out: Path, title: str, ylabel: str) -> dict[str, str]:
    try:
        import matplotlib

        matplotlib.use("Agg")
        configure_matplotlib_svg(matplotlib)
        import matplotlib.pyplot as plt
    except Exception as exc:  # pragma: no cover - optional local plotting
        return {"path": rel(out), "status": "skipped", "reason": f"matplotlib_unavailable:{exc}"}

    pairs = [(str(r[label]), number(r[value])) for r in rows]
    pairs = [(name, val) for name, val in pairs if val is not None]
    if not pairs:
        return {"path": rel(out), "status": "skipped", "reason": "no_numeric_rows"}
    labels = [name for name, _ in pairs]
    values = [val for _, val in pairs]
    out.parent.mkdir(parents=True, exist_ok=True)
    fig, ax = plt.subplots(figsize=(7, 3.5))
    ax.bar(labels, values, color="#4c78a8")
    ax.set_title(title)
    ax.set_ylabel(ylabel)
    ax.tick_params(axis="x", rotation=25)
    ax.grid(axis="y", alpha=0.25)
    fig.tight_layout()
    save_svg(fig, out)
    plt.close(fig)
    return {"path": rel(out), "status": "rendered", "reason": "source_table"}


def maybe_plot_barh(
    rows: list[dict[str, Any]], label: str, value: str, out: Path, title: str, xlabel: str, logx: bool = False
) -> dict[str, str]:
    try:
        import matplotlib

        matplotlib.use("Agg")
        configure_matplotlib_svg(matplotlib)
        import matplotlib.pyplot as plt
        from matplotlib.ticker import LogLocator, NullFormatter, NullLocator
    except Exception as exc:  # pragma: no cover - optional local plotting
        return {"path": rel(out), "status": "skipped", "reason": f"matplotlib_unavailable:{exc}"}

    pairs = [(str(r[label]), number(r[value])) for r in rows]
    pairs = [(name, val) for name, val in pairs if val is not None]
    if not pairs:
        return {"path": rel(out), "status": "skipped", "reason": "no_numeric_rows"}
    labels = [name for name, _ in pairs]
    values = [val for _, val in pairs]
    out.parent.mkdir(parents=True, exist_ok=True)
    fig, ax = plt.subplots(figsize=(7, max(3.5, 0.28 * len(labels) + 1.1)))
    ax.barh(labels, values, color="#4c78a8")
    ax.invert_yaxis()
    ax.set_title(title)
    ax.set_xlabel(xlabel)
    if logx and all(v > 0 for v in values):
        ax.set_xscale("log")
        ax.set_xlabel(f"{xlabel} (log scale)")
        ax.xaxis.set_major_locator(LogLocator(base=10.0, numticks=6))
        ax.xaxis.set_minor_locator(NullLocator())
        ax.xaxis.set_minor_formatter(NullFormatter())
    ax.grid(axis="x", alpha=0.25)
    fig.tight_layout()
    save_svg(fig, out)
    plt.close(fig)
    return {"path": rel(out), "status": "rendered", "reason": "source_table"}


def maybe_plot_policy(rows: list[dict[str, Any]], out: Path) -> dict[str, str]:
    try:
        import matplotlib

        matplotlib.use("Agg")
        configure_matplotlib_svg(matplotlib)
        import matplotlib.pyplot as plt
    except Exception as exc:  # pragma: no cover
        return {"path": rel(out), "status": "skipped", "reason": f"matplotlib_unavailable:{exc}"}

    labels = [r["ablation_id"] for r in rows]
    bids = [float(r["bid"]) for r in rows]
    asks = [float(r["ask"]) for r in rows]
    fig, ax = plt.subplots(figsize=(8, 3.6))
    ax.plot(labels, bids, marker="o", label="bid")
    ax.plot(labels, asks, marker="o", label="ask")
    ax.set_title("Synthetic Policy Quote Outputs")
    ax.set_ylabel("quote price")
    ax.tick_params(axis="x", rotation=25)
    ax.grid(axis="y", alpha=0.25)
    ax.legend()
    fig.tight_layout()
    save_svg(fig, out)
    plt.close(fig)
    return {"path": rel(out), "status": "rendered", "reason": "source_table"}


def maybe_plot_ci(rows: list[dict[str, Any]], out: Path) -> dict[str, str]:
    numeric = [r for r in rows if r["status"] == "computed"]
    if not numeric:
        return {"path": rel(out), "status": "skipped", "reason": "no_computed_intervals"}
    try:
        import matplotlib

        matplotlib.use("Agg")
        configure_matplotlib_svg(matplotlib)
        import matplotlib.pyplot as plt
    except Exception as exc:  # pragma: no cover
        return {"path": rel(out), "status": "skipped", "reason": f"matplotlib_unavailable:{exc}"}

    labels = [r["metric_id"] for r in numeric]
    estimates = [float(r["estimate"]) for r in numeric]
    lows = [float(r["ci_low"]) for r in numeric]
    highs = [float(r["ci_high"]) for r in numeric]
    yerr = [[e - lo for e, lo in zip(estimates, lows)], [hi - e for e, hi in zip(estimates, highs)]]
    fig, ax = plt.subplots(figsize=(7, 3.5))
    ax.errorbar(labels, estimates, yerr=yerr, fmt="o", color="#333333", ecolor="#f58518", capsize=4)
    ax.set_title("Bootstrap Intervals")
    ax.set_ylabel("mean")
    ax.tick_params(axis="x", rotation=20)
    ax.grid(axis="y", alpha=0.25)
    fig.tight_layout()
    save_svg(fig, out)
    plt.close(fig)
    return {"path": rel(out), "status": "rendered", "reason": "source_table"}


def build_data_lineage() -> list[dict[str, Any]]:
    sources = [
        ("features", "artifacts/features-validation/dataset_metadata.json", "chronological split metadata"),
        ("features", "artifacts/features-validation/features.jsonl", "feature rows and labels"),
        ("queue", "artifacts/execution-sensitivity/scenario_sweep.csv", "queue sensitivity table"),
        ("queue", "artifacts/execution-sensitivity/calibration_status.csv", "synthetic truth checks"),
        ("queue", "artifacts/execution-sensitivity/deterministic_fills.jsonl", "fill and markout rows"),
        ("surface", "artifacts/surface-validation/surface_diagnostics.json", "surface safety diagnostics"),
        ("surface", "artifacts/surface-validation/static_arbitrage.csv", "static arbitrage table"),
        ("surface", "artifacts/surface-validation/iv_residuals.csv", "IV residual sample"),
        ("surface", "artifacts/surface-validation/greek_fd_errors.csv", "Greek finite-difference errors"),
        ("policy", "artifacts/policy-validation/ablation_table.csv", "policy ablation outputs"),
        ("policy", "artifacts/policy-validation/pnl_attribution.json", "synthetic attribution"),
        ("policy", "artifacts/policy-validation/risk_gate_events.jsonl", "risk gate events"),
        ("policy", "artifacts/policy-validation/quote_decisions.jsonl", "policy quote decision"),
        ("public_real_data", "data_contracts/fixtures/public_iex/20180127_IEXTP1_TOPS1.6.pcap.gz", "IEX TOPS public sample source"),
        ("public_real_data", "artifacts/report/public_iex_summary.json", "IEX TOPS parser summary"),
        ("public_real_data", "artifacts/report/public_iex_quote_stats.csv", "IEX TOPS symbol diagnostics"),
        ("public_real_data", "artifacts/report/public_iex_quote_spreads.csv", "IEX TOPS valid quote spread observations"),
        ("public_real_data", "artifacts/report/public_iex_tops_events_sample.csv", "IEX TOPS parsed event sample"),
    ]
    rows = []
    for role, source, use in sources:
        path = ROOT / source
        rows.append(
            {
                "role": role,
                "source_path": source,
                "source_type": "repository_artifact",
                "row_count": row_count(path),
                "size_bytes": path.stat().st_size,
                "sha256": sha256_file(path),
                "used_for": use,
                "limitation": "synthetic fixture evidence; no market generalization",
            }
        )
    return rows


def iter_public_iex_messages() -> tuple[bytes, list[dict[str, Any]], dict[str, int]]:
    if not PUBLIC_IEX_GZ.exists():
        raise FileNotFoundError(f"missing public IEX sample: {PUBLIC_IEX_GZ}")
    if sha256_file(PUBLIC_IEX_GZ) != PUBLIC_IEX_SHA256:
        raise ValueError("public IEX compressed sample checksum drift")
    raw = gzip.open(PUBLIC_IEX_GZ, "rb").read()
    if sha256_bytes(raw) != PUBLIC_IEX_UNCOMPRESSED_SHA256:
        raise ValueError("public IEX uncompressed sample checksum drift")

    import struct

    messages: list[dict[str, Any]] = []
    counts: dict[str, int] = {}
    offset = 0
    packet_index = 0
    message_index = 0
    while offset + 12 <= len(raw):
        block_type, block_len = struct.unpack_from("<II", raw, offset)
        if block_len < 12 or offset + block_len > len(raw):
            break
        if block_type == 6:
            captured_len = struct.unpack_from("<I", raw, offset + 20)[0]
            packet = raw[offset + 28 : offset + 28 + captured_len]
            packet_index += 1
            if len(packet) >= 42 and packet[12:14] == b"\x08\x00" and packet[23] == 17:
                ip_header_len = (packet[14] & 0x0F) * 4
                payload = packet[14 + ip_header_len + 8 :]
                if len(payload) >= 40 and payload[0] == 1:
                    cursor = 40
                    while cursor + 2 <= len(payload):
                        msg_len = struct.unpack_from("<H", payload, cursor)[0]
                        cursor += 2
                        if msg_len == 0 or cursor + msg_len > len(payload):
                            break
                        body = payload[cursor : cursor + msg_len]
                        cursor += msg_len
                        msg_type = chr(body[0]) if body else "?"
                        counts[msg_type] = counts.get(msg_type, 0) + 1
                        message_index += 1
                        messages.append(parse_public_iex_body(body, message_index, packet_index))
        offset += block_len
    return raw, messages, counts


def iex_symbol(raw: bytes) -> str:
    return raw.decode("ascii", errors="replace").strip()


def iex_price(value: int) -> float:
    return value / 10000.0


def parse_public_iex_body(body: bytes, message_index: int, packet_index: int) -> dict[str, Any]:
    import struct

    row: dict[str, Any] = {
        "message_index": message_index,
        "packet_index": packet_index,
        "message_type": chr(body[0]) if body else "?",
        "parse_status": "unsupported",
    }
    if len(body) >= 10:
        row["flags"] = body[1]
        row["timestamp_ns"] = struct.unpack_from("<Q", body, 2)[0]
    if row["message_type"] == "Q" and len(body) == 42:
        bid_price = iex_price(struct.unpack_from("<Q", body, 22)[0])
        ask_price = iex_price(struct.unpack_from("<Q", body, 30)[0])
        row.update(
            {
                "parse_status": "parsed",
                "event_type": "quote_update",
                "symbol": iex_symbol(body[10:18]),
                "bid_size": struct.unpack_from("<I", body, 18)[0],
                "bid_price": bid_price,
                "ask_price": ask_price,
                "ask_size": struct.unpack_from("<I", body, 38)[0],
                "spread": ask_price - bid_price if bid_price > 0 and ask_price > 0 and ask_price >= bid_price else "",
                "trade_size": "",
                "trade_price": "",
                "trade_id": "",
            }
        )
    elif row["message_type"] == "T" and len(body) == 38:
        row.update(
            {
                "parse_status": "parsed",
                "event_type": "trade_report",
                "symbol": iex_symbol(body[10:18]),
                "bid_size": "",
                "bid_price": "",
                "ask_price": "",
                "ask_size": "",
                "spread": "",
                "trade_size": struct.unpack_from("<I", body, 18)[0],
                "trade_price": iex_price(struct.unpack_from("<Q", body, 22)[0]),
                "trade_id": struct.unpack_from("<Q", body, 30)[0],
            }
        )
    elif row["message_type"] == "S" and len(body) == 10:
        row.update(
            {
                "parse_status": "parsed",
                "event_type": "system_event",
                "symbol": "",
                "system_event": chr(body[1]),
            }
        )
    elif row["message_type"] == "D" and len(body) == 31:
        row.update(
            {
                "parse_status": "parsed",
                "event_type": "security_directory",
                "symbol": iex_symbol(body[10:18]),
            }
        )
    return row


def public_iex_tables() -> tuple[
    dict[str, Any],
    list[dict[str, Any]],
    list[dict[str, Any]],
    list[dict[str, Any]],
    list[dict[str, Any]],
    list[float],
]:
    raw, messages, counts = iter_public_iex_messages()
    parsed = [m for m in messages if m.get("parse_status") == "parsed"]
    quotes = [m for m in parsed if m.get("event_type") == "quote_update"]
    trades = [m for m in parsed if m.get("event_type") == "trade_report"]
    valid_quotes = [m for m in quotes if number(m.get("spread")) is not None]
    by_symbol: dict[str, dict[str, Any]] = {}
    for row in parsed:
        symbol = row.get("symbol") or "SYSTEM"
        info = by_symbol.setdefault(
            symbol,
            {
                "symbol": symbol,
                "source_path": rel(PUBLIC_IEX_GZ),
                "quote_updates": 0,
                "valid_quotes": 0,
                "trade_reports": 0,
                "spreads": [],
                "trade_sizes": [],
                "trade_prices": [],
                "first_timestamp_ns": row.get("timestamp_ns", ""),
                "last_timestamp_ns": row.get("timestamp_ns", ""),
            },
        )
        if row.get("timestamp_ns") != "":
            info["first_timestamp_ns"] = min(info["first_timestamp_ns"], row["timestamp_ns"])
            info["last_timestamp_ns"] = max(info["last_timestamp_ns"], row["timestamp_ns"])
        if row.get("event_type") == "quote_update":
            info["quote_updates"] += 1
            spread = number(row.get("spread"))
            if spread is not None:
                info["valid_quotes"] += 1
                info["spreads"].append(spread)
        if row.get("event_type") == "trade_report":
            info["trade_reports"] += 1
            size = number(row.get("trade_size"))
            price = number(row.get("trade_price"))
            if size is not None:
                info["trade_sizes"].append(size)
            if price is not None:
                info["trade_prices"].append(price)

    symbol_rows: list[dict[str, Any]] = []
    for symbol, info in sorted(by_symbol.items()):
        if symbol == "SYSTEM":
            continue
        spreads = info["spreads"]
        trade_sizes = info["trade_sizes"]
        trade_prices = info["trade_prices"]
        symbol_rows.append(
            {
                "symbol": symbol,
                "source_path": info["source_path"],
                "quote_updates": info["quote_updates"],
                "valid_quotes": info["valid_quotes"],
                "trade_reports": info["trade_reports"],
                "mean_spread": "" if not spreads else statistics.fmean(spreads),
                "median_spread": "" if not spreads else statistics.median(spreads),
                "mean_trade_size": "" if not trade_sizes else statistics.fmean(trade_sizes),
                "median_trade_price": "" if not trade_prices else statistics.median(trade_prices),
                "first_timestamp_ns": info["first_timestamp_ns"],
                "last_timestamp_ns": info["last_timestamp_ns"],
                "limitation": "IEX TOPS is aggregated top-of-book; no individual queue position",
            }
        )

    spread_rows = [
        {
            "message_index": q["message_index"],
            "timestamp_ns": q["timestamp_ns"],
            "symbol": q["symbol"],
            "bid_price": q["bid_price"],
            "ask_price": q["ask_price"],
            "bid_size": q["bid_size"],
            "ask_size": q["ask_size"],
            "spread": q["spread"],
            "source_path": rel(PUBLIC_IEX_GZ),
            "limitation": "aggregated TOPS best quote; not order-level queue data",
        }
        for q in valid_quotes
    ]

    quote_mids_by_symbol: dict[str, list[tuple[int, float]]] = {}
    for q in valid_quotes:
        bid = number(q.get("bid_price"))
        ask = number(q.get("ask_price"))
        if bid is not None and ask is not None:
            quote_mids_by_symbol.setdefault(q["symbol"], []).append((int(q["timestamp_ns"]), (bid + ask) / 2.0))
    for items in quote_mids_by_symbol.values():
        items.sort()

    import bisect

    markout_rows: list[dict[str, Any]] = []
    deltas: list[float] = []
    for trade in trades:
        symbol = trade["symbol"]
        mids = quote_mids_by_symbol.get(symbol, [])
        if not mids:
            continue
        ts_list = [x[0] for x in mids]
        pos = bisect.bisect_right(ts_list, int(trade["timestamp_ns"]))
        if pos >= len(mids):
            continue
        price = number(trade.get("trade_price"))
        if price is None:
            continue
        next_ts, next_mid = mids[pos]
        delta = next_mid - price
        deltas.append(delta)
        markout_rows.append(
            {
                "symbol": symbol,
                "trade_timestamp_ns": trade["timestamp_ns"],
                "trade_price": price,
                "trade_size": trade["trade_size"],
                "next_quote_timestamp_ns": next_ts,
                "next_mid": next_mid,
                "next_mid_minus_trade": delta,
                "source_path": rel(PUBLIC_IEX_GZ),
                "limitation": "direction unknown in TOPS trade report; not signed P&L markout",
            }
        )

    sample_rows = []
    for row in parsed[:5000]:
        sample_rows.append(
            {
                "message_index": row.get("message_index", ""),
                "message_type": row.get("message_type", ""),
                "event_type": row.get("event_type", ""),
                "timestamp_ns": row.get("timestamp_ns", ""),
                "symbol": row.get("symbol", ""),
                "bid_price": row.get("bid_price", ""),
                "bid_size": row.get("bid_size", ""),
                "ask_price": row.get("ask_price", ""),
                "ask_size": row.get("ask_size", ""),
                "spread": row.get("spread", ""),
                "trade_price": row.get("trade_price", ""),
                "trade_size": row.get("trade_size", ""),
                "trade_id": row.get("trade_id", ""),
                "parse_status": row.get("parse_status", ""),
            }
        )

    summary = {
        "source_id": "iex_tops_20180127_sample",
        "source_url": PUBLIC_IEX_URL,
        "terms_url": "https://www.iex.io/legal/hist-data-terms",
        "access_date": ACCESS_DATE,
        "raw_path": rel(PUBLIC_IEX_GZ),
        "compressed_size_bytes": PUBLIC_IEX_GZ.stat().st_size,
        "compressed_sha256": sha256_file(PUBLIC_IEX_GZ),
        "uncompressed_size_bytes": len(raw),
        "uncompressed_sha256": sha256_bytes(raw),
        "message_count": len(messages),
        "parsed_message_count": len(parsed),
        "message_type_counts": counts,
        "quote_update_count": len(quotes),
        "valid_quote_count": len(valid_quotes),
        "trade_report_count": len(trades),
        "symbol_count": len([r for r in symbol_rows if r["quote_updates"] or r["trade_reports"]]),
        "markout_proxy_count": len(deltas),
        "unsupported_message_policy": "counted, not guessed",
        "limitations": [
            "IEX TOPS is aggregated top-of-book, not order-level queue data",
            "trade direction is not inferred, so markout proxy is unsigned by aggressor side",
            "sample symbols are IEX test symbols from an official sample PCAP",
        ],
    }
    return summary, symbol_rows, sample_rows, spread_rows, markout_rows, deltas


def build_test_coverage() -> list[dict[str, Any]]:
    rows = []
    components = [
        ("foundation", "quality"),
        ("decoder", "decoder-validation"),
        ("replay", "replay-validation"),
        ("features", "features-validation"),
        ("execution", "execution-sensitivity"),
        ("surface", "surface-validation"),
        ("policy", "policy-validation"),
    ]
    for component, artifact_dir in components:
        source = ROOT / "artifacts" / artifact_dir / "test_summary.json"
        data = read_json(source)
        tests = data.get("tests", [])
        passed = sum(1 for t in tests if t.get("result") == "pass")
        rows.append(
            {
                "component": component,
                "source_path": rel(source),
                "status": data.get("status", ""),
                "tests_total": len(tests),
                "tests_passed": passed,
                "tests_failed_or_other": len(tests) - passed,
                "timestamp_utc": data.get("timestamp_utc", ""),
                "coverage_scope": "clean build smoke and component checks",
            }
        )
    return rows


def build_benchmark_smoke() -> list[dict[str, Any]]:
    rows = []
    components = [
        ("foundation", "quality"),
        ("decoder", "decoder-validation"),
        ("replay", "replay-validation"),
        ("features", "features-validation"),
        ("execution", "execution-sensitivity"),
        ("surface", "surface-validation"),
        ("policy", "policy-validation"),
    ]
    for component, artifact_dir in components:
        source = ROOT / "artifacts" / artifact_dir / "benchmark_summary.json"
        data = read_json(source)
        median_key = next((k for k in data if k.startswith("median_")), "")
        min_key = next((k for k in data if k.startswith("min_")), "")
        rows.append(
            {
                "component": component,
                "benchmark_id": data.get("benchmark_id", ""),
                "source_path": rel(source),
                "iterations": data.get("iterations", ""),
                "work_units": data.get("events", data.get("input_events", "")),
                "median_metric": median_key,
                "median_value_ns": data.get(median_key, ""),
                "min_metric": min_key,
                "min_value_ns": data.get(min_key, ""),
                "note": data.get("note", "smoke wiring only; not a performance claim"),
                "claim_policy": "no production latency or throughput claim",
            }
        )
    return rows


def build_public_iex_mean_spread_chart(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    selected: list[tuple[float, dict[str, Any]]] = []
    for row in rows:
        mean = number(row.get("mean_spread"))
        valid = number(row.get("valid_quotes"))
        quotes = number(row.get("quote_updates"))
        if mean is None or valid is None or quotes is None or valid < 25:
            continue
        selected.append((quotes, row))
    selected.sort(key=lambda item: (-item[0], item[1]["symbol"]))
    return [
        {
            "chart_rank": rank,
            "symbol": row["symbol"],
            "quote_updates": row["quote_updates"],
            "valid_quotes": row["valid_quotes"],
            "mean_spread": row["mean_spread"],
            "full_source_table": rel(ART / "public_iex_quote_stats.csv"),
            "chart_filter": "top_14_by_quote_updates_after_min_25_valid_quote_rows",
        }
        for rank, (_quotes, row) in enumerate(selected[:14], 1)
    ]


def build_queue_sensitivity() -> list[dict[str, Any]]:
    sweep = read_csv(ROOT / "artifacts" / "execution-sensitivity" / "scenario_sweep.csv")
    fills = {r["simulation_id"]: r for r in read_jsonl(ROOT / "artifacts" / "execution-sensitivity" / "deterministic_fills.jsonl")}
    rows = []
    for row in sweep:
        order_qty = number(fills.get(row["scenario_id"], {}).get("order_quantity")) or 3.0
        filled = number(row.get("filled_quantity")) or 0.0
        markout = number(row.get("markout_ticks_x2"))
        rows.append(
            {
                "scenario_id": row["scenario_id"],
                "source_path": "artifacts/execution-sensitivity/scenario_sweep.csv",
                "queue_model_id": row["queue_model_id"],
                "latency_model_id": row["latency_model_id"],
                "hidden_ahead_multiplier": row["hidden_ahead_multiplier"],
                "latency_multiplier": row["latency_multiplier"],
                "cancel_rate_multiplier": row["cancel_rate_multiplier"],
                "fill_status": row["fill_status"],
                "filled_quantity": int(filled),
                "order_quantity": int(order_qty),
                "fill_fraction": round(filled / order_qty, 6),
                "markout_ticks_x2": "" if markout is None else markout,
                "statistical_status": "not_enough_data",
                "interpretation": "synthetic scenario sensitivity only",
            }
        )
    return rows


def build_surface_tables() -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    diag = read_json(ROOT / "artifacts" / "surface-validation" / "surface_diagnostics.json")
    static = {r["surface_id"].strip('"'): r for r in read_csv(ROOT / "artifacts" / "surface-validation" / "static_arbitrage.csv")}
    surface_rows = []
    for fit in diag["fits"]:
        arb = static.get(fit["surface_id"], {})
        surface_rows.append(
            {
                "surface_id": fit["surface_id"],
                "source_path": "artifacts/surface-validation/surface_diagnostics.json",
                "surface_model_id": fit["surface_model_id"],
                "status": fit["status"],
                "reason": fit["reason"],
                "accepted_quote_count": fit["accepted_quote_count"],
                "rmse_iv": "" if fit["rmse_iv"] is None else fit["rmse_iv"],
                "monotonic_violations": fit["static_arbitrage"]["monotonic_violations"],
                "butterfly_violations": fit["static_arbitrage"]["butterfly_violations"],
                "calendar_violations": fit["static_arbitrage"]["calendar_violations"],
                "interpolation_status": fit["interpolation"]["status"],
                "interpolation_fair_value": "" if fit["interpolation"]["fair_value"] is None else fit["interpolation"]["fair_value"],
                "static_source_path": "artifacts/surface-validation/static_arbitrage.csv",
                "static_reason": arb.get("reason", ""),
            }
        )
    filter_rows = [
        {
            "status": status,
            "count": count,
            "source_path": "artifacts/surface-validation/surface_diagnostics.json",
            "interpretation": "quote filter fixture coverage",
        }
        for status, count in sorted(diag["quote_filter_counts"].items())
    ]
    return surface_rows, filter_rows


def build_policy_tables() -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    ablation = read_csv(ROOT / "artifacts" / "policy-validation" / "ablation_table.csv")
    base_bid = number(ablation[0]["bid"]) or 0.0
    base_ask = number(ablation[0]["ask"]) or 0.0
    ablation_rows = []
    for row in ablation:
        bid = number(row["bid"]) or 0.0
        ask = number(row["ask"]) or 0.0
        ablation_rows.append(
            {
                **row,
                "source_path": "artifacts/policy-validation/ablation_table.csv",
                "quote_width": round(ask - bid, 12),
                "bid_shift_from_passive": round(bid - base_bid, 12),
                "ask_shift_from_passive": round(ask - base_ask, 12),
                "claim_policy": "quote mechanics only; no return or fill-rate claim",
            }
        )
    pnl = read_json(ROOT / "artifacts" / "policy-validation" / "pnl_attribution.json")
    component_keys = [
        "quoted_spread_capture",
        "inventory_revaluation",
        "delta_hedge_pnl",
        "vega_explain",
        "gamma_explain",
        "theta_explain",
        "fees",
        "slippage",
        "residual",
        "total_pnl",
    ]
    pnl_rows = [
        {
            "component": key,
            "value": pnl.get(key, ""),
            "source_path": "artifacts/policy-validation/pnl_attribution.json",
            "status": pnl.get(f"{key}_status", pnl.get("status", "")),
            "interpretation": "synthetic accounting identity only",
        }
        for key in component_keys
    ]
    return ablation_rows, pnl_rows


def build_bootstrap_rows(
    queue_rows: list[dict[str, Any]],
    policy_rows: list[dict[str, Any]],
    public_symbol_rows: list[dict[str, Any]],
    public_spread_rows: list[dict[str, Any]],
    public_markout_rows: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    iv_rows = read_csv(ROOT / "artifacts" / "surface-validation" / "iv_residuals.csv")
    greek_rows = read_csv(ROOT / "artifacts" / "surface-validation" / "greek_fd_errors.csv")
    specs = [
        (
            "surface_mean_residual_iv",
            [number(r["residual_iv"]) for r in iv_rows],
            "artifacts/surface-validation/iv_residuals.csv",
            "bootstrap_percentile_mean",
        ),
        (
            "surface_mean_abs_residual_iv",
            [abs(number(r["residual_iv"]) or 0.0) for r in iv_rows if number(r["residual_iv"]) is not None],
            "artifacts/surface-validation/iv_residuals.csv",
            "bootstrap_percentile_mean",
        ),
        (
            "surface_mean_abs_residual_price",
            [abs(number(r["residual_price"]) or 0.0) for r in iv_rows if number(r["residual_price"]) is not None],
            "artifacts/surface-validation/iv_residuals.csv",
            "bootstrap_percentile_mean",
        ),
        (
            "queue_fill_fraction",
            [number(r["fill_fraction"]) for r in queue_rows],
            "artifacts/execution-sensitivity/scenario_sweep.csv",
            "not_enough_data",
        ),
        (
            "queue_markout_ticks_x2",
            [number(r["markout_ticks_x2"]) for r in queue_rows],
            "artifacts/execution-sensitivity/scenario_sweep.csv",
            "not_enough_data",
        ),
        (
            "policy_bid_shift",
            [number(r["bid_shift_from_passive"]) for r in policy_rows],
            "artifacts/policy-validation/ablation_table.csv",
            "not_enough_data",
        ),
        (
            "surface_greek_abs_error",
            [number(r["abs_error"]) for r in greek_rows],
            "artifacts/surface-validation/greek_fd_errors.csv",
            "not_enough_data",
        ),
        (
            "public_iex_mean_quote_spread",
            [number(r["spread"]) for r in public_spread_rows],
            "artifacts/report/public_iex_quote_spreads.csv",
            "bootstrap_percentile_mean",
        ),
        (
            "public_iex_median_quote_spread",
            [number(r["spread"]) for r in public_spread_rows],
            "artifacts/report/public_iex_quote_spreads.csv",
            "bootstrap_percentile_median",
        ),
        (
            "public_iex_next_mid_minus_trade",
            [number(r["next_mid_minus_trade"]) for r in public_markout_rows],
            "artifacts/report/public_iex_markout_proxy.csv",
            "bootstrap_percentile_mean",
        ),
    ]
    rows = []
    for metric_id, raw_values, source, method in specs:
        values = [v for v in raw_values if v is not None]
        if len(values) >= MIN_BOOTSTRAP_N and method != "not_enough_data":
            stat = statistics.median if method.endswith("median") else statistics.fmean
            estimate, low, high = bootstrap_stat(values, stat)
            limitation = "synthetic fixture rows; no p-value"
            if metric_id.startswith("public_iex"):
                limitation = "official IEX TOPS sample; top-of-book only; no p-value"
            if metric_id == "public_iex_mean_quote_spread":
                limitation += "; mean is sensitive to extreme sample/test-symbol spreads"
            rows.append(
                {
                    "metric_id": metric_id,
                    "source_path": source,
                    "n": len(values),
                    "estimate": estimate,
                    "ci_low": low,
                    "ci_high": high,
                    "method": method,
                    "seed": SEED,
                    "status": "computed",
                    "limitation": limitation,
                }
            )
        else:
            rows.append(
                {
                    "metric_id": metric_id,
                    "source_path": source,
                    "n": len(values),
                    "estimate": "" if not values else statistics.fmean(values),
                    "ci_low": "not_enough_data",
                    "ci_high": "not_enough_data",
                    "method": "not_enough_data",
                    "seed": SEED,
                    "status": "not_enough_data",
                    "limitation": f"requires at least {MIN_BOOTSTRAP_N} rows and independent interpretation",
                }
            )
    return rows


def build_negative_results() -> list[dict[str, Any]]:
    return [
        {
            "area": "external_data",
            "source_path": "artifacts/report/free_data_manifest.json",
            "observed_result": "IEX TOPS public sample ingested, but it is aggregated top-of-book",
            "interpretation": "real quote/trade diagnostics are supported; individual queue/fill claims are not",
            "treatment": "real_data_scope_limit",
        },
        {
            "area": "queue_sensitivity",
            "source_path": "artifacts/execution-sensitivity/scenario_sweep.csv",
            "observed_result": "hidden_ahead_x1 and latency_x4 scenarios are UNFILLED_CENSORED",
            "interpretation": "synthetic fills are sensitive to hidden-ahead and latency assumptions",
            "treatment": "reported as sensitivity, not calibration",
        },
        {
            "area": "queue_statistics",
            "source_path": "artifacts/report/bootstrap_intervals.csv",
            "observed_result": "queue and markout samples are below bootstrap threshold",
            "interpretation": "confidence intervals would be fake precision",
            "treatment": "not_enough_data",
        },
        {
            "area": "surface_safety",
            "source_path": "artifacts/surface-validation/static_arbitrage.csv",
            "observed_result": "corrupt call and put smiles fail static-arbitrage checks",
            "interpretation": "safe-fail path works on fixture",
            "treatment": "negative fixture evidence",
        },
        {
            "area": "forward_extraction",
            "source_path": "artifacts/surface-validation/surface_diagnostics.json",
            "observed_result": "put-call parity forward extraction remains TODO",
            "interpretation": "manifest assumptions are not a real forward curve",
            "treatment": "limitation",
        },
        {
            "area": "pnl_attribution",
            "source_path": "artifacts/policy-validation/pnl_attribution.json",
            "observed_result": "delta hedge and Greeks explain fields are zero placeholders",
            "interpretation": "no hedge-performance or Greeks-attribution claim",
            "treatment": "limitation",
        },
        {
            "area": "benchmarks",
            "source_path": rel(FIG / "benchmark_smoke_table.csv"),
            "observed_result": "benchmark artifacts are smoke wiring timings",
            "interpretation": "not production latency or throughput evidence",
            "treatment": "no performance claim",
        },
        {
            "area": "quantstats",
            "source_path": "artifacts/policy-validation/pnl_attribution.json",
            "observed_result": "no chronological returns series exists",
            "interpretation": "QuantStats report would be misleading",
            "treatment": "not_applicable",
        },
    ]


def build_free_data_manifest() -> dict[str, Any]:
    return {
        "package_id": "research_report",
        "access_date": ACCESS_DATE,
        "external_data_used_for_metrics": True,
        "downloaded_external_files": [
            {
                "path": rel(PUBLIC_IEX_GZ),
                "source_url": PUBLIC_IEX_URL,
                "size_bytes": PUBLIC_IEX_GZ.stat().st_size,
                "sha256": sha256_file(PUBLIC_IEX_GZ),
                "transformations": "gzip decompress in-memory, parse pcapng/IEX-TP/TOPS quote and trade messages, emit report CSV/JSON summaries",
            }
        ],
        "audited_sources": [
            {
                "source_id": "iex_hist",
                "name": "IEX Exchange Historical Data (HIST)",
                "source_urls": [
                    "https://www.iex.io/products/equities/market-data-connectivity",
                    "https://www.iex.io/resources/trading/market-data",
                    "https://www.iex.io/legal/hist-data-terms",
                    "https://iextrading.com/trading/alerts/2017/014/",
                ],
                "access_requirements": "Free historical download path; no login/API key/payment observed for HIST. Real-time feeds require agreements and are not used.",
                "terms_summary": "Attribution required when distributing/accessing IEX historical data; IEX retains proprietary rights; data is provided as-is, IEX-only, not investment advice.",
                "queue_relevance": "DEEP+ sample PCAP is order-by-order displayed depth; DEEP/TOPS are aggregated and cannot identify individual queue position.",
                "candidate_status": "used_via_official_2018_tops_sample",
                "data_file_downloaded": True,
                "file_size_bytes": PUBLIC_IEX_GZ.stat().st_size,
                "checksum_sha256": sha256_file(PUBLIC_IEX_GZ),
                "transformations": "gzip decompress in-memory, parse pcapng/IEX-TP/TOPS quote and trade messages, emit summary CSV/JSON",
                "limitations": "TOPS is aggregated top-of-book; non-displayed liquidity and individual queue position are not represented; IEX-only venue scope.",
            },
            {
                "source_id": "nasdaq_totalview_sample",
                "name": "Nasdaq TotalView-ITCH public sample notice",
                "source_urls": [
                    "https://www.nasdaqtrader.com/TraderNews.aspx?id=nva2008-091",
                    "https://data.nasdaq.com/databases/NTV",
                ],
                "access_requirements": "Old public FTP sample notice; current Nasdaq Data Link page requires JavaScript and may require account/subscription for product data.",
                "terms_summary": "Not selected because availability and redistribution path were not clear enough for a no-login reproducible addendum.",
                "queue_relevance": "Order-level ITCH would be relevant if a lawful sample is confirmed.",
                "candidate_status": "defer_until_no_login_download_and_terms_are_verified",
                "data_file_downloaded": False,
                "file_size_bytes": "not_applicable_not_downloaded",
                "checksum_sha256": "not_applicable_not_downloaded",
                "transformations": "none",
                "limitations": "2008 sample notice says files remained through March 31, 2009; current product path may be gated.",
            },
            {
                "source_id": "stooq_ohlc",
                "name": "Stooq historical OHLC CSV",
                "source_urls": ["https://stooq.com/db/h/"],
                "access_requirements": "Public web page, but browser verification/JavaScript was observed during audit.",
                "terms_summary": "Search result states personal use; not selected for repository reproduction.",
                "queue_relevance": "OHLC bars cannot support queue, fill, markout, or order-book policy questions.",
                "candidate_status": "rejected_not_queue_relevant",
                "data_file_downloaded": False,
                "file_size_bytes": "not_applicable_not_downloaded",
                "checksum_sha256": "not_applicable_not_downloaded",
                "transformations": "none",
                "limitations": "No order book; not enough for the study question.",
            },
        ],
    }


def main() -> int:
    ART.mkdir(parents=True, exist_ok=True)
    FIG.mkdir(parents=True, exist_ok=True)

    public_summary, public_symbols, public_sample, public_spreads, public_markouts, _public_deltas = public_iex_tables()
    public_spread_chart = build_public_iex_mean_spread_chart(public_symbols)
    write_json(ART / "public_iex_summary.json", public_summary)
    write_csv(ART / "public_iex_quote_stats.csv", public_symbols)
    write_csv(ART / "public_iex_tops_events_sample.csv", public_sample)
    write_csv(ART / "public_iex_quote_spreads.csv", public_spreads)
    write_csv(ART / "public_iex_markout_proxy.csv", public_markouts)

    lineage = build_data_lineage()
    tests = build_test_coverage()
    benchmarks = build_benchmark_smoke()
    queue = build_queue_sensitivity()
    surface, filters = build_surface_tables()
    policy, pnl = build_policy_tables()
    bootstrap = build_bootstrap_rows(queue, policy, public_symbols, public_spreads, public_markouts)
    negative = build_negative_results()
    free_data = build_free_data_manifest()

    write_csv(FIG / "data_lineage.csv", lineage)
    write_csv(FIG / "test_coverage.csv", tests)
    write_csv(FIG / "benchmark_smoke_table.csv", benchmarks)
    write_csv(FIG / "queue_sensitivity.csv", queue)
    write_csv(FIG / "surface_diagnostics.csv", surface)
    write_csv(FIG / "surface_quote_filter_counts.csv", filters)
    write_csv(FIG / "policy_ablation.csv", policy)
    write_csv(FIG / "pnl_attribution_components.csv", pnl)
    write_csv(FIG / "public_iex_quote_stats.csv", public_symbols)
    write_csv(FIG / "public_iex_mean_spread_chart.csv", public_spread_chart)
    write_csv(FIG / "public_iex_quote_spreads_sample.csv", public_spreads[:5000])
    write_csv(FIG / "public_iex_markout_proxy_sample.csv", public_markouts[:1000])
    write_csv(ART / "bootstrap_intervals.csv", bootstrap)
    write_csv(ART / "negative_results.csv", negative)
    write_json(ART / "free_data_manifest.json", free_data)

    figures = [
        {
            "figure_id": "queue_fill_fraction",
            "source_table": rel(FIG / "queue_sensitivity.csv"),
            **maybe_plot_bar(queue, "scenario_id", "fill_fraction", FIG / "queue_fill_fraction.svg", "Synthetic Queue Sensitivity", "fill fraction"),
        },
        {
            "figure_id": "surface_filter_counts",
            "source_table": rel(FIG / "surface_quote_filter_counts.csv"),
            **maybe_plot_bar(filters, "status", "count", FIG / "surface_quote_filter_counts.svg", "Quote Filter Counts", "rows"),
        },
        {
            "figure_id": "policy_quotes",
            "source_table": rel(FIG / "policy_ablation.csv"),
            **maybe_plot_policy(policy, FIG / "policy_ablation_quotes.svg"),
        },
        {
            "figure_id": "bootstrap_intervals",
            "source_table": rel(ART / "bootstrap_intervals.csv"),
            **maybe_plot_ci(bootstrap, FIG / "bootstrap_intervals.svg"),
        },
        {
            "figure_id": "public_iex_mean_spread",
            "source_table": rel(FIG / "public_iex_mean_spread_chart.csv"),
            **maybe_plot_barh(
                public_spread_chart,
                "symbol",
                "mean_spread",
                FIG / "public_iex_mean_spread.svg",
                "IEX TOPS Mean Spread (Most-Quoted Symbols)",
                "dollars",
                logx=True,
            ),
        },
    ]
    write_json(FIG / "figures_manifest.json", figures)

    claim_evidence = [
        {
            "claim_id": "REPORT-C01",
            "claim": "The report includes statistical intervals only for samples meeting the bootstrap threshold.",
            "evidence_paths": "artifacts/report/bootstrap_intervals.csv;artifacts/surface-validation/iv_residuals.csv",
        },
        {
            "claim_id": "REPORT-C02",
            "claim": "Queue outcomes are sensitive to hidden-ahead, latency, and cancel-rate assumptions in the synthetic fixture.",
            "evidence_paths": f"{rel(FIG / 'queue_sensitivity.csv')};artifacts/execution-sensitivity/scenario_sweep.csv",
        },
        {
            "claim_id": "REPORT-C03",
            "claim": "Surface safety diagnostics reject unsafe synthetic smiles and expose quote-filter statuses.",
            "evidence_paths": f"{rel(FIG / 'surface_diagnostics.csv')};artifacts/surface-validation/static_arbitrage.csv;artifacts/surface-validation/surface_diagnostics.json",
        },
        {
            "claim_id": "REPORT-C04",
            "claim": "Policy ablation rows are quote-mechanics checks only, not market-return evidence.",
            "evidence_paths": f"{rel(FIG / 'policy_ablation.csv')};artifacts/policy-validation/ablation_table.csv",
        },
        {
            "claim_id": "REPORT-C05",
            "claim": "The report uses a no-login public IEX TOPS sample for real quote/trade diagnostics.",
            "evidence_paths": "artifacts/report/free_data_manifest.json;artifacts/report/public_iex_summary.json;artifacts/report/public_iex_quote_stats.csv;REPORT.md",
        },
        {
            "claim_id": "REPORT-C06",
            "claim": "Public TOPS evidence does not support individual visible queue or fill claims.",
            "evidence_paths": "artifacts/report/public_iex_summary.json;artifacts/report/negative_results.csv;REPORT.md",
        },
    ]
    write_csv(FIG / "claim_evidence.csv", claim_evidence)

    release_files = [
        "README.md",
        "REPORT.md",
        "artifacts/report/stats_summary.json",
        "artifacts/report/bootstrap_intervals.csv",
        "artifacts/report/negative_results.csv",
        "artifacts/report/free_data_manifest.json",
        "artifacts/report/public_iex_summary.json",
        "artifacts/report/public_iex_quote_stats.csv",
        "artifacts/report/public_iex_tops_events_sample.csv",
        "artifacts/report/public_iex_quote_spreads.csv",
        "artifacts/report/public_iex_markout_proxy.csv",
        "artifacts/report/repro_commands.md",
        rel(PUBLIC_IEX_GZ),
        rel(FIG / "data_lineage.csv"),
        rel(FIG / "test_coverage.csv"),
        rel(FIG / "benchmark_smoke_table.csv"),
        rel(FIG / "queue_sensitivity.csv"),
        rel(FIG / "surface_diagnostics.csv"),
        rel(FIG / "surface_quote_filter_counts.csv"),
        rel(FIG / "policy_ablation.csv"),
        rel(FIG / "pnl_attribution_components.csv"),
        rel(FIG / "public_iex_quote_stats.csv"),
        rel(FIG / "public_iex_mean_spread_chart.csv"),
        rel(FIG / "public_iex_quote_spreads_sample.csv"),
        rel(FIG / "public_iex_markout_proxy_sample.csv"),
        rel(FIG / "claim_evidence.csv"),
        rel(FIG / "figures_manifest.json"),
    ]
    release_files.extend(f["path"] for f in figures if f["status"] == "rendered")

    summary = {
        "schema_version": "vegaflux.canonical_market.v0.1",
        "package_id": "research_report",
        "generated_from_head": "not_embedded_in_checked_in_artifacts",
        "git_head_capture_policy": "omitted from generated artifacts so clean regeneration remains deterministic after documentation commits",
        "study_question": "How sensitive are passive fills, markouts, and policy outputs to visible queue, latency, hidden-ahead, cancel-rate, and surface safety assumptions?",
        "data_basis": "checked-in synthetic fixtures plus official no-login IEX TOPS public sample",
        "bootstrap_policy": {
            "min_rows": MIN_BOOTSTRAP_N,
            "iterations": BOOTSTRAP_ITERATIONS,
            "seed": SEED,
            "p_values": "not_computed",
        },
        "quantstats_status": "not_used_no_strategy_returns_series; public TOPS supports quote/trade diagnostics only",
        "plotting_status": figures,
        "table_counts": {
            "data_lineage_rows": len(lineage),
            "test_coverage_rows": len(tests),
            "benchmark_rows": len(benchmarks),
            "queue_sensitivity_rows": len(queue),
            "surface_rows": len(surface),
            "policy_ablation_rows": len(policy),
            "bootstrap_rows": len(bootstrap),
            "negative_result_rows": len(negative),
            "public_iex_symbol_rows": len(public_symbols),
            "public_iex_mean_spread_chart_rows": len(public_spread_chart),
            "public_iex_sample_rows": len(public_sample),
            "public_iex_quote_spread_rows": len(public_spreads),
            "public_iex_markout_proxy_rows": len(public_markouts),
        },
        "public_iex_summary": public_summary,
        "bootstrap_results": bootstrap,
        "acceptance": {
            "external_paid_data_required": False,
            "external_data_used_for_metrics": True,
            "profitability_claims": False,
            "production_hft_latency_claims": False,
            "p_values_computed": False,
            "missing_evidence_listed_as_limitation": True,
        },
        "claim_evidence": claim_evidence,
        "release_files": release_files,
    }
    write_json(ART / "stats_summary.json", summary)

    missing_sources = [r["source_path"] for r in lineage if not (ROOT / r["source_path"]).exists()]
    missing_generated = [p for p in release_files if not (ROOT / p).exists() and not p.endswith(".md")]
    if missing_sources or missing_generated:
        raise SystemExit({"missing_sources": missing_sources, "missing_generated": missing_generated})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
