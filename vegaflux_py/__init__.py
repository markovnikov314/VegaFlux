from pathlib import Path

SCHEMA_VERSION = "vegaflux.canonical_market.v0.1"
DEFAULT_SEED = 424242

_tooling_package = Path(__file__).resolve().parents[1] / "python" / "vegaflux_py"
if _tooling_package.is_dir():
    __path__.append(str(_tooling_package))
