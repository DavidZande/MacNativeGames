# Uncapped renderer benchmarks

Following the user's 2026-10-19 instruction, `games/totk/scripts/bench.py` now
measures uncapped rendering and has no FPS pass/fail targets. The default matrix
is `900p-unl`, `1080p-unl`, `1440p-unl`. Old `30`, `60` and `fine` configuration
tokens are rejected, and conflicting pacing overrides are rejected before launch.

The benchmark explicitly disables display-link pacing (`DM_PRESENT_LINK=0`),
software pacing (`DM_PRESENT_HZ=0`, without the separate 120 Hz uncapped clock),
and drawable presentation (`TOTK_PRESENT_SKIP=1`). This avoids measuring a
monitor or swapchain ceiling as if it were rendering capacity. After shader
warm-up, the window shows the current test phase and a still preview refreshed
between measurement windows. It does not show live gameplay in this mode.
Internal game screenshots and benchmark-window captures are saved with results;
their readbacks and UI updates run outside the timed windows.
Frame generation is disabled. Spatial MetalFX upscaling remains an optional
resolution comparison, e.g. `720p-mfx-unl`.

Normal gameplay and launcher defaults are unchanged. The game's existing
real-time timing patch remains active; its minimum time step is 1/120 second,
so scenes rendering faster than that can advance game animation faster than
wall time. Rendering throughput is not a gameplay-speed validation.

## Running

Rebuild from the source checkout with `games/totk/scripts/dev_rebuild.sh`, then
run the script in the external development tree:

```sh
cd ~/Documents/TearsOfKingdom/TOTK_build/totk
python3.14 scripts/bench.py --configs 900p-unl 720p-mfx-unl --seconds 30
```

Each run uses a separate copy of the player's profile, the existing game-slot
mechanism and a fresh output directory at
`~/Documents/TearsOfKingdom/bench/YYYYMMDD-HHMMSS/`. A player launch takes priority. Shared translation/driver caches
remain warm unless explicitly using a cold configuration. Do not run another
game or GPU probe concurrently.

## Measurements

- `frames.csv` stores frame IDs, retirement timestamps, individual frame times,
  GPU times and loading flags. `title-frames.csv`, `idle-frames.csv` and
  `pan-frames.csv` retain the exact samples used for those windows.
- `render_fps` is `1000 * sample_count / sum(frame_ms)`. The percentiles come
  from the full distribution of frames in each window, not averaged per-second
  percentiles. All hitches remain included; counts over 50/100 ms are descriptive
  measurements, not pass/fail conditions.
- The logger copies a bounded batch on the existing performance writer thread
  and writes files there. There is no per-frame file I/O on the guest path.
  An overflow explicitly invalidates the benchmark instead of silently losing
  slow frames.
- Sample timestamps use `CLOCK_MONOTONIC` when GPU timing is collected, about
  eight frames after rendering. Windows therefore select retired frames; settle
  time separates scene changes and screenshots from the stable windows. The
  loading window also includes the first few world seconds.
- The existing per-second counters provide CPU usage, memory, compilation and
  audio metrics. `TOTK_PERF_CSV` gives each new benchmark its own counter file;
  ordinary launches retain the existing user log location. These counters and
  per-frame samples have different collection intervals.
- `summary.json` records the actual launch environment and executable SHA-256.
  Report renderer FPS separately from visible display FPS; this mode does not
  measure presentation cadence, end-to-end latency or full visual correctness.

Native tests cover batched logging, frame IDs and durations above display-rate
throughput. Benchmark tests cover cap rejection, sample-window boundaries,
overflow, pooled percentiles and preservation of a slow-frame outlier.

## Quick cache experiments and GPU diagnostics

```sh
python3.14 scripts/bench.py --configs 900p-unl --seconds 10 --cache-ab both
python3.14 scripts/bench.py --configs 900p-unl --seconds 8 --cache-ab texture
python3.14 scripts/bench.py --configs 900p-unl --seconds 5 --gpu-passes 4 \
  --env TOTK_ARG_EPOCH=1 --env TOTK_ARG_EPOCH_CHECK=1 \
  --env TOTK_TEXTURE_PLAN=1 --env TOTK_TEXTURE_PLAN_CHECK=1
```

`--cache-ab` adds four standing windows, off/on/on/off, in one game session.
It starts both optional paths disabled, toggles only the selected paths, and
waits two seconds after each switch before measuring. Select `both`, `texture`,
or `arguments`. Reference checking is forced off for these speed windows.
`summary.json` records the selected flags, order and duration; the input file
records every runtime toggle. The trailing idle and pan phases use the final
disabled state. Short windows are exploratory checks, not evidence of a stable
gain across scenes; weather, game time and background load still vary.

`--gpu-passes N` requests N profiled frames after all speed windows. It writes
`gpu-passes.tsv` and leaves frame summaries in the log. Columns are frame, encoder
index, kind, milliseconds, draws, width, height, format, color count, depth format,
load count and first fragment-shader key prefix. Timestamp capture changes
submission segmentation and disables streamed render encoding in those frames,
so these are diagnostic rankings, not normal-path FPS measurements. Keep byte
reference checking and GPU profiling separate from speed comparisons.
