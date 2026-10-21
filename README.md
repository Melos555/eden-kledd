<!-- eden-kledd fork header -->
<p align="center">
  <img src="./.github/kledd/kledd.png" alt="Kledd" width="180">
</p>

<h1 align="center">eden-kledd</h1>

<p align="center"><b>An unofficial fork of <a href="https://git.eden-emu.dev/eden-emu/eden">Eden</a>.</b><br>
Not affiliated with or endorsed by the Eden team. Please don't report bugs from this build to them.</p>

> [!IMPORTANT]
> The only official Eden source is [git.eden-emu.dev/eden-emu/eden](https://git.eden-emu.dev/eden-emu/eden).
> This repository is a personal test fork. Everything below the line is Eden's own README, kept for credit and reference.

## Download

<p align="center">
  <a href="https://github.com/Melos555/eden-kledd/releases/latest"><b>⬇️ Download the latest release (Windows / Linux)</b></a>
</p>

- **Windows:** `eden-kledd-scanfix-windows.zip`. Unzip anywhere and run `eden.exe`. It uses your normal Eden user folder (`%APPDATA%\eden`), so settings, keys and games carry over.
- **Linux:** `eden-kledd-scanfix-linux-x86_64.tar.gz`. Built on Ubuntu 26.04; needs the usual Eden runtime libraries (Qt 6, SDL2 etc.).
- The release build has the scan fix **always on**, with no toggle. No login needed to download.

## What this fork changes

**Fixes the Metroid Prime 4: Beyond scan visor jumping between targets / locking onto things behind walls.**

The game reads back a small depth-stencil patch around the crosshair (the stencil holds object IDs) to decide what you're aiming at.
In Eden's Vulkan backend, downloading a `D24S8` image created the depth and stencil copy regions at the **same buffer offset**, so the two
writes overlapped and the object IDs came back garbled. This fork downloads depth and stencil into separate regions, merges them back into
the guest format on the CPU, and does it asynchronously and only for the requested region, so there's no FPS cost.

In the **release build** (branch `kledd-fix-always-on`) the fix is always on. The test builds on other branches have it behind
`[Kledd] DMA readback fix` (plus `[Kledd] DMA readback log` for debugging) under **Graphics → Extras**.

Experimental extras (gentle texture cleanup, GPU BC7 recompression, frame logging) live on the `kledd-lab` test branch only. They are
**not** in the release.

Tested on Windows with an RTX 2060 Mobile (6GB VRAM), in both Accurate and Fast GPU mode.

## Verify the build yourself

- All changes are in this repository's commit history, on top of upstream Eden snapshot `d8c6c59b6`.
- Builds are made by **GitHub Actions** from this public source (see the [Actions tab](../../actions) and
  `.github/workflows/`). Nothing is compiled by hand.
- The `v0.1-scanfix` release files are the Actions artifacts of runs
  [39457072636](../../actions/runs/39457072636) (Windows) and [39457537419](../../actions/runs/39457537419) (Linux),
  repackaged into a zip / renamed and uploaded to the release.
- The commit hash in Eden's title bar matches the commit the build came from.

## Credits

- **Eden team** and yuzu contributors: the emulator itself.
- **Melos**: testing, investigation, and pointing at the depth buffer.
- **Claude (Anthropic AI), a.k.a. Kledd**: reverse engineering, root-cause analysis and the patch. This fork was made with AI assistance.

---

<!--
# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
-->
<!-- lang: en-GB -->

<h1 align="center">
  <br>
  <a href="https://git.eden-emu.dev/eden-emu/eden"><img src="./dist/qt_themes/default/icons/256x256/eden.png" alt="Eden" width="200"></a>
  <br>
  <b>Eden</b>
  <br>
</h1>

<h4 align="center"><b>Eden</b> is a free and open-source (FOSS) Switch 1 emulator started by developer Camille LaVey.
<br>
Written in C++, with builds for Windows, Linux, macOS, Android, FreeBSD and more.
</h4>

<p align="center">
    </a>
    <a href="https://discord.gg/HstXbPch7X">
        <img src="https://img.shields.io/discord/1367654015269339267?color=5865F2&label=Eden&logo=discord&logoColor=white"
            alt="Discord">
    </a>
    <a href="https://stt.gg/qKgFEAbH">
        <img src="https://img.shields.io/revolt/invite/qKgFEAbH?color=d61f3a&label=Stoat"
            alt="Stoat">
    </a>
</p>

<p align="center">
  <a href="#compatibility">Compatibility</a> |
  <a href="#development">Development</a> |
  <a href="#building">Building</a> |
  <a href="#download">Download</a> |
  <a href="#support">Support</a> |
  <a href="#license">License</a>
</p>

## Compatibility

The emulator is capable of running most commercial games at full speed, provided you meet the necessary hardware requirements.

A list of supported games will be available in future. Please be patient.

Check out our [website](https://eden-emu.dev) for the latest news on exciting features, monthly progress reports, and more!

[![Packaging status](https://repology.org/badge/vertical-allrepos/eden-emulator.svg)](https://repology.org/project/eden-emulator/versions)

## Contribute

To contribute to Eden; be it financially, code, bug reports, or otherwise, see our [Contributing guidelines](./CONTRIBUTING.md).

## Documentation

We have a user manual! See our [User Handbook](./docs/user/README.md).

## Building

See the [General Build Guide](docs/Build.md)

For information on provided development tooling, see the [Tools directory](./tools)

## Download

You can download the latest releases from [our release page](https://git.eden-emu.dev/eden-emu/eden/releases).

Save us some bandwidth! We have [mirrors available](./docs/user/ThirdParty.md#mirrors) as well.

## License

Eden is licensed under the GPLv3 (or any later version). Refer to the [LICENSE.txt](https://git.eden-emu.dev/eden-emu/eden/src/branch/master/LICENSE.txt) file.

## Special thanks

Super special thanks to Cloudflare for preventing the git server from blowing up.

- Yuzu
- Ryujinx
- Sudachi
- Citron
- Torzu
- Suyu
- Ryubing

And everyone who continues or had contributed to the project! <3
