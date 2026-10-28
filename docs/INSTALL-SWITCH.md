# re:Blue for Nintendo Switch: installation

An **experimental** homebrew build of re:Blue (the Blue Dragon recompilation)
for Switch consoles running custom firmware. The recompiled game runs natively
on the console and draws through deko3d, the Switch's own graphics API. You
need your own Blue Dragon discs; this download contains no game files.

> **Expect rough edges.** The game is playable, tested in handheld mode: most
> of the field runs at 28–30 fps, the heaviest areas drop to about 17–20 fps,
> and entering a new area can hitch for a moment. Crashes and graphics glitches
> are possible. Please report problems with your logs (see
> [Reporting a problem](#reporting-a-problem)).

## Contents

1. [What you need](#what-you-need)
2. [Step 1: Prepare the game files on a PC](#step-1-prepare-the-game-files-on-a-pc)
3. [Step 2: Copy everything to the SD card](#step-2-copy-everything-to-the-sd-card)
4. [Step 3: Start the game](#step-3-start-the-game)
5. [Settings](#settings)
6. [Updating](#updating)
7. [Troubleshooting](#troubleshooting)
8. [Reporting a problem](#reporting-a-problem)

## What you need

- **A Switch with custom firmware:** Atmosphère with the Homebrew Menu
  (hbmenu).
- **A microSD card formatted as exFAT** with about **15 GB free**. The game
  files take about 13 GB.
- **Your own Blue Dragon discs** (Xbox 360, all three).
- **A PC** to prepare the game files with the PC version of re:Blue.
- **The files from this release:**
  - `reblue.nro`: the game. The Switch shaders are built into it; nothing is
    compiled on the console.
  - `reblue.toml`: recommended settings (optional).

## Step 1: Prepare the game files on a PC

The Switch has no installer, so the PC version prepares the game files:

1. Install the PC version of re:Blue and let its installer read your three
   discs.
2. When it has finished, find its `game` folder. It holds `default.xex`,
   `bd_boot.ini`, `bd_config.ini`, the `pack`, `movie` and `map` folders and
   the sound files.

You don't need to play the PC version; it is only used to prepare the files.

## Step 2: Copy everything to the SD card

Put the files on the SD card in this layout. Create the folders that don't
exist yet.

```
sdmc:/switch/reblue/
    reblue.nro
    game/                   the whole game folder from the PC install
        default.xex
        pack/
        movie/
        map/
        ...
    profiles/default/
        reblue.toml         optional: the recommended settings from this release
```

- Copy the **whole contents** of the PC's `game` folder into `game/` on the
  card.
- Settings, saves, caches and logs are created in the same `reblue` folder the
  first time you play: `profiles/`, `cache/` and `logs/`.
- Saves are in `profiles/default/saves/`. Back that folder up before updating.

## Step 3: Start the game

1. Start the Homebrew Menu **through title takeover**: hold **R** while
   starting any installed game, then pick **re:Blue**.
2. **Don't start it from the Album.** The game needs the console's full
   application memory, which the Album doesn't give it. A forwarder works only
   if it uses the **39-bit address space**.
3. The first start takes longer: the game indexes its files and writes its
   caches. Mounting the game folder alone can take 10–20 seconds on a slow
   card.

## Settings

Most options are in the game's own **Settings** menu. The rest are in
`sdmc:/switch/reblue/profiles/default/reblue.toml`, created on the first start.
Each line is `name = value`; lines starting with `#` are comments, and anything
not listed keeps its default.

The `reblue.toml` in this release has the settings recommended for the Switch:

```toml
bd_scene_color_r11g11b10 = true   # lighter scene color format (if glows or transparency look wrong, set false)
input_xbox_layout = false         # face buttons by letter (Switch A = Xbox A)
bd_shadow_distance = 2.0          # shadows reach twice as far (less pop-in); 1.0 = the Switch default
bd_shadow_update_interval = 2     # shadow map redrawn every 2nd frame; 3 = faster, 1 = every frame
bd_shadow_min_pixels = 4.0        # skip shadows of objects smaller than 4 pixels on screen (0 = all)
bd_interp_min_fps = 36            # below this, no frame smoothing and a 30 fps cap
bd_opt_audio_hints = 0            # the game's Audio Hints: no spoken "Playable", "Checkpoint"...
```

What the speed-related options do:

| Option | Default | Effect |
|---|---|---|
| `bd_shadow_distance` | 1.0 | How far sun shadows reach. 2.0 removes most of the shadow pop-in and costs some speed in busy areas. |
| `bd_shadow_update_interval` | 2 | The sun shadow map is redrawn every Nth frame and reused in between. Shadows of moving characters can trail by up to N-1 frames. |
| `bd_shadow_min_pixels` | 4.0 | Objects that look smaller than this on screen (distant grass, bushes, posts) cast no shadow. Lower it if mid-sized shadows go missing. |
| `bd_interp_min_fps` | 36 | Above 30 fps the game smooths motion between its 30 Hz updates. When the console can't keep this rate, smoothing turns off and the game holds 30 fps. |
| `bd_opt_audio_hints` | 1 | The game's Audio Hints (the same option as in the Settings menu): spoken and on-screen notices such as "Playable", "Checkpoint" or an enemy spotting you. |

## Updating

Replace `reblue.nro` with the new one. Keep `game/`, `profiles/` (settings and
saves) and `cache/`. If a release says the cache changed, delete `cache/`; it is
rebuilt on the next start.

## Troubleshooting

| Problem | What to do |
|---|---|
| It closes right after starting, or says there is not enough memory | Start it through title takeover (hold **R** on a game), not from the Album. |
| It stops at start or never gets past a black screen | Check that `default.xex` is directly in `sdmc:/switch/reblue/game/`, not in a subfolder, and that the whole game folder was copied. |
| A voice says "Playable", "Checkpoint"... | That is the game's Audio Hints option: turn it off in Settings or set `bd_opt_audio_hints = 0`. |
| Shadows flicker or jump | Set `bd_shadow_update_interval = 1` and report it with your logs. |
| Glows or transparent effects look wrong | Set `bd_scene_color_r11g11b10 = false`. |
| Some objects lose their shadow | Lower `bd_shadow_min_pixels` (for example to 2) or set it to 0. |

## Reporting a problem

Please include:

- What you were doing, and where in the game.
- These files from the SD card, from the session where it happened:
  - the game log from `sdmc:/switch/reblue/logs/` (`reblue_001.log` or the newest one)
  - `sdmc:/switch/reblue/logs/rex/rex_stderr.log`
  - `sdmc:/switch/reblue/logs/rex/rex_crash.log`, if there is one
- A photo or a short video if it is a graphics problem.

For a performance report, create an empty file
`sdmc:/switch/reblue/logs/rex/perfil_pilas.flag` before starting the game and
include `logs/rex/rex_perfil.log` as well. The game runs a little slower while
that file exists, so delete it afterwards.
