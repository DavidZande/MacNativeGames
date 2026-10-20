# Next performance ideas — 2026-10-20

Brainstorm after the 2026-10-20 work (framebuffer fetch for decals, no in-place pipeline builds, RomFS cache).
Nothing here is implemented or measured yet unless it says so. Goal: shorter frame times, steadier frames, fewer
artifacts. Ideas are ranked by expected gain against effort.

## Where the time goes now

Steady state at 900p on the M1 Pro is **GPU-bound**: ~11–12 ms GPU per frame, ~85–90 FPS standing. CPU-side
improvements no longer raise the frame rate in normal play (the late fence already overlaps the replay).

Per-pass GPU capture, 4 frames, before framebuffer fetch (`bench/20261019-142046/900p-unl/gpu-passes.tsv`;
columns: frame, pass, type, ms, draws, width, height, format, colour attachments, …, first fragment shader). Profiling
changes submission behaviour, so treat the absolute numbers as indicative:

| Target size | ms / frame | passes / frame | ms / pass |
|---|---|---|---|
| 1600×900 (full res: G-buffer, decals, lighting, composite) | 2.54 | 13 | 0.196 |
| 800×450 (half res: AO, half-res lighting / effects) | 1.91 | 14 | 0.136 |
| compute | 0.90 | 10 | 0.090 |
| 400×225 | 0.90 | 14 | 0.064 |
| 100×56 | 0.78 | 12 | 0.065 |
| 800×800 (shadow cascades) | 0.56 | 4 | 0.140 |
| 200×112 | 0.46 | 7 | 0.065 |
| 32×32 | 0.42 | 2 | 0.210 |
| 80×45 | 0.33 | 4 | 0.082 |
| 1920×1080 (output canvas) | 0.33 | 2 | 0.164 |
| 256×256 | 0.29 | 4 | 0.073 |
| blit | 0.27 | 4 | 0.068 |

Two observations drive the ranking below:

1. **The profile is flat.** No single pass exceeds ~0.56 ms per frame (5 %). One heroic shader rewrite cannot win much.
2. **Small passes cost far more than their pixels.** ~43 passes per frame at 400×225 and below take ~3.2 ms in total.
   A 100×56 pass shades ~5,600 pixels yet takes ~65 µs: that is fixed per-pass cost (encoder start/end, tile
   load/store, barriers, the GPU waiting for the previous pass), not shading. The main G-buffer pass, by contrast,
   draws 1,356 times in ~0.41 ms.

---

## 1. Frame time: make the GPU do less work per frame

### 1a. Cut the fixed cost of the small post-processing passes ★ biggest likely win for its effort

The bloom / blur / luminance pyramids and other reduction chains are dozens of tiny render passes back to back.
Ideas, cheapest first:

- **Let independent small passes overlap.** Every render encoder currently starts after a full queue barrier
  (`barrierAfterQueueStages:MTLStageAll`, see `bar_start_on()` and the blit/compute equivalents), so the GPU drains
  completely between passes, and a 5,600-pixel pass pays the whole pipeline ramp-up and drain. A pass only needs to
  wait for the passes that wrote what it reads. Track, per pass, the textures it samples and the attachments earlier
  passes wrote (the replay already knows both: `draw_samples_rt`, attachment serials), and only barrier on a real
  dependency. Chains stay serial (each step reads the previous one), but independent branches (e.g. bloom and
  luminance, separate shadow cascades) can overlap.
- **Measure the bubble first.** The per-pass numbers above come from `TOTK_PASS_GPU`, which stamps a timestamp at
  each encoder boundary and reports the difference between consecutive stamps: a tiny pass's ~65 µs therefore
  includes the barrier drain and ramp-up around it, and the capture cannot separate them. The ceiling can be measured
  without new code: the existing measurement switches `TOTK_BARRIER_START=0` (no consumer barrier at encoder start)
  and `TOTK_BARRIER_END=0` (no producer barrier at encoder end) remove the barriers (the picture may be wrong; not for
  play). One A/B run with and without them bounds what dependency-based barriers could ever gain: little change →
  the cost is real per-pass work (fusion / merging route); a large jump → smarter barriers are the cheap, safe route.
