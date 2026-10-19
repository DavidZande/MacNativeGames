# Render-target feedback correction: 2026-10-19 continuation

## Current development status

The cached feedback correction has now been exercised in-game. The same roof is clean, with ordinary cast shadows and UI text retained. Evidence: `bench/shadow-20261019-214120-feedback/900p-unl/` outside the source checkout. The timed offscreen phases measured 76.67 FPS standing and 72.91 FPS panning; these are short diagnostics, not a paired release benchmark or proof about walking hitches.

The implementation now materializes deferred clears before copying, protects overrides across recursive draws, checks overlapping guest ranges plus actual Metal parent identity, and retires unused scratch descriptors above a 64 MiB idle budget. Visibility boundaries invalidate cached snapshots. The new production-path GPU probe covers two MRT inputs, sRGB/unorm data, deferred clears, aliases with different guest starts, false aliases, cached reuse, barriers, queued submissions, and argument epochs. It passed 3,085 checks under Metal validation; explicitly disabling the correction fails the pixel control as intended.

The source now enables feedback snapshots by default; `TOTK_RT_FEEDBACK_SNAPSHOT=0` remains the diagnostic control. The default-on build passed all ten GPU regression probes and 45 Python tests. App installation is pending; the installed app has not changed.

Readback prefetch is a separate experiment, default off (`TOTK_WB_PREFETCH=1`). It learns CPU-read texture layouts, snapshots up to 32 MiB at producer boundaries, and waits for producer events instead of unrelated later rendering. Its GPU probe passed 70,998 checks, including a deliberately held later submission, all mip levels, invalidation after GPU writes and preservation of newer CPU writes. Final validation includes fresh texture serials with the learned layout.

Walking evidence: `bench/shadow-20261019-215409-walk` and `bench/shadow-20261019-215809-walk-old`. Both first two phases are actual gameplay. Later phases of the first run reached game-over and must not be counted as walking. Scenes diverged during combat, so these runs cannot establish a clean A/B speedup. Remaining isolated CPU-side long frames are still under investigation.

## Paused at the user’s request: final handoff

No game process remains running. No app installation, signing, commit or push was performed. Preserve all working-tree changes, including the earlier glyph fix.

- Final built candidate: `TOTK_build/totk/build/totk-native`, build log `bench/shadow-final-build.log`. SHA-256: `6f5e0eb05b112d6beb30f43168a6dd67ecbffa29bfc4e05296a7abb2dbacc213`.
- `bench/release-probe-results.json`: all ten probes pass with Metal validation: rt-feedback (3,085 checks), readback-prefetch (71,006 checks, including 2-texture/shared-page regression), ui-snapshot, tex-slots, storage-view, ubo-snapshot, arg-epoch, dispatch-chain, linear-rt, texture-retire. Both negative controls fail deterministically (`TOTK_RT_FEEDBACK_SNAPSHOT=0` fails pixel check; `TOTK_WB_PREFETCH=0` fails completion wait). All 45 project Python unit tests pass with `TOTK_build/venv/bin/python`. Native tests `verify_gfx.py`, `probe-thread`, `probe-dirty-race`, `probe-texels-rect`, and `test-opus-codec` all pass.
- Invalidate hitch investigation & resolution: real terrain readbacks during walking were previously stalled 20-30 ms because `mtl_pool_invalidate` waited indiscriminately for all in-flight CPU texture uploads (`while (atomic_load(&g_up_pending)) usleep(200)`), even when uploads targeted completely unrelated textures. Implemented `g_up_inflight` (counted set of in-flight upload root textures); invalidations now wait only if the textures being read back are active upload destinations (`TOTK_WB_UPWAIT=all` retains the fallback).
- Walk acceptance benchmark `bench/shadow-20261019-221609-walk/900p-unl/`:
  - `walk-1`: 72.86 FPS, p99 19.37 ms, max 25.50 ms, 1 frame >= 25 ms, 0 >= 33 ms.
  - `walk-2`: 69.29 FPS, p99 17.64 ms, max 24.78 ms, 0 frames >= 25 ms, 0 >= 33 ms.
  - In-world invalidations > 20 ms eliminated; 92 readback prefetch snapshots consumed.
- Diagnostic logging: added `[readback-miss]` with reason tagging (1 = freshly rendered / no transition, 2 = same target next pass, 3 = unsupported target/mip/sample/size, 5 = format unpack error, 6 = snapshot byte budget, 7 = missing metal texture, 11-16 = GPU write invalidation sites).
- Preserved all working-tree changes cleanly. App packaging and signing ready for promotion when approved.
- Scripts outside repo: `bench/walk_acceptance.py` (now two phases), `bench/analyze_walk.py`, `bench/run-feedback-regressions.py`, `bench/shadow-final-pipeline.py`. Do not run GPU probes or compile during gameplay timing. Current source checkout is `MacNativeGames`, branch `mac-local-improvements`. The artifact volume named in AGENTS.md is absent; this work reused the existing external `TOTK_build` and `bench` directories.

## Historical handoff (before this continuation)

# Moving black patches on stone: investigation handoff

