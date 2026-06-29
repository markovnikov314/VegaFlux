from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def run(command: list[str]) -> None:
    print("+ " + " ".join(command))
    subprocess.run(command, cwd=ROOT, check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--preset", default="dev")
    parser.add_argument("--config", default="Debug")
    parser.add_argument("--build-dir", type=Path, default=Path("build/dev"))
    args = parser.parse_args()

    build_dir = (ROOT / args.build_dir).resolve() if not args.build_dir.is_absolute() else args.build_dir
    shutil.rmtree(build_dir, ignore_errors=True)
    run(["cmake", "--preset", args.preset])
    run(["ctest", "--test-dir", str(build_dir), "-C", args.config, "--output-on-failure"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