- **Batch tiny passes into one encoder where the targets are tiny.** For targets ≤ 256×256 with a single colour
  attachment and no depth, replace the chain step's render pass with a compute dispatch writing the texture directly,
  so a whole chain runs in one compute encoder with lightweight intra-encoder barriers. Only for passes that are
  provably full-screen (the `uv_proof` machinery) and blending-free.
- **Measured ceiling (2026-10-20, same binary, 900p-unl, 10 s phases; `bench/20261020-181314` defaults vs
  `bench/20261020-182453` with `TOTK_BARRIER_START=0 TOTK_BARRIER_END=0`):**

  | phase | defaults: FPS / GPU ms | no start/end barriers: FPS / GPU ms | GPU saved |
  |---|---|---|---|
  | title | 114.5 / 8.68 | 157.9 / 6.35 | −2.33 ms (−27 %) |
  | standing | 95.9 / 10.28 (p95 11.4) | 101.5 / 9.27 (p95 15.2) | −1.01 ms (−10 %) |
  | panning | 95.2 / 10.19 | 111.3 / 8.87 | −1.32 ms (−13 %) |

  The barrier-free picture is wrong: the distant islands lose detail and look washed out (a haze / fog pass reading
  unfinished data), so some barriers guard real dependencies. Standing p95 / p99 got worse without barriers (frames
  became bimodal), so the standing mean understates the GPU saving. Reading: barriers cost ~1.0–1.3 ms of GPU time
  per frame in the world (10–13 %) and ~2.3 ms at the title. Dependency-based barriers can recover part of that, not
  all (chains keep theirs); the rest of the tiny-pass cost (~2 ms of the ~3.2 ms) is real per-pass work, the target
  of fusion.
- **Layered fusion of sibling passes** (suggested by DeepSeek). Reuse the default-on cube-face grouping
  (`nvn_cube_group.h`): copy each tiny target into one layer of an owned 2D array texture, replay all their draws in
  a single layered pass (per-draw layer constant, layered vertex variant from `vs_fn_for`), copy the layers back. No
  new shader generation. Limits: (1) only *independent* passes qualify: a chain step that samples the previous step
  cannot share a pass with it (that read-while-writing is the hazard behind the ground patches); (2) array layers
  share one size and format, so only same-size / same-format groups qualify, not pyramid steps of different sizes;
  (3) the copy in / out adds blit encoders with their own barriers. Candidates from the capture: the 12 passes per
  frame at exactly 100×56 (if they are siblings, not a ping-pong blur) and the 4 shadow cascades at 800×800 (same
  size and format, independent: close to the cube-group pattern). Choose between this and dependency-based
  barriers after the measurement: barrier gaps → barriers; fixed per-pass cost with sibling groups → fusion.
- **Skip work the game recomputes needlessly.** Some 32×32 / 80×45 passes are probably exposure / luminance reductions
  that converge slowly; check whether their results change frame to frame (hash the output) before considering
  caching. Lower priority: risk of visible lag in exposure.

### 1b. G-buffer + lighting in tile memory — ruled out by the frame structure (2026-10-20)

**Checked against the pass order of a captured frame (frame 14569): it does not fit.** G-buffer pass 25 (1,356
draws, 4 colour + depth, 0.41 ms) → compute, linear depth, decals (27, 29, 33), half-res passes → shadow cascades
36–39 (800×800) → half-res AO / lighting passes (69, 72: 8 attachments each), more decals (70) → full-res lighting 82
(`685322ae`, 0.45 ms). Lighting needs the shadow maps, which are rendered after the G-buffer, and a Metal render pass
cannot pause for other targets; several in-between passes also sample the G-buffer from memory (half-res AO,
compute). So the G-buffer must be stored regardless and lighting cannot join its pass. What remains: folding the early
decal passes 27 / 29 / 33 back into the G-buffer pass, worth ~0.06 ms per boundary (pass 27 reloads 5 attachments in
0.062 ms total), ~0.2 ms at best, with command reordering. Framebuffer fetch for the decals already took the part
that paid. Original idea, kept for reference:

