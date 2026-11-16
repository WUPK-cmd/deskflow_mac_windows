#!/usr/bin/env python3
"""Public build identity and native package verification (standard library only)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import struct
import subprocess
import sys
import tempfile


def run(*args, **kwargs):
    result = subprocess.run(args, check=True, text=True, capture_output=True, timeout=90, **kwargs)
    print(result.stdout, end="")
    print(result.stderr, end="")
    return result.stdout.strip()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def smoke_environment(root, mac):
    env = os.environ.copy()
    for key in list(env):
        normalized = key.upper()
        if normalized.startswith(("QT_", "QML", "DYLD_")) or normalized in {"QTDIR", "QT6_DIR", "CMAKE_PREFIX_PATH"}:
            env.pop(key, None)
    # Deployment tools ship native platform plugins, not the SDK's offscreen
    # test plugin. Use only backends available in the distributed application.
    env["QT_QPA_PLATFORM"] = "cocoa" if mac else "windows"
    if mac:
        env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
    else:
        # os.environ is case insensitive on Windows, but its copied dict is not.
        system_root = next(value for key, value in env.items() if key.upper() == "SYSTEMROOT")
        env["PATH"] = f"{root};{system_root}/System32;{system_root}"
    return env


def verify_macho_dependencies(output, binary):
    # otool includes the inspected file's absolute path (and architecture) as
    # section headers. Only indented load-command entries are dependencies.
    for line in output.splitlines():
        if not line.startswith("\t"):
            continue
        dependency = line.strip().split(" (compatibility version", 1)[0]
        if dependency.startswith(("/opt/homebrew/", "/usr/local/", "/Users/")):
            raise ValueError(f"Developer dependency in {binary}: {dependency}")


def record(args):
    if run("git", "status", "--porcelain", "--untracked-files=no"):
        raise ValueError("Tracked source changed during build")
    identity = {
        "schema": 1,
        "repository": os.environ["GITHUB_REPOSITORY"],
        "commit": run("git", "rev-parse", "HEAD"),
        "describe": run("git", "describe", "--tags", "--always"),
        "target": args.target,
        "qt": os.environ["QT_VERSION"],
        "openssl": args.openssl.strip(),
        "cmake": run("cmake", "--version").splitlines()[0],
        "runner": os.environ["RUNNER_OS"],
        "runnerImage": os.environ.get("ImageVersion", "unknown"),
        "architecture": platform.machine(),
        "microphoneFollow": True,
        "runId": os.environ["GITHUB_RUN_ID"],
        "runAttempt": os.environ["GITHUB_RUN_ATTEMPT"],
        "workflow": ".github/workflows/self-built-ci.yml",
        "run": f"{os.environ['GITHUB_SERVER_URL']}/{os.environ['GITHUB_REPOSITORY']}/actions/runs/{os.environ['GITHUB_RUN_ID']}",
        "signing": "unsigned" if args.target == "windows-x64" else "ad-hoc; not notarized",
    }
    if not identity["openssl"] or identity["commit"] != os.environ["GITHUB_SHA"]:
        raise ValueError("Missing OpenSSL identity or checkout does not match build SHA")
    write_json(args.output, identity)


def verify_tree(args):
    root = args.root.resolve()
    mac = args.target == "macos-arm64"
    if mac:
        matches = [root] if root.name == "Deskflow.app" else list(root.rglob("Deskflow.app"))
        if len(matches) != 1:
            raise ValueError(f"Expected one app in {root}, found {matches}")
        root = matches[0]
        identity = root / "Contents/Resources/build-info.json"
        binaries = [root / "Contents/MacOS/Deskflow", root / "Contents/MacOS/deskflow-core"]
        multimedia = root / "Contents/Frameworks/QtMultimedia.framework"
        plugins = list(root.glob("Contents/PlugIns/multimedia/*.dylib"))
        # Qt uses PlugIns on macOS; retain an explicit check rather than relying on developer Qt.
    else:
        matches = list(root.rglob("deskflow.exe"))
        if len(matches) != 1:
            raise ValueError(f"Expected one GUI in {root}, found {matches}")
        root = matches[0].parent
        identity = root / "build-info.json"
        binaries = [root / "deskflow.exe", root / "deskflow-core.exe"]
        multimedia = root / "Qt6Multimedia.dll"
        plugins = list(root.glob("plugins/multimedia/*.dll"))
        if args.portable:
            for name in ("settings/Deskflow.conf", "vcruntime140.dll", "msvcp140.dll"):
                if not (root / name).is_file():
                    raise ValueError(f"Portable runtime file is missing: {name}")
            if (root / "deskflow-daemon.exe").exists():
                raise ValueError("Portable archive must not contain the Windows service")
    expected = json.loads(args.identity.read_text(encoding="utf-8"))
    actual = json.loads(identity.read_text(encoding="utf-8"))
    if actual != expected or actual["microphoneFollow"] is not True or actual["target"] != args.target:
        raise ValueError("Embedded build identity mismatch")
    if not multimedia.exists() or not plugins:
        raise ValueError(f"Qt Multimedia runtime/backend plugins missing under {root}")
    if mac:
        multimedia_objects = [path for path in multimedia.rglob("QtMultimedia") if path.is_file() and not path.is_symlink()]
        if not multimedia_objects:
            raise ValueError("Qt Multimedia framework binary is missing")
        for dependency in [*multimedia_objects, *plugins]:
            links = run("/usr/bin/otool", "-L", str(dependency))
            verify_macho_dependencies(links, dependency)
    for path in root.rglob("*"):
        name = path.name.lower()
        if path.suffix.lower() in {".pem", ".key", ".log", ".sys", ".kext", ".dext"}:
            raise ValueError(f"Private/runtime/driver file in package: {path}")
        if name == "deskflow.conf" and path.read_text(encoding="utf-8").strip():
            raise ValueError("Package contains nonempty private settings")
        if "vbcable" in name or "blackhole" in name:
            raise ValueError(f"Third-party audio driver payload found: {path}")
    env = smoke_environment(root, mac)
    with tempfile.TemporaryDirectory(prefix="deskflow-package-smoke-") as settings:
        env.update({"HOME": settings, "XDG_CONFIG_HOME": settings, "APPDATA": settings, "LOCALAPPDATA": settings})
        if not mac:
            # Loading each supplied backend in a clean child verifies its DLL
            # dependencies too. This does not open a microphone or record audio.
            run(sys.executable, "-c",
                "import ctypes, os, sys; "
                "directory = os.add_dll_directory(sys.argv[1]); "
                "handles = [ctypes.WinDLL(path) for path in sys.argv[2:]]; "
                "print('VERIFIED bundled Multimedia backend DLL dependencies')",
                str(root), *(str(plugin) for plugin in plugins), env=env, cwd=settings)
        for binary in binaries:
            if mac:
                if run("/usr/bin/lipo", "-archs", str(binary)) != "arm64":
                    raise ValueError(f"Unexpected binary architecture: {binary}")
                dependencies = run("/usr/bin/otool", "-L", str(binary))
                verify_macho_dependencies(dependencies, binary)
            else:
                with binary.open("rb") as stream:
                    stream.seek(0x3C)
                    stream.seek(struct.unpack("<I", stream.read(4))[0])
                    if stream.read(6) != b"PE\0\0\x64\x86":
                        raise ValueError(f"Expected Windows x64 PE: {binary}")
            output = run(str(binary), "--version", env=env, cwd=settings)
            if not output or "deskflow" not in output.lower():
                raise ValueError(f"Missing product version output: {binary}")
    if mac:
        run("/usr/bin/codesign", "--verify", "--deep", "--strict", str(root))
    print(f"VERIFIED {args.target}: {root}; Multimedia, backend, identity, architecture, isolated GUI/Core --version")


def verify_config(args):
    cache = args.cache.read_text(encoding="utf-8")
    for option in ("BUILD_MICROPHONE_FOLLOW", "BUILD_INSTALLER", "BUILD_TESTS"):
        if not re.search(rf"^{option}:BOOL=ON$", cache, re.MULTILINE):
            raise ValueError(f"Required native package build option is not ON: {option}")
    if not re.search(r"^Qt6Multimedia_DIR:PATH=(?!.*NOTFOUND).+$", cache, re.MULTILINE):
        raise ValueError("Qt Multimedia was not configured")
    print("VERIFIED native package configuration: microphone follow, Multimedia, installer and tests enabled")


def manifest(args):
    files = sorted(p for p in args.root.iterdir() if p.is_file() and p.name != "SHA256SUMS.txt")
    if not files:
        raise ValueError("No package assets")
    lines = []
    for path in files:
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        lines.append(f"{digest.hexdigest()}  {path.name}\n")
    (args.root / "SHA256SUMS.txt").write_text("".join(lines), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    cmd = sub.add_parser("record")
    cmd.add_argument("--target", required=True, choices=["windows-x64", "macos-arm64"])
    cmd.add_argument("--openssl", required=True)
    cmd.add_argument("--output", type=Path, required=True)
    cmd.set_defaults(func=record)
    cmd = sub.add_parser("verify-tree")
    cmd.add_argument("--target", required=True, choices=["windows-x64", "macos-arm64"])
    cmd.add_argument("--identity", type=Path, required=True)
    cmd.add_argument("--root", type=Path, required=True)
    cmd.add_argument("--portable", action="store_true")
    cmd.set_defaults(func=verify_tree)
    cmd = sub.add_parser("manifest")
    cmd.add_argument("--root", type=Path, required=True)
    cmd.set_defaults(func=manifest)
    cmd = sub.add_parser("verify-config")
    cmd.add_argument("--cache", type=Path, required=True)
    cmd.set_defaults(func=verify_config)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
