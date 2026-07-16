#!/usr/bin/env python3
"""Write a privacy-safe JSON manifest for a reproduced matrix.

Only explicitly whitelisted hardware fields are queried.  In particular this
script never invokes ``system_profiler``, whose full output contains device
serial numbers and provisioning identifiers.
"""

import argparse
import csv
import hashlib
import importlib.metadata
import json
import os
import platform
import subprocess
from datetime import datetime, timezone
from pathlib import Path


def command(argv, cwd=None):
    try:
        result = subprocess.run(
            argv, cwd=cwd, capture_output=True, text=True, timeout=10, check=False
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def one_line(argv, cwd=None):
    value = command(argv, cwd=cwd)
    return value.splitlines()[0] if value else None


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def integer_sysctl(name):
    value = command(["sysctl", "-n", name])
    try:
        return int(value) if value is not None else None
    except ValueError:
        return None


def platform_manifest():
    info = {
        "system": platform.system(),
        "release": platform.release(),
        "architecture": platform.machine(),
        "logical_cpus": os.cpu_count(),
    }
    if platform.system() == "Darwin":
        info.update({
            "os_product_version": command(["sw_vers", "-productVersion"]),
            "os_build": command(["sw_vers", "-buildVersion"]),
            "hardware_model": command(["sysctl", "-n", "hw.model"]),
            "chip": command(["sysctl", "-n", "machdep.cpu.brand_string"]),
            "physical_cpus": integer_sysctl("hw.physicalcpu"),
            "memory_bytes": integer_sysctl("hw.memsize"),
            "performance_cores": integer_sysctl("hw.perflevel0.physicalcpu"),
            "efficiency_cores": integer_sysctl("hw.perflevel1.physicalcpu"),
        })
    elif platform.system() == "Linux":
        pretty_name = None
        try:
            for line in Path("/etc/os-release").read_text().splitlines():
                if line.startswith("PRETTY_NAME="):
                    pretty_name = line.partition("=")[2].strip().strip('"')
                    break
        except OSError:
            pass
        info["os_product_version"] = pretty_name
        info["memory_bytes"] = None
        try:
            for line in Path("/proc/meminfo").read_text().splitlines():
                if line.startswith("MemTotal:"):
                    info["memory_bytes"] = int(line.split()[1]) * 1024
                    break
        except (OSError, ValueError, IndexError):
            pass
    return {key: value for key, value in info.items() if value is not None}


def dependency_versions():
    versions = {}
    for package in ("pandas", "matplotlib"):
        try:
            versions[package] = importlib.metadata.version(package)
        except importlib.metadata.PackageNotFoundError:
            versions[package] = None
    return versions


def git_revision(path):
    if not (path / ".git").exists():
        return None
    return command(["git", "rev-parse", "HEAD"], cwd=path)


def cmake_cache(path):
    """Return configured cache entries without leaking the cache wholesale."""
    values = {}
    try:
        lines = path.read_text().splitlines()
    except OSError:
        return values
    for line in lines:
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        declaration, value = line.split("=", 1)
        key = declaration.split(":", 1)[0]
        values[key] = value
    return values


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag", required=True)
    parser.add_argument("--trials", required=True, type=int)
    parser.add_argument("--seconds", required=True, type=float)
    parser.add_argument("--profile", default="full")
    parser.add_argument("--queue-order", choices=["fixed", "rotated"], default="fixed")
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    if not args.dataset.is_file():
        parser.error(f"dataset not found: {args.dataset}")
    with args.dataset.open(newline="") as handle:
        row_count = sum(1 for _ in csv.DictReader(handle))

    root = Path(__file__).resolve().parents[1]
    commit = command(["git", "rev-parse", "HEAD"], cwd=root)
    # Generated datasets/figures are intentionally untracked at this point;
    # record whether tracked source files differ from the revision instead.
    dirty_output = command(
        ["git", "status", "--porcelain", "--untracked-files=no"], cwd=root
    )
    cache = cmake_cache(args.build_dir / "CMakeCache.txt")
    compiler = cache.get("CMAKE_CXX_COMPILER")
    cmake_command = cache.get("CMAKE_COMMAND")
    if not compiler or not cmake_command:
        parser.error(
            f"configured CMake build not found in {args.build_dir}; "
            "pass the build directory used for the benchmark"
        )
    compiler_version = one_line([compiler, "--version"])
    cmake_version = one_line([cmake_command, "--version"])
    if not compiler_version or not cmake_version:
        parser.error(f"cannot query the configured toolchain in {args.build_dir}")

    third_party = {}
    for name in ("rigtorp_spsc", "moodycamel_rwq", "moodycamel_cq"):
        revision = git_revision(args.build_dir / "_deps" / f"{name}-src")
        if revision:
            third_party[name] = revision

    assets = {}
    asset_dir = root / "paper/assets"
    for path in sorted(asset_dir.glob(f"fig_*_{args.tag}.*")):
        if path.suffix in {".pdf", ".png"}:
            assets[path.name] = sha256(path)

    summary_path = args.dataset.parent / f"summary_{args.tag}.md"
    summary = None
    if summary_path.is_file():
        summary = {
            "path": summary_path.as_posix(),
            "sha256": sha256(summary_path),
        }

    manifest = {
        "schema_version": 1,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "study": {
            "tag": args.tag,
            "profile": args.profile,
            "trial_processes_per_configuration": args.trials,
            "warmup_trial": 0,
            "measured_trials_requested": args.trials - 1,
            "seconds_per_process": args.seconds,
            "queue_order": args.queue_order,
        },
        "repository": {"commit": commit, "tracked_sources_dirty": bool(dirty_output)},
        "platform": platform_manifest(),
        "toolchain": {
            "compiler": compiler_version,
            "cmake": cmake_version,
            "python": platform.python_version(),
            "python_packages": dependency_versions(),
        },
        "third_party_commits": third_party,
        "outputs": {
            "dataset": {
                "path": args.dataset.as_posix(),
                "rows": row_count,
                "sha256": sha256(args.dataset),
            },
            "summary": summary,
            "figures": assets,
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(f"manifest -> {args.output}")


if __name__ == "__main__":
    main()
