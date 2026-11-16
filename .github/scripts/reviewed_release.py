#!/usr/bin/env python3
"""Verify one completed fork CI run and prepare its two native package sets.

Uses gh's authenticated API; never executes code from downloaded artifacts.
The release workflow invokes this from the trusted default branch.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import zipfile

REPOSITORY = "WUPK-cmd/deskflow_mac_windows"
WORKFLOW = ".github/workflows/self-built-ci.yml"
TARGETS = {"windows-x64": {".zip", ".msi"}, "macos-arm64": {".zip", ".dmg"}}


def gh(*args):
    return subprocess.check_output(["gh", *args], text=True).strip()


def api(path):
    return json.loads(gh("api", f"repos/{REPOSITORY}/{path}"))


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def verify_identity(info, sha, run_id, attempt, target):
    expected = {"schema": 1, "repository": REPOSITORY, "commit": sha, "target": target,
                "microphoneFollow": True, "runId": str(run_id), "runAttempt": str(attempt),
                "workflow": WORKFLOW, "qt": "6.8.3",
                "run": f"https://github.com/{REPOSITORY}/actions/runs/{run_id}"}
    for key, value in expected.items():
        if info.get(key) != value or type(info.get(key)) is not type(value):
            raise ValueError(f"{target}: identity mismatch for {key}")
    if not info.get("openssl"):
        raise ValueError(f"{target}: missing dependency identity")


def verify_run(run, workflow_id, sha):
    if (run.get("repository", {}).get("full_name") != REPOSITORY
            or run.get("head_repository", {}).get("full_name") != REPOSITORY
            or run.get("workflow_id") != workflow_id or run.get("path") != WORKFLOW
            or run.get("head_sha") != sha or run.get("status") != "completed"
            or run.get("conclusion") != "success" or run.get("event") not in {"push", "workflow_dispatch"}):
        raise ValueError("Run must be a successful native package workflow for this exact fork commit")


def verify_assets(root, sha, run_id, attempt, target):
    if any(p.is_symlink() or not p.is_file() for p in root.iterdir()):
        raise ValueError(f"{target}: unexpected directory/symlink in artifact")
    info = json.loads((root / "build-info.json").read_text(encoding="utf-8"))
    verify_identity(info, sha, run_id, attempt, target)
    manifest = {}
    for line in (root / "SHA256SUMS.txt").read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  ([A-Za-z0-9_.-]+)", line)
        if not match or match[2] in manifest:
            raise ValueError(f"{target}: invalid or duplicate checksum entry")
        manifest[match[2]] = match[1]
    files = {p.name for p in root.iterdir()} - {"SHA256SUMS.txt"}
    if files != set(manifest):
        raise ValueError(f"{target}: manifest must cover exactly all assets")
    packages = [root / name for name in files if name != "build-info.json"]
    if len(packages) != 2 or {p.suffix for p in packages} != TARGETS[target]:
        raise ValueError(f"{target}: expected ZIP and native installer")
    for path in packages:
        if not path.name.startswith("deskflow-") or sha not in path.name:
            raise ValueError(f"{target}: package name does not contain exact source SHA")
        platform_name = "win-x64" if target == "windows-x64" else "macos-arm64"
        if platform_name not in path.name:
            raise ValueError(f"{target}: unexpected package target name")
    for name, checksum in manifest.items():
        if digest(root / name) != checksum:
            raise ValueError(f"{target}: checksum mismatch: {name}")
    archive = next(p for p in packages if p.suffix == ".zip")
    with zipfile.ZipFile(archive) as zip_file:
        paths = zip_file.namelist()
        if len(paths) != len(set(paths)):
            raise ValueError(f"{target}: duplicate ZIP entries")
        for name in paths:
            if "\\" in name or name.startswith("/") or ".." in name.split("/"):
                raise ValueError(f"{target}: unsafe ZIP member")
        metadata = [name for name in paths if name.endswith("/build-info.json") or name == "build-info.json"]
        if len(metadata) != 1 or json.loads(zip_file.read(metadata[0])) != info:
            raise ValueError(f"{target}: archive identity differs from manifest identity")
        if target == "macos-arm64" and metadata[0] != "Deskflow.app/Contents/Resources/build-info.json":
            raise ValueError("macOS identity must be inside the app")
    return packages, info


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if os.environ.get("GITHUB_REPOSITORY") != REPOSITORY or os.environ.get("GITHUB_REF") != "refs/heads/main":
        raise ValueError("Release verification must run in the fork's main workflow")
    if not re.fullmatch(r"[1-9][0-9]*", args.run_id) or not re.fullmatch(r"[0-9a-f]{40}", args.commit):
        raise ValueError("Use a specific numeric run ID and full lowercase source commit SHA")
    workflow = api("actions/workflows/self-built-ci.yml")
    run = api(f"actions/runs/{args.run_id}")
    verify_run(run, workflow["id"], args.commit)
    attempt = run["run_attempt"]
    jobs = api(f"actions/runs/{args.run_id}/attempts/{attempt}/jobs?per_page=100")
    for target in TARGETS:
        matches = [job for job in jobs["jobs"] if job["name"] == target]
        if len(matches) != 1 or matches[0]["conclusion"] != "success":
            raise ValueError(f"Missing successful native build for {target}")
    artifacts = api(f"actions/runs/{args.run_id}/artifacts?per_page=100")
    args.output.mkdir(parents=True, exist_ok=False)
    for target in TARGETS:
        name = f"self-built-{target}-{args.commit}"
        matches = [a for a in artifacts["artifacts"] if a["name"] == name and not a["expired"]]
        if len(matches) != 1 or matches[0].get("workflow_run", {}).get("head_sha") != args.commit:
            raise ValueError(f"Expected one unexpired artifact from the selected source: {name}")
        destination = args.output.parent / f"download-{target}"
        gh("run", "download", args.run_id, "--repo", REPOSITORY, "--name", name, "--dir", str(destination))
        packages, _ = verify_assets(destination, args.commit, args.run_id, attempt, target)
        for package in packages:
            shutil.copy2(package, args.output / package.name)
        shutil.copy2(destination / "build-info.json", args.output / f"build-info-{target}.json")
    # Produce one non-colliding manifest covering all four native assets and identities.
    (args.output / "SHA256SUMS.txt").write_text("".join(
        f"{digest(path)}  {path.name}\n" for path in sorted(args.output.iterdir())
    ), encoding="utf-8")
    (args.output.parent / "release-notes.md").write_text(
        f"Microphone-enabled native packages from `{args.commit}`.\n\n"
        f"Reviewed build: https://github.com/{REPOSITORY}/actions/runs/{args.run_id} (attempt {attempt}).\n\n"
        "Windows x64: portable ZIP and MSI. macOS arm64 (macOS 14+): app ZIP and DMG. "
        "Qt Multimedia is bundled; microphone follow is compiled in and remains opt-in.\n\n"
        "Windows packages are unsigned. The Mac app is ad-hoc signed, not Developer ID signed or notarized. "
        "The MSI requires the Microsoft Visual C++ runtime; it does not silently install dependencies.\n\n"
        "Audio drivers are not bundled or installed: obtain VB-CABLE/BlackHole separately from their official providers. "
        "CI checked automated tests, package contents and extracted GUI/Core startup with developer dependency paths cleared. "
        "Physical/virtual audio routing, target applications and microphone permissions still require validation on the target computers.\n",
        encoding="utf-8")
    print(f"VERIFIED release source {args.commit}, run {args.run_id}/{attempt}, both native targets")


if __name__ == "__main__":
    main()
