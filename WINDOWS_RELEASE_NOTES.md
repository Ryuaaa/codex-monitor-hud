# Codex Monitor HUD Windows 1.1.0

Windows 1.1 is the first stable Windows x64 release, aligned with the macOS 1.1
product version. It is a native Win32 and
C++17 desktop HUD with no embedded browser engine.

## Highlights

- Configurable Home, Codex, and Computer pages in one resizable always-available HUD.
- CPU, memory, process, network, disk, and Codex/ChatGPT resource attribution.
- Codex five-hour and weekly limits, usage trends, token and estimated-cost history,
  recent tasks, local activity estimation, and OpenAI service status.
- Optional weekly-limit consumption alerts with a user-selected threshold and
  natural-day or rolling-24-hour calculation.
- Daily and manual update checks with signed-MSI one-click updating.

## Security and privacy

- Authenticode signing is provided by SignPath.io, certificate by SignPath Foundation.
- The signed EXE and MSI are built from the exact public tag on GitHub-hosted runners.
- The updater verifies SHA-256, the pinned publisher certificate, MSI identity,
  installed executable identity, and exact version before switching versions.
- The app does not read browser cookies, API keys, prompts, replies, or tool output.

## Validation

- MSVC x64 Release build and the full automated Windows test suite.
- Native window startup and taskbar-minimize smoke test.
- Silent MSI install, uninstall, and real previous-version major-upgrade tests.
- Signature, checksum, publisher-pin, MSI identity, and tamper-rejection tests.

System requirement: Windows 10 or Windows 11 x64. Windows ARM64 is not yet supported.
