# Changelog

All notable changes to this project will be documented in this file.

## [1.2.0] - 2026-11-08

- Dark mode, covering the title bar, the window, the buttons, the status line and the log
- A Theme button cycles Auto, Light and Dark; Auto follows the Windows setting and changes with it while the app is open
- The choice is remembered in `[ui] Theme` in `config.ini`
- `--dark`, `--light` and `--theme-auto` set it from the command line
- Log lines are coloured by level: steps, successes, warnings and failures are now distinguishable at a glance
- The log moved from a plain edit to a rich edit, and its text cap was raised so a long run cannot lose its tail
- Buttons are owner-drawn in both themes, with hover, pressed, focus and disabled states
- Minimum window width is 780, wide enough for the status line beside the new button

## [1.1.1] - 2026-10-30

- `verify-binary.ps1` rejects an unknown parameter instead of silently checking the default binary
- README binary size figures match the released build

## [1.1.0] - 2026-10-31

- Fetch Settings button saves your current driver settings without opening a PowerShell prompt
- The app reports how many settings were captured, read back from the saved file rather than from the script output
- `--capture` runs the same path unattended
- The preflight message for missing settings points at the button instead of a PowerShell command
- Release builds stamp the window title and the manifest identity as well as the file properties
- The repository carries the version being prepared, so a plain local build shows the right number

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
