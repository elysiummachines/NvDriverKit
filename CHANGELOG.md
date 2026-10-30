# Changelog

All notable changes to this project will be documented in this file.

## [1.1.0] - 2026-10-31

- Fetch Settings button saves your current driver settings without opening a PowerShell prompt
- The app reports how many settings were captured, read back from the saved file rather than from the script output
- `--capture` runs the same path unattended

## [1.0.1] - 2026-10-30

- NvPostInstall.ps1 is tracked in the repo and ships inside the release zip
- Post-install script defaults to the copy beside the exe instead of a fixed F: path
- Release notes are generated from this changelog at tag time
- README header icon matches the application icon
- README binary figures describe the released build rather than a local one

## [1.0.0] - 2026-10-30

- Initial release
- Drives NVCleanstall 1.19.0 through all four wizard pages via UI Automation
- Builds a clean-install package, verifies it, and installs the driver
- Reinstalls the NVIDIA Control Panel from the Microsoft Store
- Restores saved driver profiles and DSR Factors / Smoothness
- Signature and SHA-256 pinning on both inputs, staged to a hardened ProgramData directory
- ASLR, high-entropy ASLR and DEP enabled, with a single requireAdministrator manifest
- GitHub Actions CI and a tag-driven release pipeline
