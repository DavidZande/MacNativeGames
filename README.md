# MacNativeGames — performance fork

> [!IMPORTANT]
> Unofficial fan project, not affiliated with or endorsed by Nintendo. No game content, firmware or keys are included.

A fork of [ly05010419/MacNativeGames](https://github.com/ly05010419/MacNativeGames), which runs *Tears of the Kingdom*
natively on Apple silicon Macs. This fork adds rendering and stutter fixes. All credit for the port goes to upstream.

**Tested on 1.0.0 only** (MacBook Pro M1 Pro, 16 GB, macOS 27.2, 900p). Every change can be turned off with the
environment switch listed.

## What this fork changes

| Change | Result | Off switch |
|---|---|---|
| Frame fence answered one frame late, so the Metal replay overlaps the next game frame | busy area at the 60 FPS cap: 54 → 60 FPS | `TOTK_FENCE_LAG=0` |
| Tile-based GPU pass optimizations | +9–12 % FPS | `DM_NVN_TBDR=0` |
| Decals read the G-buffer through framebuffer fetch | moving patches on stone fixed, +11–16 % FPS | `TOTK_RT_FEEDBACK_FETCH=0` |
| HUD glyph data snapshotted at submit | flickering letters fixed | `TOTK_STORAGE_SNAPSHOT=0` |
| Partial texture copies, batched GPU write-back | frames over 50 ms: 28 → ~16 per minute | `TOTK_TEXELS_RECT=0`, `TOTK_WB_BATCH=0` |
| No pipeline builds on the render thread | one 8.7 s freeze at world entry gone | `TOTK_PSO_OMIT_SYNC=1` |
| Shaders precompiled to `.metallib` and loaded from disk | after a shader cache clear: startup wait 74 → 17 s | `TOTK_RT_METALLIB=0` |
| RomFS read cache | world-entry read time −30–35 % | `TOTK_ROMFS_CACHE=0` |
| GPU results the game reads back wait for the compute work that writes them | objects no longer vanish while the camera moves; up to ~10 % FPS in busy areas | `TOTK_LAG_READBACK=0` |

Also: a settings window (Cmd-,) with live diagnostic switches, rolling save backups, screenshots (F12), and a stutter recorder (F9) that writes
frame timings to `~/Library/Logs/MacNativeGames/flight/`. The commit-by-commit breakdown for upstream is in
[UPSTREAMING.md](UPSTREAMING.md). The investigation notes are kept at the
[`notes-2026-10-21`](https://github.com/DavidZande/MacNativeGames/tree/notes-2026-10-21/docs) tag.

Known issue: the first frames after spawning can still take ~150–200 ms.

## Requirements

- Apple silicon Mac, macOS 26 or later, 16 GB memory recommended, ~25 GB free disk space.
- Python 3.14.
- The game, base version 1.0.0, as XCI or NSP, and a keys file. Optionally the 1.1.2 or 1.4.3 update.
- For an NSP: the system fonts (`--firmware` or `--shared-fonts`).

## Build

```sh
python3.14 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys
python3.14 build_app.py ~/Games/totk.nsp -k ~/keys/prod.keys --firmware ~/Games/firmware
python3.14 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys --update ~/Games/1.4.3.nsp
```

The app appears next to the game file. Every step is checked against known hashes, and a mismatch stops the build.
`python3.14 build_app.py --help` lists all options.

For faster startup, install Apple's Metal Toolchain once: `xcodebuild -downloadComponent MetalToolchain`.

## Controls

Game controllers recognized by macOS work. Keyboard (US layout positions):

| Key | Switch | Key | Switch |
|---|---|---|---|
| W A S D | left stick | arrow keys | right stick |
| J | A | K, Space | B |
| I | X | U | Y |
| Q / E | L / R | Option / Shift | ZL / ZR |
| F T H G | d-pad | Z / C | stick press |
| Return | + | Tab, Esc | − |

**1** overlays, **2** frame interpolation, **3** picture mode (MetalFX / native 2K).

## Files

- Saves: `~/Library/Application Support/MacNativeGames/TOTK/profile`
- Shader cache: `~/Library/Application Support/MacNativeGames/TOTK/shader-cache`
- Log: `~/Library/Logs/MacNativeGames/totk.log`

How the port works: [docs/how-it-works.md](docs/how-it-works.md).

## Credits and license

Built on [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile), [nxnative](https://github.com/seedds/nxnative),
Ryujinx, [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross), deko3d, bcdec, libopus and libvpx.

This project's code is MIT ([LICENSE](LICENSE)); third-party code keeps its own license
([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). Legal notice: [LEGAL.md](LEGAL.md).

---

## 中文

非官方爱好项目，与任天堂无关。本仓库不含游戏内容、固件或密钥。

这是 [ly05010419/MacNativeGames](https://github.com/ly05010419/MacNativeGames) 的性能分支，为 Apple 芯片 Mac 上的《王国之泪》加入渲染与卡顿修复（见上表），每项都可用环境变量关闭。仅在 1.0.0、M1 Pro 上测试。构建方法同上：

```sh
python3.14 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys
```
