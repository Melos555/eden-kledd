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

- **Windows:** `eden-kledd-v0.3-windows.zip`. Unzip anywhere and run `eden.exe`. It uses your normal Eden user folder (`%APPDATA%\eden`), so settings, keys and games carry over.
- **Linux:** `eden-kledd-v0.3-linux-x86_64.tar.gz` or `eden-kledd-v0.3-x86_64.AppImage`. Built on Debian 13 (glibc 2.41, same as SteamOS), so the AppImage also runs on the Steam Deck.
- **v0.3** is the Metroid Prime 4 performance build: scan fix always on, plus the texture, audio and mouse-gyro work listed below. No login needed to download.

## Mods: 1080p + 120 FPS that don't break the scan visor

<p align="center">
  <a href="https://github.com/Melos555/eden-kledd/raw/main/mods/Prime4_1080p_120FPS_scan-safe.zip"><b>⬇️ Prime4_1080p_120FPS_scan-safe.zip</b></a>
</p>

- **`[1920x1080 v1.1.0] Kledd scan-safe`**: Fl4sh's 1080p patch with one fix. The scan buffer is pinned to the game's native 1600×900, so the "ZL Fix scan" cheat is **not needed** (and shouldn't be used). The original reads that size from memory only the cheat fills, which breaks or crashes the scan without it.
- **`[120FPS Dynamic v1.1.0] v1.0`**: Fl4sh's 120 FPS mod, unchanged.
- **Install:** right-click the game → *Open Mod Data Location* → copy both folders in. Turn off any other resolution mod and the ZL scan cheat. Game version 1.1.0 only.
- All credit for the original patches goes to [Fl4sh](https://github.com/Fl4sh9174/Switch-Emulator-Ultrawide-FPS-Mods) ([Ko-fi](https://ko-fi.com/Fl4sh9174)).

## What this fork changes

**Fixes the Metroid Prime 4: Beyond scan visor jumping between targets / locking onto things behind walls.**

The game reads back a small depth-stencil patch around the crosshair (the stencil holds object IDs) to decide what you're aiming at.
In Eden's Vulkan backend, downloading a `D24S8` image created the depth and stencil copy regions at the **same buffer offset**, so the two
writes overlapped and the object IDs came back garbled. This fork downloads depth and stencil into separate regions, merges them back into
the guest format on the CPU, and does it asynchronously and only for the requested region, so there's no FPS cost.

The fix is always on in the release.

### New in v0.3 (Metroid Prime 4 performance build)

Settings that won the A/B tests are built in (locked in code), so a config can't put them back:

- **Texture uploads:** the async GPU unswizzle with 128-slice chunks, and textures kept for at least 5 s of real time before the
  cleanup may delete them (fewer re-uploads when you turn around or pause). okboomerxd's delta texture upload is **off**: in every
  test it left textures black after doors.
- **Descriptor stride fix** (Amicuchu, Eden PR #4549, pointed out by John) with max 256 descriptors per array (John's idea): fixes
  wrong textures/lighting in Prime 4.
- **Hold audio during stalls** (okboomerxd): no buzzing loop when the game hitches.
- **Cheat write fix** (Eden PR #4640): no slowdown over long sessions with cheats.
- **ASTC recompression BC7 on the GPU** by default (less video memory); changeable in Graphics.
- **Mouse gyro** (Controls → Mouse panning → Configure): *Mouse to camera* (mouse look through the gyro) or *Mouse to cursor*
  (the reticle sits where the mouse is; type in Prime 4's in-game cursor sensitivity and the angles are worked out for you),
  plus a Gravity Check key.
- **TAA** tuned (SMAA inside instead of FXAA, less smear), and a **frame recorder** (Shift+F7 on/off, F7 saves a summary to
  `log/`).

Tested on Windows with an RTX 2060 Mobile (6GB VRAM); testers on Linux and AMD.

## Verify the build yourself

- All changes are in this repository's commit history, on top of upstream Eden snapshot `d8c6c59b6`.
- Builds are made by **GitHub Actions** from this public source (see the [Actions tab](../../actions) and
  `.github/workflows/`). Nothing is compiled by hand.
- The `v0.3` release files are the Actions artifacts of runs
  [42986200649](../../actions/runs/42986200649) (Windows) and [42986223648](../../actions/runs/42986223648) (Linux), commit `d91b302`,
  renamed and uploaded to the release. (`v0.1-scanfix`: runs [39457072636](../../actions/runs/39457072636) and
  [39457537419](../../actions/runs/39457537419).)
- The commit hash in Eden's title bar matches the commit the build came from.

## Credits

- **Eden team** and yuzu contributors: the emulator itself.
- **Melos**: testing, investigation, and pointing at the depth buffer; the 5 s texture cleanup and the v0.3 settings.
- **John, okboomerxd, Amicuchu**: the descriptor fixes, audio hold and other changes credited above; **Lanner, Akario** and the
  other testers.
- **Claude (Anthropic AI), a.k.a. Kledd**: reverse engineering, root-cause analysis and the patches. This fork was made with AI assistance.

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