Status saved 2026-10-19 late evening at the user's request to conserve usage.
This is a status record, not an instruction to resume work automatically.

## Current outcome

- The user confirmed the UI/text flicker fix works in the installed app.
- The stone/roof patches have existed since the original fork. We reproduced the exact roof from the screenshots using an isolated copy of the user's save.
- **Cause identified: a material/decal pass samples the same Metal color textures that it is currently rendering into.** Copying those inputs before the draw visibly removes the roof patches and preserves ordinary cast shadows and text.
- The first working reusable-copy prototype is too slow (roughly 29 FPS during capture, versus earlier roughly 90 FPS normal runs; not a paired timed comparison). A caching optimization has been built but **has never been run**.
- The shadow prototype is opt-in, uncommitted, and **not installed in the .app**. Do not describe the shadow issue as shipped or fully fixed.

## Installed app and development build

Workspace: `~/Documents/TearsOfKingdom`; source: `MacNativeGames/`, branch `mac-local-improvements`, HEAD `da07530`.

Installed `Tears of the Kingdom.app` still contains the verified glyph storage snapshot fix only. Runtime SHA-256:

```
5546778015ac0d00fc09655d971bf945842958bc7f94d9559fc8f1a5d06d8201
```

Previous app: `TOTK_build/ui-glyph-release/previous.app`. See [UI fix record](ui-flicker-20261019.md) for its successful GPU probes, negative control, 45 Python tests, gameplay capture and signing checks. Keep this working text fix intact.

Latest development binary: `TOTK_build/totk/build/totk-native`, successfully built with the **untested feedback cache optimization**. SHA-256:

```
829568cdd88d0bb0b7267411526d754cd65f65a47699205a74b1de64a552a6cf
```

Build log: `bench/feedback-cache-build.log`. No game process was left running. No app installation or release validation was performed for this candidate.

## Direct evidence of framebuffer feedback

`bench/shadow-20261019-210349-trace/900p-unl/totk.log`, frame 13755, pass 865043:

- Fragment shader `e06b8a496f933970e452d66c2353e62d202b8ec4e79079f14d809d872e5d92d6-58f27a9b8868_b`.
- Vertex shader `ae952862ffc5985611da986e405f340a08e6f19048c4fb386b12bdec8f9b2dc5-b7530f698e76_b`.
- Color attachment 1: serial 172989, format 71 (RGBA8Unorm_sRGB), 1600×900, guest memory `0x1348faa00`. Fragment texture register 0 / texture ID 28269 reads **the same serial and memory**.
- Color attachment 3: serial 172990, format 70 (RGBA8Unorm), 1600×900, guest memory `0x134f3ac00`. Fragment texture register 1 / texture ID 28270 reads **the same serial and memory**.
- Draw writes outputs 1 and 3, depth test enabled, depth writes disabled (`dt=1,dw=0,df=4`). Related subsequent decal shaders include `95ac…`, `f88f…`, `449d…`, `700a…`.

Ordinary sampling from an active attachment on a tiled GPU does not provide a stable pre-draw image. The existing same-target guard only splits render encoders; it does not remove the simultaneous sampled/rendered texture alias. NVN `OP_BARRIER` otherwise remains a no-op. The evidence and corrected capture identify material/decal feedback rather than a conventional shadow-map bias problem.

## Evidence folders

All paths below are relative to the workspace, outside the source checkout. Capture timing and per-draw diagnostic runs are intrusive; do not use them as release FPS measurements.

| Evidence | Location and result |
|---|---|
| Baseline | `bench/shadow-20261019-204656-default/900p-unl/view-0-0.png`: exact roof, jagged black patches. Five camera positions, 12 consecutive frames each; `roof-contact.png`. |
| Fence lag off | `bench/shadow-20261019-204849-lagoff/900p-unl`: artifact remains with `TOTK_FENCE_LAG=0`. |
| Shadow bias | `bench/shadow-20261019-205052-bias/900p-unl/bias-contact.png`: D16 bias scale 0/2/4 and units 2/8/32/−8 do not fix it. Four-layer 800×800 shadow map; original polygon offset is zero. |
| Shadow-pass isolation | `bench/shadow-20261019-205339-isolate/900p-unl`: skipping `3f9bbff41b330ed4` removes Link's ground shadow but leaves roof patches. Albedo `buffer-10534200.png` already contains the patches before final lighting; packed surface/normal `buffer-10534300.png` is intact. `grid-roof.png` marks actual dark pixels near (150,410), (250,410), (300,420). |
| Main roof material | `bench/shadow-20261019-205724-material/900p-unl`: main FS `235e85b57ab565a6b459d15cfedc22a171cb19ba728af7978305d6618543c7ec-b7530f698e76_b`; skipping it removes the roof. `texture-contact.png` and `ao-contact.png`: sampled textures and mips are valid. |
| Shader isolation | `bench/shadow-20261019-210349-trace/900p-unl/trace-contact.png`: patches survive constant-color output from the main roof material. Individual blend textures, weights, UV and LOD0 were checked. `geo/` contains buffers/uniforms. Log contains the feedback proof above. |
| Material depth bias | `bench/shadow-20261019-210711-ztest/900p-unl/z-contact.png`: 0/−1/−2/−4/−16/+16/−64 do not fix it. |
| Baked AO | `bench/shadow-20261019-211155-aotest/900p-unl`: forcing AO to 1 or AO LOD0 does not fix it. |
| Per-draw pixel trace | `bench/shadow-20261019-211727-pixel/900p-unl`: frame 8857; base material writes valid stone, later e06b decal changes color. One selected point (300,405) was sky, so this trace alone is not proof of every black pixel's history. |
| Rejected allocation approach | `bench/shadow-20261019-212445-feedback/900p-unl`: new texture per feedback draw hit the existing 5000 MB GPU guard (5028 MB), before world entry. Guard was not changed. |
| **Working visual prototype** | **`bench/shadow-20261019-212823-feedback/900p-unl/view-0-0.png`**: roof clean, cast shadows and text retained. Five positions × 12 frames. Reusable scratch pool, copy per feedback draw; slow. This predates the latest cache optimization. |

