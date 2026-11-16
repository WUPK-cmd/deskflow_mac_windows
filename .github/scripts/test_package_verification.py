"""Regression checks for artifact identity and release validation boundaries."""
import argparse
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from package_identity import manifest, smoke_environment, verify_config, verify_macho_dependencies
from reviewed_release import REPOSITORY, WORKFLOW, verify_assets, verify_identity, verify_run


class ReleaseVerification(unittest.TestCase):
    sha = "a" * 40

    def test_mac_dependency_headers_may_name_the_runner_checkout(self):
        output = (
            "/Users/runner/staging/QtMultimedia (architecture arm64):\n"
            "\t@rpath/QtCore.framework/Versions/A/QtCore (compatibility version 6.0.0, current version 6.8.3)\n"
            "\t/System/Library/Frameworks/CoreAudio.framework/CoreAudio (compatibility version 1.0.0, current version 1.0.0)\n"
        )
        verify_macho_dependencies(output, "QtMultimedia")

    def test_mac_actual_developer_load_commands_are_rejected(self):
        for prefix in ("/opt/homebrew/", "/usr/local/", "/Users/developer/"):
            with self.subTest(prefix=prefix), self.assertRaises(ValueError):
                verify_macho_dependencies(
                    f"/Users/runner/App:\n\t{prefix}libssl.3.dylib (compatibility version 3.0.0, current version 3.0.0)\n",
                    "App",
                )

    def test_windows_package_smoke_uses_copied_uppercase_environment(self):
        with patch.dict(os.environ, {
            "SYSTEMROOT": "C:/Windows", "PATH": "developer-qt/bin", "Qt6_DIR": "developer-qt",
            "QT_PLUGIN_PATH": "developer-qt/plugins", "DYLD_FRAMEWORK_PATH": "developer-qt/lib",
        }, clear=True):
            env = smoke_environment(Path("staged-package"), False)
        self.assertEqual(env["PATH"], "staged-package;C:/Windows/System32;C:/Windows")
        self.assertEqual(env["QT_QPA_PLATFORM"], "windows")
        self.assertFalse(any("developer-qt" in value for value in env.values()))

    def test_mac_package_smoke_uses_deployed_cocoa_not_sdk_offscreen(self):
        with patch.dict(os.environ, {"PATH": "developer-qt/bin", "QT_QPA_PLATFORM": "offscreen"}, clear=True):
            env = smoke_environment(Path("Deskflow.app"), True)
        self.assertEqual(env["QT_QPA_PLATFORM"], "cocoa")
        self.assertEqual(env["PATH"], "/usr/bin:/bin:/usr/sbin:/sbin")

    def fixture(self, target="windows-x64"):
        info = {"schema": 1, "repository": "WUPK-cmd/deskflow_mac_windows", "commit": self.sha,
                "target": target, "microphoneFollow": True, "runId": "123", "runAttempt": "2",
                "workflow": ".github/workflows/self-built-ci.yml", "qt": "6.8.3", "openssl": "OpenSSL 3",
                "run": "https://github.com/WUPK-cmd/deskflow_mac_windows/actions/runs/123"}
        root = Path(self.directory.name)
        (root / "build-info.json").write_text(json.dumps(info), encoding="utf-8")
        platform = "win-x64" if target == "windows-x64" else "macos-arm64"
        stem = f"deskflow-{self.sha}-{platform}"
        archive = root / f"{stem}.zip"
        self.member = "deskflow/build-info.json" if target == "windows-x64" else "Deskflow.app/Contents/Resources/build-info.json"
        with zipfile.ZipFile(archive, "w") as package:
            package.writestr(self.member, json.dumps(info))
        installer = root / (stem + (".msi" if target == "windows-x64" else ".dmg"))
        installer.write_bytes(b"native-package-fixture")
        manifest(argparse.Namespace(root=root))
        return root, archive, installer, info

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="deskflow-release-test-")
        self.addCleanup(self.directory.cleanup)

    def check_assets(self, root, target="windows-x64"):
        return verify_assets(root, self.sha, "123", 2, target)

    def test_accept_complete_windows_assets(self):
        root, _, _, _ = self.fixture()
        self.assertEqual(len(self.check_assets(root)[0]), 2)

    def test_require_actual_microphone_build_configuration(self):
        cache = Path(self.directory.name) / "CMakeCache.txt"
        valid = ("BUILD_MICROPHONE_FOLLOW:BOOL=ON\nBUILD_INSTALLER:BOOL=ON\n"
                 "BUILD_TESTS:BOOL=ON\nQt6Multimedia_DIR:PATH=/qt/lib/cmake/Qt6Multimedia\n")
        cache.write_text(valid)
        verify_config(argparse.Namespace(cache=cache))
        cache.write_text(valid.replace("BUILD_MICROPHONE_FOLLOW:BOOL=ON", "BUILD_MICROPHONE_FOLLOW:BOOL=OFF"))
        with self.assertRaisesRegex(ValueError, "BUILD_MICROPHONE_FOLLOW"):
            verify_config(argparse.Namespace(cache=cache))
        cache.write_text(valid.replace("/qt/lib/cmake/Qt6Multimedia", "Qt6Multimedia_DIR-NOTFOUND"))
        with self.assertRaisesRegex(ValueError, "Qt Multimedia was not configured"):
            verify_config(argparse.Namespace(cache=cache))

    def test_accept_app_embedded_mac_identity(self):
        root, _, _, _ = self.fixture("macos-arm64")
        self.assertEqual(len(self.check_assets(root, "macos-arm64")[0]), 2)

    def test_reject_modified_native_installer(self):
        root, _, installer, _ = self.fixture()
        installer.write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            self.check_assets(root)

    def test_reject_sidecar_only_identity(self):
        root, archive, _, _ = self.fixture()
        with zipfile.ZipFile(archive, "w") as package:
            package.writestr("deskflow.exe", "fixture")
        manifest(argparse.Namespace(root=root))
        with self.assertRaisesRegex(ValueError, "archive identity"):
            self.check_assets(root)

    def test_reject_different_archive_commit(self):
        root, archive, _, info = self.fixture()
        info["commit"] = "b" * 40
        with zipfile.ZipFile(archive, "w") as package:
            package.writestr(self.member, json.dumps(info))
        manifest(argparse.Namespace(root=root))
        with self.assertRaisesRegex(ValueError, "archive identity"):
            self.check_assets(root)

    def test_reject_missing_native_installer(self):
        root, _, installer, _ = self.fixture()
        installer.unlink()
        manifest(argparse.Namespace(root=root))
        with self.assertRaisesRegex(ValueError, "ZIP and native installer"):
            self.check_assets(root)

    def test_reject_unhashed_extra_asset(self):
        root, _, _, _ = self.fixture()
        (root / "private.key").write_text("fixture")
        with self.assertRaisesRegex(ValueError, "exactly all assets"):
            self.check_assets(root)

    def test_reject_zip_traversal(self):
        root, archive, _, _ = self.fixture()
        with zipfile.ZipFile(archive, "a") as package:
            package.writestr("../unsafe", "fixture")
        manifest(argparse.Namespace(root=root))
        with self.assertRaisesRegex(ValueError, "unsafe ZIP"):
            self.check_assets(root)

    def test_reject_wrong_run_attempt_repository_target_or_flag(self):
        _, _, _, info = self.fixture()
        for key, value in [("runAttempt", "1"), ("repository", "deskflow/deskflow"),
                           ("target", "macos-arm64"), ("microphoneFollow", False),
                           ("microphoneFollow", 1), ("commit", "b" * 40)]:
            with self.subTest(key=key, value=value):
                with self.assertRaisesRegex(ValueError, "identity mismatch"):
                    verify_identity(dict(info, **{key: value}), self.sha, "123", 2, "windows-x64")

    def test_accept_exact_successful_native_run(self):
        run = {"repository": {"full_name": REPOSITORY}, "head_repository": {"full_name": REPOSITORY},
               "workflow_id": 42, "path": WORKFLOW, "head_sha": self.sha,
               "status": "completed", "conclusion": "success", "event": "push"}
        verify_run(run, 42, self.sha)
        for key, value in [("repository", {"full_name": "deskflow/deskflow"}),
                           ("head_repository", {"full_name": "other/fork"}), ("workflow_id", 43),
                           ("path", ".github/workflows/other.yml"), ("head_sha", "b" * 40),
                           ("conclusion", "failure"), ("status", "in_progress"), ("event", "pull_request")]:
            with self.subTest(key=key):
                with self.assertRaisesRegex(ValueError, "successful native package workflow"):
                    verify_run(dict(run, **{key: value}), 42, self.sha)


if __name__ == "__main__":
    unittest.main(verbosity=2)
