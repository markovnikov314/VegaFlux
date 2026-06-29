from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import socket
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

from vegaflux_py import SCHEMA_VERSION


DEPENDENCY_LOCK_CANDIDATES = [
    "pyproject.toml",
    "CMakePresets.json",
    "CMakeLists.txt",
]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_many(paths: list[Path]) -> str:
    digest = hashlib.sha256()
    for path in paths:
        digest.update(path.as_posix().encode("utf-8"))
        digest.update(path.read_bytes())
    return digest.hexdigest()


def existing_paths(root: Path, relative_paths: list[str]) -> list[Path]:
    return [root / relative for relative in relative_paths if (root / relative).exists()]


def run_text(args: list[str], cwd: Path | None = None, timeout: int = 5) -> str:
    try:
        completed = subprocess.run(args, cwd=cwd, text=True, capture_output=True, timeout=timeout, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return "unavailable"
    text = (completed.stdout or completed.stderr).strip()
    return text.splitlines()[0] if text else "unavailable"


def git_sha(root: Path) -> str:
    return run_text(["git", "rev-parse", "HEAD"], cwd=root)


def cmake_cache(build_dir: Path) -> dict[str, str]:
    cache_path = build_dir / "CMakeCache.txt"
    if not cache_path.exists():
        return {}
    values: dict[str, str] = {}
    for line in cache_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" not in line or line.startswith("//") or line.startswith("#"):
            continue
        key_type, value = line.split("=", 1)
        key = key_type.split(":", 1)[0]
        values[key] = value
    return values


def compiler_metadata(build_dir: Path) -> tuple[str, list[str]]:
    cache = cmake_cache(build_dir)
    compiler = cache.get("CMAKE_CXX_COMPILER", os.environ.get("CXX", "unavailable"))
    if compiler == "unavailable" and cache.get("CMAKE_AR", "").endswith("lib.exe"):
        candidate = str(Path(cache["CMAKE_AR"]).with_name("cl.exe"))
        if Path(candidate).exists():
            compiler = candidate
    flags = [
        cache.get("CMAKE_CXX_FLAGS", ""),
        cache.get("CMAKE_CXX_FLAGS_DEBUG", ""),
        cache.get("CMAKE_CXX_FLAGS_RELEASE", ""),
    ]
    if compiler == "unavailable":
        generator = cache.get("CMAKE_GENERATOR", "unavailable")
        instance = cache.get("CMAKE_GENERATOR_INSTANCE", "")
        return f"{generator} {instance}".strip(), [flag for flag in flags if flag]
    version_arg = ["/Bv"] if compiler.lower().endswith("cl.exe") else ["--version"]
    version = run_text([compiler, *version_arg])
    return f"{compiler} ({version})", [flag for flag in flags if flag]


def host_metadata() -> dict:
    gpu = run_text(["nvidia-smi", "--query-gpu=name,driver_version", "--format=csv,noheader"], timeout=2)
    return {
        "cpu": platform.processor() or platform.machine(),
        "gpu": gpu,
        "hostname": platform.node() or socket.gethostname(),
        "machine": platform.machine(),
        "logical_cpus": os.cpu_count(),
        "platform": platform.platform(),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--component", required=True)
    parser.add_argument("--build-dir", type=Path, default=Path("build/dev"))
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seed", type=int, required=True)
    args = parser.parse_args(argv)

    root = args.root.resolve()
    build_dir = (root / args.build_dir).resolve() if not args.build_dir.is_absolute() else args.build_dir
    dataset = (root / args.dataset).resolve() if not args.dataset.is_absolute() else args.dataset
    config = (root / args.config).resolve() if not args.config.is_absolute() else args.config
    output = (root / args.output).resolve() if not args.output.is_absolute() else args.output
    compiler, flags = compiler_metadata(build_dir)
    metadata = host_metadata()
    dependency_lock_files = existing_paths(root, DEPENDENCY_LOCK_CANDIDATES)
    dependency_lock_hash = sha256_many(dependency_lock_files)
    manifest = {
        "schema_version": SCHEMA_VERSION,
        "component_id": args.component,
        "git_sha": git_sha(root),
        "dependency_lock_hash": dependency_lock_hash,
        "dependency_lock_files": [path.relative_to(root).as_posix() for path in dependency_lock_files],
        "host": {
            "hostname": metadata["hostname"],
            "machine": metadata["machine"],
            "logical_cpus": metadata["logical_cpus"],
        },
        "environment": {
            "ci": os.environ.get("CI", "false"),
            "python_executable": sys.executable,
            "pythonpath_set": bool(os.environ.get("PYTHONPATH")),
            "build_dir": str(build_dir),
        },
        "python_version": platform.python_version(),
        "cmake_version": run_text(["cmake", "--version"]),
        "compiler": compiler,
        "compiler_flags": flags,
        "os": metadata["platform"],
        "cpu": metadata["cpu"],
        "gpu": metadata["gpu"],
        "cpu_gpu_metadata": metadata,
        "dataset_checksum": sha256_file(dataset),
        "config_hash": sha256_file(config),
        "tooling_hash": dependency_lock_hash,
        "seed": args.seed,
        "timestamp_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"output": str(output), "status": "pass"}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
