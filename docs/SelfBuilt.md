# Building and accepting the Windows/macOS fork

This fork's source is [WUPK-cmd/deskflow_mac_windows](https://github.com/WUPK-cmd/deskflow_mac_windows).
The **Self-built Windows and macOS** workflow runs on `main` and `codex/**` pushes, pull requests and manual dispatches.
It compiles and tests Windows x64 and macOS arm64 with Qt 6.8.3, then uploads runtime ZIPs, SHA-256 checksums,
`build-info.json`, and test evidence. Download both host artifacts from the **same workflow run**. On a pull
request, `build-info.json.commit` identifies the tested merge commit; use that value rather than assuming
the PR head is the build input. The separate upstream workflow remains unchanged.

These are Actions artifacts, retained for 14 days. This workflow does not create tags, publish Releases,
sign with a distribution certificate, notarize, or install on either host. Its macOS bundle is ad hoc signed.
Intel Macs need a native x86_64 build; the arm64 CI job does not verify Intel compatibility. The version
checker reads this fork's latest published stable GitHub Release. An empty release channel, API failure or
404 produces no update notification. CI artifacts do not become Releases automatically. Opening the download
entry leads to this fork's Releases page, where there may be no release yet; builds can be obtained from
[Actions](https://github.com/WUPK-cmd/deskflow_mac_windows/actions/workflows/self-built-ci.yml).
An explicitly configured `gui/updateCheckUrl` override is retained; review that override if an existing
installation still checks a different endpoint.

The application and settings identity remains Deskflow, including the `deskflow.org` organization domain.
Existing settings and TLS pairing must remain usable. The microphone feature keeps its project default
`BUILD_MICROPHONE_FOLLOW=OFF`; it is outside the acceptance scope below. For a deliberate microphone build,
install Qt Multimedia and add `-DBUILD_MICROPHONE_FOLLOW=ON` on **both** hosts, then accept that feature separately.

## Build the same commit on both hosts

Local build commands below exclude the legacy native clipboard and macOS key-injection test executables,
which can change the live clipboard or send keys to the focused app. The remaining tests use isolated
settings, synthetic platform adapters, or temporary loopback connections. Run the native acceptance matrix
below in a disposable text editor; do not start native key-injection tests with a terminal focused.

Choose a reviewed commit and retain its full 40-character SHA in the acceptance record. Clone the fork, fetch
its tags and check out that SHA on each host. Do not compare only the marketing version: different source
commits can share that version. Start with an empty build directory when changing Qt, compiler or architecture.

### Windows x64

Requirements: Git, Python 3, CMake 3.24+, Ninja, and a Visual Studio 2022 x64 developer shell with the Desktop
C++ workload and Windows SDK. The following commands run in **Developer PowerShell for VS 2022**. Replace
the first value with the reviewed SHA; the dependency paths are separate from the source checkout.

```powershell
$DeskflowCommit = 'REPLACE_WITH_REVIEWED_40_CHARACTER_SHA'
git clone https://github.com/WUPK-cmd/deskflow_mac_windows.git deskflow-self-built
Set-Location deskflow-self-built
git fetch --tags origin
git checkout --detach $DeskflowCommit
if ((git rev-parse HEAD) -ne $DeskflowCommit) { throw 'Wrong source commit' }

$DeskflowDeps = Join-Path (Split-Path -Parent $PWD.Path) 'deskflow-deps'
New-Item -ItemType Directory -Path $DeskflowDeps -Force | Out-Null
python -m venv "$DeskflowDeps/aqt"
& "$DeskflowDeps/aqt/Scripts/python.exe" -m pip install aqtinstall==3.3.0
& "$DeskflowDeps/aqt/Scripts/python.exe" -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O "$DeskflowDeps/Qt"
git clone --branch 2026.10.16 --depth 1 https://github.com/microsoft/vcpkg.git "$DeskflowDeps/vcpkg"
& "$DeskflowDeps/vcpkg/bootstrap-vcpkg.bat" -disableMetrics
& "$DeskflowDeps/vcpkg/vcpkg.exe" install openssl:x64-windows-release --classic --disable-metrics
if ($LASTEXITCODE -ne 0) { throw 'Dependency build failed' }

$DeskflowQt = "$DeskflowDeps/Qt/6.8.3/msvc2022_64"
$env:PATH = "$DeskflowQt/bin;$DeskflowDeps/vcpkg/installed/x64-windows-release/bin;$env:PATH"
$env:QT_QPA_PLATFORM = 'offscreen'
cmake -S . -B build-self-built -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DBUILD_TESTS=ON -DSKIP_BUILD_TESTS=ON -DBUILD_INSTALLER=OFF `
  -DCMAKE_COMPILE_WARNING_AS_ERROR=ON -DCMAKE_PREFIX_PATH="$DeskflowQt" `
  -DCMAKE_TOOLCHAIN_FILE="$DeskflowDeps/vcpkg/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-release -DVCPKG_MANIFEST_MODE=OFF `
  -DOPENSSL_ROOT_DIR="$DeskflowDeps/vcpkg/installed/x64-windows-release"
if ($LASTEXITCODE -ne 0) { throw 'Configure failed' }
cmake --build build-self-built --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
ctest --test-dir build-self-built --output-on-failure --parallel 2 --timeout 120 -E '^(MSWindowsClipboardTests|OSXClipboardTests|OSXKeyStateTests)$'
if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
cmake --install build-self-built --prefix "$PWD/staging-self-built"
if ($LASTEXITCODE -ne 0) { throw 'Staging failed' }
Get-ChildItem "$env:VCToolsRedistDir/x64/Microsoft.VC*.CRT" -Directory | ForEach-Object {
  Copy-Item "$($_.FullName)/*.dll" staging-self-built
}
& ./staging-self-built/deskflow-core.exe --version
Remove-Item Env:QT_QPA_PLATFORM
& ./staging-self-built/deskflow.exe
```

Retain the configure/build/test output, `build-self-built/Testing/Temporary/LastTest.log`, `git rev-parse HEAD`,
`git status --short`, and hashes of the staged GUI/Core. In an existing checkout, keep its remotes and use
`git fetch origin` plus `git checkout --detach <reviewed SHA>` only after preserving any local changes.

### macOS, including Dot's native acceptance host

Requirements: Xcode command-line tools, Git, Python 3, Homebrew, CMake 3.24+, and Ninja. Use a native terminal
(`uname -m` must match the intended architecture; do not build arm64 dependencies under Rosetta).
Homebrew OpenSSL is maintained independently, so record its actual version alongside Qt and the compiler.

```bash
DeskflowCommit='REPLACE_WITH_REVIEWED_40_CHARACTER_SHA'
git clone https://github.com/WUPK-cmd/deskflow_mac_windows.git deskflow-self-built
cd deskflow-self-built
git fetch --tags origin
git checkout --detach "$DeskflowCommit"
test "$(git rev-parse HEAD)" = "$DeskflowCommit" || exit 1

brew install cmake ninja openssl@3
DeskflowDeps="$(cd .. && pwd)/deskflow-deps"
python3 -m venv "$DeskflowDeps/aqt"
"$DeskflowDeps/aqt/bin/python" -m pip install aqtinstall==3.3.0
"$DeskflowDeps/aqt/bin/python" -m aqt install-qt mac desktop 6.8.3 clang_64 -O "$DeskflowDeps/Qt"
export PATH="$DeskflowDeps/Qt/6.8.3/macos/bin:$PATH"
DeskflowArch="$(uname -m)"
case "$DeskflowArch" in arm64|x86_64) ;; *) exit 1 ;; esac
cmake -S . -B build-self-built -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=ON -DSKIP_BUILD_TESTS=ON -DBUILD_INSTALLER=OFF \
  -DCMAKE_COMPILE_WARNING_AS_ERROR=ON \
  -DCMAKE_PREFIX_PATH="$DeskflowDeps/Qt/6.8.3/macos" \
  -DCMAKE_OSX_ARCHITECTURES="$DeskflowArch" -DCMAKE_OSX_DEPLOYMENT_TARGET=14 \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" || exit 1