Framebuffer fetch removed the decal copies (+11–16 %). The same idea scales: passes that read the G-buffer only at
their own pixel could run in the same render pass as the G-buffer, so the G-buffer (5 colour attachments at
1600×900 in this capture) would never be stored to and reloaded from memory, and could even be memoryless.

- The fork already has the machinery (readable-tile pass merge, `fuse_try` / `fz_p2_draw`, behind `DM_NVN_TBDR=1`),
  but it relies on offline `_bf` shader variants that TOTK's runtime-translated shaders do not have.
- `nvn_rt_fetch.h` (2026-10-20) now proves "samples only its own pixel" from the MSL text at run time and compiles
  framebuffer-fetch variants in the background. Feeding that into the pass-merge path would let the merge work for
  TOTK without offline variants.
- Expected gain: the full-resolution store + reload of every merged attachment (1600×900×5 attachments × 4–8 bytes,
  tens of MB per frame). Effort: high (pass merge state machine, correctness across deferred clears and draw
  ordering). Do after 1a.

### 1c. Hand-tuned replacements for the heaviest full-screen shaders — sized, skipped for now (2026-10-20)

Sizing the candidates: `685322ae` (lighting, 0.45 ms) is 68 lines with 1 texture read and `0ac9a549` (composite,
0.39 ms) 126 lines with 2: their cost is full-screen blending / bandwidth (12 full-screen draws into the HDR target),
not arithmetic, so there is nothing to hand-tune. The real candidates are the half-res AO / lighting shaders
`dbe0acd9` (1,676 lines, 23 texture reads, 8 outputs, 0.38 ms) and `debef6ad` (1,550 lines, 20 reads, 0.40 ms):
~0.8 ms together, a realistic saving of ~0.3–0.4 ms per frame (~3–4 % FPS). Most of the work would be a pixel-
comparison harness (captured frame inputs, original vs replacement). Delegating the drafting to an external model
would send game-derived shader code off the machine; David chose to skip the item for now. Original idea:

Translated Maxwell shaders compile with exact-math settings (needed for correctness in general). For the heaviest few
(e.g. `dbe0acd9` half-res, 0.55 ms; `685322ae` full-res, 0.44 ms; composite `0ac9a549`), a native Metal version keyed
by shader hash could use half precision and Apple-friendly sampling. The fork already does this once (quarter-res
bloom, `bloomq_*`). Each replacement is verifiable pixel by pixel against the original. Gain per shader small
(0.1–0.3 ms); worth it only for the top handful.

### 1d. Let the display adapt (perceived smoothness) — implemented (fe65ac3, `TOTK_VRR=1`), needs an on-screen check

