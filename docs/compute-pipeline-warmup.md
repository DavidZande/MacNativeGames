# Cached compute pipeline warm-up experiment (2026-10-19)

This experiment followed the earlier handoff's priority of smoothness after the
capped-mode improvement. The [consolidated current findings](practical-performance-20261019.md)
also cover later uncapped tests and cache experiments. The capped results below
are historical; current benchmarks have no FPS cap or pass/fail targets.
The startup seed already emits compute PSO rows; a second implementation of that
would add nothing. Two paths still lacked comprehensive pipeline warm-up:

- `prepare_extra()` ships 128 compute shaders in the 14,985-shader effect/UI set
  on this 1.0.0 build. Its background priming compiles libraries, not pipelines.
- The supplemental translation-cache pass loads libraries but does not create
  compute pipelines unless a previous play session recorded a matching C row.

`TOTK_WARM_COMPUTE=1` now reads current-chain reflection for cached shaders and
adds their compute pipelines to the normal warm-up. The ordinary C-row
deduplication, progress reporting and render-thread `WarmItem` claim remain in
use. Planning and execution share one snapshot of the input manifests. Dispatch
ordering, shader dependency matching and synchronous compute fallbacks are
unchanged. This does not translate undiscovered binding variants ahead of time.

## Local result

Two development-binary runs used the same isolated-profile benchmark route,
900p/60, three seconds settling and ten seconds per phase. These are smoke tests,
not the eight-window performance comparison described in performance-followups.
The system shader cache was retained. No claim of statistical improvement or
fully cold first-launch behavior follows from these runs.

| Measurement | Baseline | Compute warm-up enabled |
|---|---:|---:|
| Warmed compute pipelines | 39 | 220 |
| Background warm-up duration | 18.87 s | 21.86 s |
| First presented frame from launch | 5.08 s | 6.92 s |
| Standing FPS | 60.2 | 60.3 |
| Standing maximum frame | 33.1 ms | 33.0 ms |
| Panning FPS | 60.3 | 60.2 |
| Panning maximum frame | 22.2 ms | 29.2 ms |
| Loading-phase maximum frame | 183.2 ms | 183.2 ms |
| Panning peak footprint | 4,979 MiB | 4,974 MiB |

All 220 compute pipelines built successfully: 181 more than the baseline. No new
compute pipeline compilation occurred on either measured gameplay route, so the
extra coverage's hitch reduction was **not exercised**. The standing screenshots
show the same rendered scene; both raw render-target captures have existing
magenta padding outside the rendered viewport. This is not comprehensive visual
validation. No measurable gameplay improvement was established, and the added
startup cost is real. The option therefore stays **off by default**. The app was
not rebuilt for this initial trial; a later practical-pass rebuild included the
option disabled, along with the logging and benchmark-window fixes. See the
consolidated report for its hash and packaged-app validation.

Evidence root outside the source repository:
`~/Documents/TearsOfKingdom/bench/`.

- `20261019-133400/`: baseline log, screenshots, per-phase CSV and summary.
- `20261019-133637/`: enabled candidate, same outputs. This intermediate
  binary enabled the option by default; final source makes the same behavior
  explicitly opt-in. Its SHA-256 was
  `8dfd258908526ac7bc8c72e87b8cb56a8c27ada498866dad2e5530f70cc6f1a6`.
- `compute-dispatch-probe.log`: Metal-validation dispatch-chain probe.
- `compute-warm-final-build.log`: successful rebuild with the final
  opt-in default. Binary SHA-256:
  `1daf1d8ccc36ff4ef430b5d37c675a759e551ecafcbb766d12311631c174a70d`.

At the time of this trial, 40 source tests passed. The later suite has 45 passing
tests; run it from the repository root with the external build environment's
Python (`~/Documents/TearsOfKingdom/TOTK_build/venv/bin/python -m unittest discover -s tests -v`).
The native discovery test covers unplayed compute variants,
graphics exclusion, stale-chain reflection, malformed JSON, missing reflection
and missing source. `verify_gfx.py` and the production GPU dispatch-chain probe
also pass. The initial system-Python suite run lacked `cryptography`; rerunning
with the existing build environment passed all tests without installing anything.

## Next useful work

Find a reproducible route that first uses one of the 181 additional pipelines;
compare explicit `TOTK_WARM_COMPUTE=0/1` with per-dispatch compilation traces and
all slow frames retained. Measure reflection discovery and reduce its startup
cost before considering a default change. A previously unseen translation can
still stall because compute work cannot safely be skipped.

The baseline's roughly 33 ms standing frames and 183 ms loading frames occurred
without compute compilation, so this experiment cannot fix them. Their causes,
and the video memory-mapping/logging work in the handoff, remain separate targets.
These development runs also lacked a normalized `build/shader-seed` manifest
(the seed lived under `build/shader-seed/v0`); their logs reported no built-in startup
seed. Do not present these development launches as packaged-App first-launch
shader validation.