cmake --build build-self-built --parallel 4 || exit 1
QT_QPA_PLATFORM=offscreen ctest --test-dir build-self-built --output-on-failure --parallel 2 --timeout 120 -E '^(MSWindowsClipboardTests|OSXClipboardTests|OSXKeyStateTests)$' || exit 1
cmake --install build-self-built --prefix "$PWD/staging-self-built" || exit 1
macdeployqt "$PWD/staging-self-built/Deskflow.app" \
  "-executable=$PWD/staging-self-built/Deskflow.app/Contents/MacOS/deskflow-core" -codesign=- || exit 1
codesign --verify --deep --strict staging-self-built/Deskflow.app || exit 1
staging-self-built/Deskflow.app/Contents/MacOS/deskflow-core --version
open "$PWD/staging-self-built/Deskflow.app"
```

This manual recipe targets macOS 14+ on either native architecture. A build for an older OS needs a separate
deployment target and acceptance on that OS. If using CI artifacts, verify the archive checksum first and
extract the macOS ZIP with `ditto -x -k <archive.zip> <new-directory>` to preserve the bundle. macOS may require
explicit approval in Privacy & Security for an unnotarized application. Grant Accessibility/Input Monitoring
to the actual candidate application when requested, and record any blocked permissions rather than treating
their denial as a successful input test. Do not disable system security or delete quarantine attributes globally.

## Update and roll back

1. Save the last accepted runtime, its manifest/checksum, and both hosts' settings/TLS files before an update.
   Find the active settings location in the GUI/log and consult [Configuration](Configuration.md); copying
   only an assumed default path can miss portable or custom configurations. Keep private keys private.
2. Stage the candidate in a new directory on both hosts. Check that both manifests name the **same commit**
   and expected architecture; compare the downloaded ZIP's SHA-256 with `SHA256SUMS.txt` (`Get-FileHash` on
   Windows, `shasum -a 256` on macOS). Hashes of the two hosts' different binaries are not expected to match.
3. Stop sharing and quit the old GUI through its menu; wait for its Core to exit. For Windows service mode,
   use that installation's service controls. Do not overwrite files while the service/Core still runs.
   Start the new GUI by its full path, then start sharing. Keep the previous runtime until acceptance passes.
4. Verify existing screen layout, settings, trusted TLS peer/fingerprint and connection role. Replacing a
   self-built binary must not silently reset pairing. Follow the acceptance table below before switching
   login/startup shortcuts or the service's executable path to the candidate.
5. To roll back, stop/quit the candidate, confirm its Core exited, and launch the retained accepted GUI by
   its full path. Restore the saved settings only if they changed incompatibly; retain the failed candidate's
   logs first. Restore any startup/service path changed in step 4. Recheck identity, pairing, two-way input,
   clipboard and GUI Stop/Start. A rollback is complete only when the old pair works again.

Use the GUI's normal controls for these steps. Forced termination is reserved for the specific fault tests
below and must target a verified test PID. Never terminate every `deskflow*` process by name.

## Dot / two-host acceptance record

Record the final reviewed full commit, each host's OS/architecture, Qt/OpenSSL/compiler versions, candidate
paths, GUI/Core PIDs, process paths/command lines, roles, screen arrangement and DPI, mouse polling rate,
connection type and each test's start/end time. Verify process identity after **every** restart/upgrade;
a successful build in one directory says nothing about an old process running elsewhere.

Windows process evidence (read-only):

```powershell
Get-CimInstance Win32_Process | Where-Object { $_.Name -in @('deskflow.exe', 'deskflow-core.exe', 'deskflow-daemon.exe') } |
  Select-Object ProcessId, ParentProcessId, ExecutablePath, CommandLine
