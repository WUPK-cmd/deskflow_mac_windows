# Desktop clarity, microphone settings and packages

## Intent and authorization

The user requested microphone GUI settings, microphone-enabled packages for Windows and Mac, preservation of the working version, validation before mainline delivery, and a clearer, more attractive desktop interface. Continue in the existing repository. Implementation details below are engineering choices under that authorization.

## Approach

Extend the existing Qt Widgets application. Keep the established Core, connection protocol, settings keys, TLS identity and audio route unchanged. A web/Electron rewrite would add deployment and input-integration risk without helping this request. A documentation-only solution would leave the manual configuration problem unresolved.

## Product behavior

- Main window: clear visual hierarchy, plain explanations of sending/receiving keyboard and mouse, connection status and a direct microphone settings entry. Preserve advanced controls, keyboard accessibility, resizing and native light/dark readability. Display configured microphone routing as configuration, never as measured live audio health.
- Preferences: microphone tab, explicit opt-in, Windows physical input and virtual output selection, Mac virtual output selection, target Mac computer name and SHA-256 pin on the Windows server. Read/save the existing INI keys. Device refresh must preserve unavailable configured names; no silent fallback to the physical default. Cancel must not save microphone edits. Disabled settings retain their values.
- Validate enabled settings before saving: supported role/build, required devices, 48 kHz compatibility where discoverable, target name, 64 hexadecimal fingerprint, TLS and client verification. Explain missing drivers and provide official links. Do not alter OS sound defaults, install drivers, record audio or trust certificates automatically.
- Saving successful changes uses the existing SettingsDialog/MainWindow restart path. Show that applications must select CABLE Output or BlackHole 2ch.
- Build Windows x64 and macOS arm64 with Qt Multimedia and microphone follow ON. Produce Windows portable ZIP plus MSI, Mac app ZIP plus DMG, source identity and checksums. Do not redistribute VB-CABLE/BlackHole drivers. Release notes state signing/notarization limits and tested scope. Publication is explicit/manual after checks.
- Preserve current source with a stable tag and exact local runtime/config backup. Candidate testing must not replace the live installation or regenerate its certificates. Private settings, logs and backups never enter GitHub assets.

## Verification

Automated Qt widget tests cover load/save/cancel, invalid input, missing devices, role/build differences and retained configuration. Test real GUI entry and controls on Windows using isolated settings without disturbing the existing Core. Run appropriate regression tests. Validate package contents and launch staged binaries with developer dependency paths removed. CI provides native macOS build/tests/package verification; audio driver and target-application checks on the user's Mac remain separate if remote access is unavailable. Clearly report any unverified path.
