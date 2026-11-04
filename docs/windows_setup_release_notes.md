# Tourmaline Windows Setup — Release Notes

This draft GitHub Release contains the **Windows x64 Tourmaline Setup installer**.

## Included

- Guided creation of a Tourmaline project using the native `Project.exe init` command
- Project-only, localhost asset repository, or authenticated remote repository configuration
- User-token setup for remote repositories and an optional Maya asset picker installation
- Built native `Project.exe` runtime and the setup components packaged into the installer

## Installation

Download **TourmalineSetup-Windows.exe** from this release, verify its
SHA-256 digest against **SHA256SUMS.txt**, and run the installer on Windows x64.
Choose **Tourmaline Setup Wizard** from Start after installation.

The installer is built using the source code at the release's Git tag.
It does not install Maya, graphics drivers, system compiler toolchains,
or every optional Tourmaline editor dependency. The application binaries and
runtime DLLs should be validated on a clean Windows VM before publishing.

## Linux

There is no Linux binary installer in this release. Linux continues to use
`sh setup_tourmaline.sh` or `python3 tools/setup_wizard.py --terminal` with
an existing native `Project` build.

## Before publishing the draft release

- [ ] Windows CI and installer build completed successfully
- [ ] Install/uninstall and project creation tested on a clean Windows x64 VM
- [ ] Remote HTTPS login and local server startup tested
- [ ] Validate all runtime DLLs, redistribution licenses and malware-scanning results
- [ ] Confirm version/tag and SHA256SUMS.txt match the attached installer
- [ ] Sign the installer where appropriate, and verify the signature

**Release channel:** installer-only tags named `setup-vMAJOR.MINOR.PATCH`.
The release automation creates a **draft**; a maintainer publishes it after validation.
