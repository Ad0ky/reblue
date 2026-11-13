<div align="center">

# re:Blue for Nintendo Switch

**Blue Dragon (Xbox 360), recompiled to run natively on the Nintendo Switch.**

[![Release](https://img.shields.io/github/v/release/Ad0ky/reblue?include_prereleases&label=release)](https://github.com/Ad0ky/reblue/releases)
[![Downloads](https://img.shields.io/github/downloads/Ad0ky/reblue/total?label=downloads)](https://github.com/Ad0ky/reblue/releases)
[![License: BSD-3-Clause](https://img.shields.io/badge/license-BSD--3--Clause-blue)](LICENSE)
![Nintendo Switch](https://img.shields.io/badge/Nintendo%20Switch-homebrew-E60012)
![deko3d](https://img.shields.io/badge/graphics-deko3d-5E5E5E)

### [Download](https://github.com/Ad0ky/reblue/releases) · [Installation guide](docs/INSTALL-SWITCH.md) · [Report a problem](https://github.com/Ad0ky/reblue/issues)

By **Ad0ky**, based on [zolaware's re:Blue](https://github.com/zolaware/reblue).
Free and open source: if you paid for this, ask for a refund.

</div>

## What this is

A homebrew build of re:Blue for Switch consoles running custom firmware (Atmosphère). The game's Xbox 360 code is statically recompiled to native ARM64, and its graphics run on **deko3d**, the Switch's own graphics API. It plays at the Switch's stock clocks.

You need your own Blue Dragon discs. This project contains no game files.

> **Experimental.** The game is playable, but expect rough edges: report crashes, missing effects or anything that looks or sounds different from the Xbox 360 ([how](#reporting-a-problem)).

## Performance

Measured at the Switch's **stock clocks** (CPU 1020 MHz, GPU 460 MHz) in handheld mode, with the scene rendered at 960×540:

| Scene | Frame rate at stock clocks |
| :--- | ---: |
| Most of the field, towns and dungeons | **25–30 FPS**, the game's cap is 30 |
| The castle town's main street, with its crowd | **16–18 FPS** |

Entering a new area can hitch for a moment while its files load. The castle town's main street is limited by both the GPU (about 1.6 million vertices a frame) and the CPU (the animation of its many characters). Work on both continues.

## Getting started

You need:

- a Switch with **Atmosphère** and the Homebrew Menu;
- a **microSD card (exFAT)** with about **15 GB** free;
- your **Blue Dragon discs** (all three, Global release: US/EU);
- a **PC**, to prepare the game files once with the [PC version of re:Blue](https://github.com/zolaware/reblue/releases/latest).

Then:

1. **Prepare the game files:** install the PC version and let its installer read your three discs. It leaves a `game` folder.
2. **Copy `reblue.nro`** to `sdmc:/switch/reblue/`, and the whole `game` folder into `sdmc:/switch/reblue/game/`.
3. **Start the Homebrew Menu through title takeover:** hold **R** while starting any installed game, then choose **re:Blue**. Starting from the Album doesn't work, because the game needs the full application memory.

The [installation guide](docs/INSTALL-SWITCH.md) has every step, the SD card layout and how to update.

### Four-core mode

Started from a **forwarder** that uses the 39-bit address space, the game can use all four CPU cores: the audio and shader compile threads move to the fourth core, away from the game. On the first start from a forwarder the game writes the forwarder's NPDM to `sdmc:/atmosphere/contents/<forwarder id>/exefs/main.npdm`; it takes effect from the next start. Through title takeover the game runs on three cores, as before.

## Settings

Most options are in the game's own **Settings** menu. The rest are in `sdmc:/switch/reblue/profiles/default/reblue.toml`, created on the first start. Each line is `name = value`; anything not listed keeps its default. The release's `reblue.toml` has the settings recommended for the Switch.

| Option | Default | What it does |
| :--- | :---: | :--- |
| `bd_shadow_distance` | 1.0 | How far sun shadows reach. 2.0 removes most of the shadow pop-in and costs some speed in busy areas. |
| `bd_shadow_update_interval` | 1 | The sun shadow map is redrawn every Nth frame. 2 or more is faster, but shadows of moving characters can flicker. |
| `bd_shadow_min_pixels` | 4.0 | Objects smaller than this on screen cast no shadow. Lower it if mid-sized shadows go missing. |
| `bd_reflection_update_interval` | 2 | Water reflections are redrawn every Nth frame. 1 redraws them every frame. |
| `bd_reflection_min_pixels` | 3.0 | Objects smaller than this are left out of the reflection. |
| `bd_interp_min_fps` | 36 | Above 30 FPS the game smooths motion between its 30 Hz updates. Below this rate smoothing turns off and the game holds 30 FPS. |
| `bd_render_thread` | true | Draws are recorded on a thread of their own, on another core. |
| `bd_scene_color_r11g11b10` | false | A lighter scene color format (recommended: true). Set false if glows or transparency look wrong. |
| `bd_opt_audio_hints` | 1 | The game's Audio Hints: spoken and on-screen notices such as "Playable" or "Checkpoint". |
| `bd_gpu_profile` | 1 | GPU timing in the log: 0 off, 1 render passes, 2 per shader (costs GPU time). |

The [installation guide](docs/INSTALL-SWITCH.md#settings) explains the release's `reblue.toml`.

## Reporting a problem

[Open an issue](https://github.com/Ad0ky/reblue/issues) with:

- the release version;
- handheld or docked, and whether you start it through title takeover or a forwarder;
- your `reblue.toml`;
- what you were doing, and where in the game;
- a photo or a short video if it is a graphics problem.

Also attach these files from the SD card, from the session where it happened:

- the newest game log in `sdmc:/switch/reblue/logs/` (`reblue_001.log` or similar);
- `sdmc:/switch/reblue/logs/rex/rex_stderr.log`;
- `sdmc:/switch/reblue/logs/rex/rex_crash.log`, if there is one.

For a performance report, create an empty file `sdmc:/switch/reblue/logs/rex/perfil_pilas.flag` before starting the game and also attach `logs/rex/rex_perfil.log`. The game runs a little slower while that file exists, so delete it afterwards.

Don't attach the game's files (`game/`).

## For developers

- [docs/SWITCH.md](docs/SWITCH.md): building the Switch version, how the port works, every optimization with its measurements, logs and diagnostics.
- [docs/SWITCH-deko3d.md](docs/SWITCH-deko3d.md): the deko3d graphics back end.
- **Building:** on Windows with Docker, `run-switch-build.bat -Pgo use -Lto -InPlace` builds the release NRO (profile-guided, link-time optimized, in-place localized copies). The build needs the files from your own copy of Blue Dragon.

This repository also carries the rest of upstream's source, so the PC versions still build from it. For those, use [zolaware/reblue](https://github.com/zolaware/reblue).

## Credits

- **Switch port:** [Ad0ky](https://github.com/Ad0ky).
- **re:Blue:** [crack](https://github.com/tomcl7) (project lead) and [rcold](https://github.com/RC0ld), with the playtesters, translators and contributors listed in [upstream's README](https://github.com/zolaware/reblue#credits).
- **Research and tools:**
  - [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) and [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) by hedge-dev, [plume](https://github.com/renderbag/plume) and [Xenia](https://github.com/xenia-project/xenia);
  - on the Switch: [devkitPro](https://devkitpro.org), [libnx](https://github.com/switchbrew/libnx), [deko3d](https://github.com/devkitPro/deko3d) and [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere);
  - ChanseyIsTheBest's [renut-nx base](https://github.com/ChanseyIsTheBest/renut_nx_ultimate_base), for the four-core mode and its optimization guide.
- **Upscaling:** thanks to AMD for [FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) (MIT), used by the experimental dynamic resolution.

Blue Dragon and its assets belong to their respective owners. This is an unofficial, non-commercial project under the [BSD 3-Clause License](LICENSE); dependencies keep their own licenses. Use data from discs you own, and don't commit game executables, archives, textures, audio, video, generated game code or captures to this repository.
