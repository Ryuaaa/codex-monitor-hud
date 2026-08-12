# Code signing policy

Free code signing for the Windows release is provided by
[SignPath.io](https://signpath.io/), certificate by
[SignPath Foundation](https://signpath.org/).

## Project and source

- Project: Codex Monitor HUD
- Source repository: <https://github.com/Ryuaaa/codex-monitor-hud>
- Download and release page: <https://github.com/Ryuaaa/codex-monitor-hud/releases>
- License: MIT

Only artifacts built from this repository by the project's GitHub Actions
workflows are eligible for release signing. Release-signing requests must use
the exact tagged source and the automated GitHub-hosted build.

## Team roles

- Committer and reviewer: [Ryuaaa](https://github.com/Ryuaaa)
- Signing approver: [Ryuaaa](https://github.com/Ryuaaa)

Changes from outside contributors require review before they can be merged.
The signing approver is responsible for confirming that a tagged build is an
intended public release before approving its signing request.

## Privacy

See the project [privacy policy](PRIVACY.md). This program will not transfer any
information to other networked systems unless specifically requested by the
user or the person installing or operating it. Those user-requested features
include reading Codex account data through the locally installed official
Codex interface, reading OpenAI's public service-status endpoint, and checking
this repository's public GitHub Releases for updates. It does not upload
performance history, conversation content, prompts, account tokens, cookies,
or API keys.

## Security and reproducibility

- Repository and signing accounts must use multi-factor authentication.
- Windows release binaries are built on GitHub-hosted runners from the exact
  public tag.
- The signing service verifies the build origin before signing.
- The release pipeline verifies version metadata, tests, signatures, installer
  identity, and SHA-256 checksums before publishing.
- Security issues should be reported through the repository's private security
  reporting channel as described in [SECURITY.md](SECURITY.md).
