# Desktop and microphone implementation plan

**Goal:** Make the existing Deskflow understandable and configurable, and deliver reproducible microphone-enabled desktop packages without replacing the working installation.

**Architecture:** Existing Qt Widgets UI and existing INI/Core restart flow. Independent UI, microphone preferences and packaging work; integrate and verify together.

**Tech stack:** C++/Qt 6.8.3 Widgets and Multimedia, Qt Test, CMake/CPack, GitHub Actions.

**Spec:** [Design](../specs/2026-11-16-desktop-microphone-design.md).

## Global constraints

Preserve source/runtime/configuration before testing. No private settings or certificate material in commits/assets. Microphone stays opt-in. Preserve installed virtual drivers, default sound devices, audio transport, input ordering and clipboard behavior. Keep candidate tests isolated from the live installation. Do not claim virtual audio hardware passed based on CI.

## Review focus

- Missing or renamed sound device must remain visible and not silently select another input.
- Invalid microphone configuration must block saving without modifying other settings.
- Saving disabled microphone settings must preserve target and fingerprint.
- Connection labels must distinguish running/listening from a connected peer and configured routing from live audio health.
- Archives/installers must contain Qt Multimedia runtime/plugins, build identity and instructions, and exclude developer/user settings.

## Tasks

- [x] 1. Snapshot working Windows runtime/config privately; record hashes; create stable source tag and candidate branch. Verify backup file hashes and unchanged active connection.
- [x] 2. Add microphone preferences widget and integrate SettingsDialog load/save/reset/modified/validation. Add meaningful Qt tests, run RED then GREEN using isolated settings. Do not capture audio in tests.
- [x] 3. Improve MainWindow hierarchy, role descriptions, status and microphone entry without replacing control flow. Add relevant widget coverage and inspect the real rendered application.
- [x] 4. Enable Multimedia in fork CI, create Windows MSI/ZIP and macOS DMG/ZIP with runtime verification and checksums, and add explicit release publication. Validate workflows/scripts and run native packaging.
- [x] 5. Integrate, update Chinese translations and usage/build documentation. Run the safe regression suite and actual isolated Windows GUI paths. Review the complete diff and resolve findings.
- [ ] 6. After local checks, push candidate branch for native Mac CI; inspect results and both artifacts. Only then advance main and publish the reviewed packages. Keep the old stable tag/runtime intact; report unverified target-hardware paths.