Not faster, but smoother: at 80–95 FPS on a 120 Hz ProMotion screen, present through `CAMetalDisplayLink` with a
preferred frame-rate range so the display refreshes when a frame is ready (variable refresh) instead of judder
between 120 Hz slots. Combine optionally with dynamic resolution driven by measured GPU time and MetalFX temporal
upscaling (upstream PR #87).

---

## 2. Frame stability: predict instead of react

Remaining hitches are all first-use work: textures uploaded the first time they are drawn (post-spawn 150–200 ms
frames with ~290 new textures and 0 MB disk reads, submit-side graphics-lock stall), first-time pipelines, read-back
waits (~22 ms, of which ~17 ms waits for the frame just rendered).

### 2a. Per-area texture working sets (learned, like the pipeline list)

The game streams the same areas repeatedly. Record which textures get their first upload near each map position
(position from the existing world-survey hooks, texture identity by content hash / guest layout), store per profile,
and on loading screens and quiet frames pre-convert (deswizzle / decode) and upload them. Same idea as the PSO
manifest, applied to textures.

### 2b. Upload budget per frame, low mips first

**Diagnosis first (2026-10-20, `bench/20261020-184530`, `TOTK_STALL_DUMP=60 TOTK_WAITLOG=1`).** Post-spawn frame 17084:
259 ms, 366 first-time texture uploads (118 MB, only 10.3 ms CPU on the submit side), main thread waiting 238 ms on
the game's graphics lock. Stack dumps during the wait: the lock holder (render / submit thread 0x1004) sits in a host
`pthread_cond_wait` inside our layer, not computing. Counters for frames 16800–17100: the translation thread had 38
jobs queued (normally 0–2) and the submit side spent 80 ms in drain waits (`x_drain`). Mechanism: the burst of new
resources makes the translation thread fall behind; the render thread then waits for that backlog (drains, likely
a full job queue) while holding the graphics lock. The upload CPU cost itself is not the problem; a budget only helps
if it throttles the work that piles up on the translation thread. Next: profile the translation thread during the
burst (texture creation, residency registration, deswizzle, or something else), and get the caller of that
`pthread_cond_wait` (the stall dump prints only pc / lr).

Original idea:

Cap first-upload work per frame (e.g. ~2 ms), and upload the small mip levels first: the texture appears blurry for
a frame or two instead of the frame stalling. Virtual-texturing behaviour in the translation layer. Needs care for
textures the game reads back or renders into.

### 2c. Warm-up during loading screens and menus

While the game shows a loading screen or a pause menu, the GPU is mostly idle. Compile the pipelines most likely
needed next, predicted from which shaders followed which in earlier sessions (an adjacency list built from
`pso_user.tsv` order and the frame they first appeared).

### 2d. Read-backs one frame late where safe

~17 ms of each read-back invalidate waits for the frame just rendered. For read-back targets whose consumer tolerates
one frame of latency (to be identified per texture; e.g. auto-exposure statistics), answer from the previous frame's
copy, in the spirit of the late fence. Needs per-texture evidence; never for gameplay-critical reads.

---

## 3. Artifacts: make them report themselves

Both artifacts fixed so far (HUD letters, ground patches) were "data read at the wrong moment". More are likely.

### 3a. "Changed after submit" sanitizer ★ cheap, protects everything else — implemented (fe65ac3, `TOTK_SUBMIT_SANITIZER=1`)

First runs (`bench/20261020-185126`, `-185429`): in the world, ~70k uniform and ~48k storage ranges per 600
submissions change after submit, all covered by the snapshot (0 likely bugs); GPU-written ranges are classified as
expected. 32 small vertex ranges (64–512 B, rings of 4, one memory region) changed while the game was being closed
(likely quit-dialog / menu UI): fix candidate = snapshot small vertex buffers (≤ a few KiB) like the glyph buffer.
Cost when on: 5–7 FPS in the world (debug only). Original idea:

Debug mode: at the submit call, checksum every buffer range the submission binds (uniform, storage, vertex, index);
at replay time, checksum again before use. A mismatch logs shader, binding, size and the guest code that wrote it
(the dirty-page tracking already knows writers). It finds the next flicker bug without anyone spotting it in play.
Self-healing variant: a binding pattern caught changing gets the submit-time snapshot from then on, like the glyph
buffer.

### 3b. Frame capture and image diff

Capture one frame's command stream and the memory it references; replay it offline through two builds and compare
the output pixels. A changed region pinpoints a regression without a play session, and could run on every build.
Later: a reference render of the same capture through Ryujinx to say what the frame should look like.

### 3c. Known correctness fixes to adopt

- Upstream PR #91: vertex outputs a shader never writes start at (0, 0, 0, 1) (held-item drop marker, slightly wrong
  volumetric light). Our `fbf_vs_screen` proof must first accept the leading vector initialisation, or every
  framebuffer-fetch variant falls back to copies.
- The 11 shader pairs that keep feedback copies (`.gather` / offset reads): for those, a pass split (as in upstream
  PR #67) is cheaper than a copy and still correct.

---

## Suggested order

1. **1a measurement**: one bench A/B, `--env TOTK_BARRIER_START=0 --env TOTK_BARRIER_END=0` against defaults
   (ceiling of barrier relaxation), plus recording which textures each small pass samples (to find independent sibling
   groups for layered fusion). Tells which route pays before writing it.
2. **3a sanitizer**: cheap, finds artifacts, makes every later change safer.
3. **1a implementation** (dependency-based barriers), then **2b** (upload budget) for the post-spawn stalls.
4. **1b** (tile-memory G-buffer), the largest structural change.
