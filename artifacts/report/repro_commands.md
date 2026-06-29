# Report Reproduction Commands

Run from repository root on Windows PowerShell.

```powershell
$env:PYTHONPATH = "python"
python python/scripts/generate_report.py
python python/scripts/clean_build_test.py
python -m json.tool artifacts/report/stats_summary.json
python -m json.tool artifacts/report/free_data_manifest.json
python -m json.tool artifacts/report/figures/figures_manifest.json
```

Optional source-table inspection:

```powershell
Get-Content artifacts/report/figures/queue_sensitivity.csv
Get-Content artifacts/report/bootstrap_intervals.csv
Get-Content artifacts/report/negative_results.csv
Get-Content artifacts/report/public_iex_summary.json
```

No paid, proprietary, login-gated, API-key, exchange-agreement, or credit-card data is required.
