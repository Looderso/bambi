# 0261 · The macOS release is signed, notarised and installed by a package; Windows and Linux ship unsigned

**Status:** accepted

*2026-10-21. The live host ships with the beta, and on macOS it is three things in three places.*

**macOS.** Every bundle — the plugins, the Decoder's nested helper, the live host and its loop driver — is signed
with a Developer ID under the hardened runtime and notarised. The download is an installer package with two
choices: *Plugins* into `/Library/Audio/Plug-Ins`, and *Live host*: the app into `/Applications`, the loop driver
into `/Library/Audio/Plug-Ins/HAL` and a Core Audio restart. The driver alone rules out a copy-it-yourself install:
it needs an administrator and that restart. An uninstall script is installed beside the licences, since a package
cannot remove what it installed. A notarised zip of the plugins stays for anyone who copies them by hand. The Max for
Live device is not in the package: the installer runs as root and cannot know whose Live to serve, so the host
copies the device from its bundle into the User Library its own user's Live names, at every start, whenever it
differs. All of it is `tools/release/macos/` and the build workflow's `macos-package` job, on a tag or a run started
by hand.

**The loop driver carries none of BlackHole's name.** BlackHole's licence keeps its name and branding off
third-party builds: the driver, its device and its box are named and made by *bambi*, and the notices credit
BlackHole's copyright under the GPLv3. Its source at the pinned commit goes into each release's source archive.

**Windows and Linux ship unsigned.** A host loading a plugin DLL does not go through SmartScreen, and Linux has no
signing to pass; the live host does not exist there. Signing for Windows is taken up when someone asks for it.

What would overturn it: a host or a macOS version that refuses a system-wide plugin install, or a Windows host
that blocks unsigned plugins.