An independent Metal sparse-output test (`bench/sparse-output.m` and binary) showed unwritten MRTs are preserved with either write mask; the output-mask hypothesis was rejected and removed.

## Current source implementation

Only the extra shadow work in `games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m` is new beyond the UI fix. All earlier temporary shader, bias and pixel diagnostics were removed from production source. An external historical copy remains at `bench/shadow-experimental.m`.

`TOTK_RT_FEEDBACK_SNAPSHOT=1` enables the prototype; it defaults **off**. It uses reflected vertex/fragment texture bindings, without shader-specific exceptions:

1. `feedback_prepare()` runs after `prepare_draw_textures()` / material fixups. It finds sampled textures sharing guest memory with color attachments, then verifies their actual topmost Metal `parentTexture` identity. Distinct Metal storage sharing guest addresses is not counted as feedback. Multisampled sources are skipped.
2. It ends the render encoder, obtains a matching private ShaderRead scratch texture from a retained pool, and copies all mip levels/slices of the sampled view. Matching includes texture type, pixel format, dimensions, array length and mip count.
3. `tex_fast()` / `tex_fast_rid()` use per-draw overrides, with argument bindings invalidated. `feedback_reset()` clears overrides after the draw so they do not leak into compute.
4. The **latest untested optimization** retains source-view-to-copy mappings across draws until a visibility boundary. A pool texture reserved by another live cache entry cannot be reused. Boundaries clear the cache at submission start and before `OP_BARRIER`, `OP_SET_RT`, color/depth/texture clears, dispatch and buffer/texture copy commands. The intent is one copy per relevant interval instead of every decal draw.

Relevant symbols: `feedback_cache_clear`, `feedback_texture`, `feedback_prepare`, `feedback_reset`, `g_fb_pool`, `g_fb_source`, `g_fb_saved`. Mapping limits are 128. First five feedback events log counts, but title/loading shaders consume these logs, so they are insufficient world statistics.

## Remaining work before promotion

1. Run the latest cached candidate on the same copied-save roof with the opt-in flag. Confirm clean roof, stable camera movement and correct shadows/text; measure short standing/panning phases separately from screenshots. The cache build itself has never been run.
2. Review NVN barrier/visibility semantics, overlapping decals, intervening writes, subresources, alias/view changes, submission ordering and argument cache invalidation. Check reentrant/deferred/fused/layer-merged draw paths; per-draw global override arrays may need protection if a draw recursively enters another draw.
3. Handle deferred attachment clears before copying: if a source has pending `R.clear_c` with no open encoder, simply calling `rp_end` can copy old contents. Materialize the clear first.
4. Bound/retire scratch textures across descriptor changes and long sessions. Current pool retains every descriptor encountered. Current cheap guest-address equality test can miss overlapping aliases with different start addresses; only the exact-memory case is verified.
5. Add a deterministic GPU probe using production feedback preparation and argument encoding: capture known MRT values, overwrite originals before draw, and verify the draw reads copies. Cover normal/albedo MRTs, alias views, false guest-memory aliases backed by distinct Metal objects, pool reuse with queued submissions, barrier invalidation, and argument epochs. A disabled-fix negative control should fail deterministically, without relying on undefined simultaneous read/write behavior.
6. Run existing glyph/storage snapshots, texture-slot/storage-view, uniform/argument and CPU graphics probes plus the 45 Python tests. No shadow-specific regression suite has been run yet.
7. Only after correctness and acceptable performance: consider default-on, build, gameplay acceptance, app installation/signing and matching SHA verification. None of those shadow-release steps has happened.

`bench/shadow_diagnostic.py` runs copied-profile captures and has a `feedback` mode that enables the opt-in flag. Historical `trace`, `pixel`, bias and shader modes require instrumentation no longer in production source; do not assume those modes still work. Rebuild via `MacNativeGames/games/totk/scripts/dev_rebuild.sh`. Preserve source changes and keep generated evidence/build outputs outside `MacNativeGames/`.

Source changes remain uncommitted, including the earlier text fix in Makefile, runtime probes/loader, NVN headers/recording and storage snapshot probe. `git diff --check` passed at the last check. The installed app remains the known-good text-fix build while shadow work is unfinished.
