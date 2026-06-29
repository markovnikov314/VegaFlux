# VegaFlux Report

## Short Version

VegaFlux is a reproducible research project for market-data replay, queue
sensitivity, options-surface safety checks, and synthetic policy ablation. It
uses small controlled fixtures for most claims and one free public IEX TOPS
sample for parser and quote/trade diagnostics.

## Main Question

How sensitive are passive fills, markouts, and policy outputs to visible queue,
latency, hidden-ahead, cancel-rate, and surface safety assumptions?

The current evidence says: very sensitive in the controlled fixtures, but not
calibrated to a venue. That boundary is deliberate.

## Evidence Map

| Topic | Source |
| --- | --- |
| Data lineage | `artifacts/report/figures/data_lineage.csv` |
| Test coverage | `artifacts/report/figures/test_coverage.csv` |
| Smoke benchmarks | `artifacts/report/figures/benchmark_smoke_table.csv` |
| Queue sensitivity | `artifacts/report/figures/queue_sensitivity.csv` |
| Surface diagnostics | `artifacts/report/figures/surface_diagnostics.csv` |
| Policy ablation | `artifacts/report/figures/policy_ablation.csv` |
| Bootstrap intervals | `artifacts/report/bootstrap_intervals.csv` |
| Negative results | `artifacts/report/negative_results.csv` |
| Public data manifest | `artifacts/report/free_data_manifest.json` |
| Public IEX summary | `artifacts/report/public_iex_summary.json` |

## Public Data Card

| Field | Value |
| --- | --- |
| Source | Official IEX TOPS 1.6 sample PCAP gzip |
| URL | `https://www.googleapis.com/download/storage/v1/b/iex/o/data%2Ffeeds%2F20180127%2F20180127_IEXTP1_TOPS1.6.pcap.gz?alt=media&generation=1517101257197858` |
| Terms/spec context | `https://www.iex.io/resources/trading/market-data`, `https://www.iex.io/legal/hist-data-terms` |
| Access date | 2026-06-28 |
| Local path | `data_contracts/fixtures/public_iex/20180127_IEXTP1_TOPS1.6.pcap.gz` |
| Compressed size | 2,176,186 bytes |
| Compressed SHA-256 | `ecfcef16491d3d6591b869e0db21164ed0fb9d2a491067f87fde40336f842d3b` |
| Uncompressed SHA-256 | `09fbcf83bb847650bd0c866b4406c07eff4d893bcb4d6c51ff24d53eadd2cf72` |
| Transformations | gzip decompress in memory, parse pcapng/IEX-TP/TOPS, emit CSV/JSON summaries |
| Limitations | Aggregated top-of-book, no individual queue, no hidden liquidity, no trade direction |

No paid, proprietary, login-gated, API-key, exchange-agreement, or credit-card
data is required to reproduce the report.

## Public Sample Counts

| Metric | Count |
| --- | ---: |
| TOPS messages | 99,871 |
| Parsed messages | 63,640 |
| Quote updates | 41,959 |
| Valid quote-spread rows | 12,613 |
| Trade reports | 21,672 |
| Next-mid-minus-trade proxy rows | 21,557 |

## Statistical Rules

Bootstrap intervals use 2,000 percentile resamples with seed `424242` and a
minimum sample size of 10. If a table is too small, the result is reported as
`not_enough_data`. P-values are not computed because the current assumptions do
not justify them.

## Negative Results

| Area | Result |
| --- | --- |
| Public TOPS data | Useful for quote/trade diagnostics, not individual queue position |
| Queue CI | Too few rows for confidence intervals |
| Markout CI | Too few observations for confidence intervals |
| Surface checks | Corrupt synthetic surfaces are rejected |
| P&L attribution | Synthetic accounting only; no profitability inference |
| Benchmarks | Smoke timings only; no latency claim |
| QuantStats | Not used because there is no chronological strategy returns series |

## Reproduction

```powershell
python python/scripts/generate_report.py
python -m json.tool artifacts/report/stats_summary.json
python -m json.tool artifacts/report/free_data_manifest.json
python -m json.tool artifacts/report/public_iex_summary.json
python -m json.tool artifacts/report/figures/figures_manifest.json
python python/scripts/clean_build_test.py
```