Get-FileHash -Algorithm SHA256 ./staging-self-built/deskflow.exe, ./staging-self-built/deskflow-core.exe
```

macOS process evidence (read-only):

```bash
pgrep -fl 'Deskflow|deskflow-core'
ps -axo pid,ppid,command | grep -E '[D]eskflow|[d]eskflow-core'
# For each verified GUI/Core PID, record the loaded executable path:
lsof -a -p <PID> -d txt
shasum -a 256 staging-self-built/Deskflow.app/Contents/MacOS/{Deskflow,deskflow-core}
```

Run each direction where applicable, with Windows and macOS each serving as the controlled host. Start from
the normal GUI, connect the paired peer, perform the action, and verify the final application state/output.
Use test documents and applications. Preserve concise logs and screenshots/video of critical steps; a
screenshot of an open window does not establish successful paste, release, reconnect or persistence.

| Path | User actions and required result |
| --- | --- |
| Identity and pairing | Launch both reviewed runtimes by full path. Check commit parity, About/version output, loaded GUI/Core paths, role, screen arrangement and existing TLS trust. Stop/Start in GUI; state and real connectivity agree. |
| Text, HTML and images | In both directions paste ASCII, Chinese/emoji/multiline text, rich HTML with formatting/links and a known image into suitable native target apps. Compare actual content, formatting, dimensions and image appearance; check clipboard formats where possible. |
| Rapid copy | Copy at least 20 numbered distinct values quickly, then paste after the final copy. The final value must arrive without stale overwrite; repeat with HTML and images, and after crossing back to the source screen. |
| Clipboard busy | Hold Windows `OpenClipboard` for one second while a Mac copy is delivered. Coordinate the copy so delivery occurs during the lock; after release, verify the pending content arrives without another copy. Repeat a Windows read while another app holds it. Retries run every 50 ms, up to 40 attempts (about two seconds when the event loop is responsive). Separately test a lock beyond that budget: expect a warning and responsive input, then verify a fresh copy works after release. Record lock/delivery times and logs. |
| Ordinary and high-rate movement | At the usual polling rate, move slowly, diagonally, in small circles and repeatedly across edges for 60 seconds; repeat at an available higher rate. Check endpoints, overshoot, pauses and return behavior. Repeat during large text/image transfer and continuous key input; record measurements or video, without inventing a latency bound. |
| Buttons, wheels and modifiers | Test left/right/middle and available extra buttons; small vertical wheel increments, reversals and horizontal wheel. Test left/right Shift/Ctrl/Alt/Meta independently, shortcuts, held modifiers across transitions, Chinese input and release on return. Confirm no stuck state or phantom click. |
| Drag | Drag a window and select text with left button held. Repeat right/middle drags in apps that support them and modifier-assisted drag. Check continuous motion, correct drag button and release at destination, across a screen edge and back. |
| Move while copying | Keep ordinary movement, wheel and typing active while repeatedly transferring a large image/text payload. Inputs must continue, clipboard must finish correctly, and the GUI must remain controllable. Compare with idle under the same host/network conditions. |
| Disconnect during drag | With a known test drag and held modifier on the remote screen, stop the peer Core or use an authorized test-link interruption. Verify local return and button/modifier release on both hosts. Restore the peer/link, reconnect without relaunching the server and repeat a drag/click/shortcut successfully. |
| Close and reconnect | Repeatedly stop/start a peer and reconnect; stop sharing while the peer is unavailable or not reading. Verify bounded shutdown, subsequent listener availability, consistent GUI state and no steadily growing process handles/memory after repeated cycles. Keep timing/resource observations. |
| Sleep and wake | Sleep/wake each host separately and both together. After wake, verify local input, reconnect, screen transitions, clipboard and no held buttons/modifiers. If testing Wi-Fi/address changes, use only an authorized test network and record the changed address. |
| GUI/Core restart and orphan | Restart GUI normally and Core through GUI. Then terminate only a verified test GUI PID while Core remains, relaunch the candidate GUI and verify adoption/state plus working Stop/Start. Terminate only a verified test Core PID; confirm recovery and controls. Repeat service/watchdog recovery if service mode is actually used; otherwise mark it untested. |
| Upgrade and rollback | With the previous Core deliberately left behind in an isolated test, start the reviewed GUI; verify controlled replacement/adoption as appropriate, correct process identity and working sharing. Execute the rollback procedure above, then the update again, preserving settings and pairing. |
| Mixed DPI and multiple monitors | Test every real screen edge, mixed scale factors, negative coordinates, non-aligned monitors/gaps, corners and quick back-and-forth transitions. Repeat drag and small wheel increments on each display. Verify normal and configured emergency/local return. Record any topology unavailable on the host. |

For the short clipboard-busy row, this separate Windows test terminal holds the clipboard for one second.
Coordinate the Mac copy with the helper (using scheduled commands if needed), and confirm delivery actually
occurred during the lock. After release, paste on Windows without a second copy. The short lock stays within
the retry budget; a longer lock tests exhaustion instead of guaranteed deferred delivery. The `finally` block
releases the clipboard even if the wait is interrupted.

```powershell
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class DeskflowClipboardLockTest {
    [DllImport("user32.dll")] public static extern bool OpenClipboard(IntPtr owner);
    [DllImport("user32.dll")] public static extern bool CloseClipboard();
}
'@
if (![DeskflowClipboardLockTest]::OpenClipboard([IntPtr]::Zero)) { throw 'Clipboard already busy; retry the helper' }
try {
    Write-Host 'Clipboard held: copy from the Mac now'
    Start-Sleep -Seconds 1
} finally {
    [DeskflowClipboardLockTest]::CloseClipboard() | Out-Null
}
```

For every row, record `PASS`, `FAIL` or `BLOCKED/NOT RUN`, exact steps, expected and observed results, and evidence
paths. A build/test pass is not a substitute for macOS native acceptance. Classify failures before changing code:

- **Permission/environment:** Accessibility/Input Monitoring denied, incompatible OS/architecture, missing runtime,
  firewall/link failure, unavailable display/polling hardware, or clipboard owner that never releases. Record the
  specific blocker and retest after resolving it; do not mark the application path passed.
- **Native input:** Protocol/test logs show delivery, but the destination app receives a wrong button, drag, wheel,
  modifier or coordinate. Capture receiving-host/app evidence, event details, topology and permission state.
- **Protocol/lifecycle/clipboard:** Connection/state cleanup, data ordering, clipboard retry, GUI control or recovery
  fails with the environment usable. Retain both host logs, source commit, repro timing and final process identity.
- **Performance:** Record polling rate, payload, network and measurement method, then compare repeat runs against the
  last accepted version under matching conditions. Report observed distributions/pauses, not an assumed cause.

Before delivery, attach the completed record and explicitly name every unverified path. The final commit is the
one verified in the artifact/source manifests and running binaries, not a branch name that can move later.

Workflow references: [Qt installer action](https://github.com/jurplel/install-qt-action),
[GitHub runner architectures](https://docs.github.com/en/actions/reference/runners/github-hosted-runners),
[artifact retention and permissions](https://github.com/actions/upload-artifact),
[Qt macOS deployment](https://doc.qt.io/qt-6/macos-deployment.html), and
[vcpkg registry release](https://github.com/microsoft/vcpkg/releases/tag/2026.10.16).
