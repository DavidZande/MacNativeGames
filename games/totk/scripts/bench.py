#!/usr/bin/env python3
"""Benchmark uncapped renderer throughput at the title screen and in the world.

Each run starts the game binary from the build's work folder (TOTK_build/totk, the same files the app bundle holds) with
the environment the app's launcher sets, never the app bundle, open(1) or LaunchServices: a rebuilt 16 GB bundle gets a
new signature, and macOS then scans all of it once with its "Verifying..." dialog. --app runs the packaged app's
launcher executable instead. Every run uses an isolated copy of the player's profile, so the player's saves are never written. The run holds the game slot as an
agent (TOTK_SLOT_OWNER=codex): a game the player starts takes over and stops the benchmark. Phases:

  title  N s at the title screen, no input
  idle   after one A press (Continue) and the loading screen: N s standing still
  pan    N s of a slow camera pan (right stick to the right, held in steps)

Results (perf.csv rows per phase, screenshots, a summary) go to ~/Documents/TearsOfKingdom/bench/<time>/.

No display pacing, software FPS ceiling or FPS pass/fail targets are used.
The window shows phase updates and still previews between timed measurements.
Per-frame samples supply pooled frame-time percentiles, including all hitches.

  bench.py                          # 900p / 1080p / 1440p, uncapped
  bench.py --configs 900p-unl 720p-mfx-unl --seconds 20
  bench.py --title-only             # no input at all
"""
import argparse
import csv
import hashlib
import json
import math
import os
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

HOME = Path.home()
APP = Path(os.environ.get('TOTK_APP', HOME / 'Documents/TearsOfKingdom/Tears of the Kingdom.app'))
TREE = Path(os.environ.get('TOTK_TREE', HOME / 'Documents/TearsOfKingdom/TOTK_build/totk'))   # build_app.py's work tree
CHAIN = 'totk-native-v4-unverified'   # dmsh_chain.h: the shader cache folder name
DATA = HOME / 'Library/Application Support/MacNativeGames/TOTK'
LOG = HOME / 'Library/Logs/MacNativeGames/totk.log'
PERF = HOME / 'Library/Logs/MacNativeGames/perf.csv'
OUT = HOME / 'Documents/TearsOfKingdom/bench'
KEY_A, KEY_RIGHT_STICK_RIGHT = 38, 124   # J = A, right arrow = right stick right (runtime/input.c)

RES = {'540p': '960x540', '720p': '1280x720', '900p': '0', '1080p': '1920x1080', '1440p': '2560x1440'}   # 0: the game's own (dynamic, up to 900p)
UNCAPPED_ENV = dict(DM_PRESENT_HZ='0', DM_PRESENT_LINK='0', TOTK_PRESENT_SKIP='1',
                    TOTK_UNCAPPED='0', TOTK_INTERVAL_SCALE='1', TOTK_REALTIME='1', TOTK_FPS60='1', TOTK_MFX='0')
CACHE_FLAGS = {'both': ['arg_epoch', 'texture_plan'], 'arguments': ['arg_epoch'], 'texture': ['texture_plan']}


def config_env(name):
    """Resolution / diagnostic choices; every configuration measures uncapped rendering."""
    parts = name.split('-')
    env = {'TOTK_SETTINGS': 'off', 'TOTK_PAUSE_HIDDEN': '0', 'TOTK_FULLSCREEN': '0', 'TOTK_OPERATION_MODE': 'docked', 'TOTK_RENDER_RES': '0', 'TOTK_MFX_UPSCALE': '0', **UNCAPPED_ENV}
    for p in parts:
        if p in RES:
            env['TOTK_RENDER_RES'] = RES[p]
            if p == '1440p': env['TOTK_OUTPUT_RES'] = '2560x1440'   # the native 2K canvas, so the 1440p picture is not scaled down to 1080p
        elif p == 'handheld': env['TOTK_OPERATION_MODE'] = 'handheld'
        elif p == 'mfx': env['TOTK_MFX_UPSCALE'] = '1'
        elif p == 'unl': pass
        elif p in ('30', '60', 'fine'): raise SystemExit('capped benchmark configs were removed; use e.g. 900p-unl')
        elif p == 'async': env['TOTK_ASYNC_PSO'] = '1'
        elif p == 'sync': env.update(TOTK_ASYNC_PSO='0', TOTK_ASYNC_XLATE='0', TOTK_ASYNC_SHLOAD='0')
        elif p == 'noshload': env['TOTK_ASYNC_SHLOAD'] = '0'   # libraries loaded on the render thread (before this change)
        elif p == 'salt': env['TOTK_MSL_SALT'] = f'bench {name} {time.time()}'   # every Metal compile new to the system cache
        elif p == 'norecycle': env['TOTK_PSO_RECYCLE'] = '0'   # background pipeline table not recycled when full (before)
        elif p == 'synclog': env['TOTK_SYNC_LOG'] = '1'   # GPU-fence waits: calls, blocked, time
        elif p == 'freecores': env['TOTK_FREE_CORES'] = '1'   # no Switch core arbitration between guest threads
        elif p == 'replay': env['TOTK_ASYNC_REPLAY'] = '1'   # submit replay and Metal encoding on the translation thread
        elif p == 'lag': env.update(TOTK_ASYNC_REPLAY='1', TOTK_FENCE_LAG='1')   # translation thread + the frame fence answered one frame late (experiment)
        elif p == 'check': env.update(TOTK_POOL_QUERY_CHECK='1', TOTK_USNAP_INDEX_CHECK='1', TOTK_SHADOW_INDEX_CHECK='1', TOTK_TEX_SLOTS_CHECK='1', TOTK_DRV_CACHE_CHECK='1')   # replay caches compare every hit with the full lookup (stops on a mismatch)
        elif p == 'noprime': env['TOTK_XLATE_PRIME'] = '0'   # translated libraries compiled on the render thread (before this fix)
        elif p in ('qosutil', 'qosdef', 'qosinit'): env['TOTK_COMPILE_QOS'] = {'qosutil': 'utility', 'qosdef': 'default', 'qosinit': 'initiated'}[p]
        elif p == 'xcold': env.update(TOTK_SHADER_SEED='off', DM_PSO_MANIFEST='off', TOTK_WARM_LIGHT='0')   # empty translation cache (see run)
        elif p == 'fs': env['TOTK_FULLSCREEN'] = '1'   # full screen (macOS Game Mode)
        elif p == 'rawaudio': env['TOTK_AUDIO_SMOOTH'] = '0'
        elif p.startswith('lat') and p[3:].isdigit(): env['DM_PRESENT_LATENCY'] = p[3:]   # CAMetalDisplayLink preferredFrameLatency
        elif p == 'cold': env.update(DM_PSO_MANIFEST='off', TOTK_WARM_LIGHT='0')   # no warm-up; with a fresh binary copy (cold Metal cache, see run)
        else: raise SystemExit(f'unknown config part {p!r} in {name!r}')
    return env


GAME_PROC = None   # the game this run started from the work tree (its own handle: pgrep -f can miss a process for seconds)


def game_running():
    if GAME_PROC is not None: return GAME_PROC.poll() is None
    # pgrep -f misses a process whose arguments cannot be read for a moment (seen in state U while it starts): ask 3 times
    for i in range(3):
        if subprocess.run(['pgrep', '-f', 'build/totk-native(-cold-[0-9]+)? rewritten'], capture_output=True).returncode == 0: return True
        if i < 2: time.sleep(0.5)
    return False


def send(cmd_file, line):
    with open(cmd_file, 'a') as f:
        f.write(line + '\n')


def perf_rows_since(offset):
    """perf.csv rows written after byte offset (header lines skipped)."""
    if not PERF.exists(): return []   # a new run may still be starting its launcher
    with open(PERF) as f:
        f.seek(offset)
        text = f.read()
    rows, header = [], None
    for line in text.splitlines():
        if line.startswith('#'): continue
        if line.startswith('time,'): header = line.split(','); continue
        if header: rows.append(dict(zip(header, line.split(','))))
    return rows


def wait_for(pred, timeout, step=1.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if pred(): return True
        if not game_running(): raise RuntimeError(f'the game exited (or was taken over by a player launch; exit status {GAME_PROC.returncode if GAME_PROC else "?"})')
        time.sleep(step)
    return False


def sample_rows(path, start_ns, end_ns):
    if not path.exists(): return []
    text = path.read_text(); lines = text.splitlines()
    if text and not text.endswith('\n'): lines = lines[:-1]   # writer may be flushing its final line
    if any(line.startswith('# overflow=') for line in lines):
        raise RuntimeError('frame sample buffer overflowed; this run is incomplete')
    return [r for r in csv.DictReader(lines) if start_ns <= int(r['retired_ns']) < end_ns]


def summarize(rows, samples=None):
    if not rows: return {}
    f = lambda k: [float(r[k]) for r in rows if r.get(k) not in (None, '', '-1', '-1.00')]
    fps, p50, p95, p99, mx, gpu, cpu = f('fps'), f('frame_p50_ms'), f('frame_p95_ms'), f('frame_p99_ms'), f('frame_max_ms'), f('gpu_avg_ms'), f('cpu_pct')
    result = {'seconds': len(rows), 'render_fps': round(statistics.mean(fps), 1) if fps else None,
            'p50_ms': round(statistics.median(p50), 2) if p50 else None, 'p95_ms': round(statistics.median(p95), 2) if p95 else None,
            'p99_ms': round(max(p99), 2) if p99 else None, 'max_ms': round(max(mx), 1) if mx else None,
            'gpu_ms': round(statistics.mean(gpu), 2) if gpu else None, 'cpu_pct': round(statistics.mean(cpu)) if cpu else None,
            'long50': int(sum(f('long50'))), 'long100': int(sum(f('long100'))),
            'pipelines': int(sum(f('new_pipelines'))), 'libraries': int(sum(f('new_libraries'))), 'build_ms': round(sum(f('build_ms'))),
            'async_skipped': int(sum(f('async_skipped'))),
            'inplace_pso': int(sum(f('inplace_pso'))), 'inplace_merged': int(sum(f('inplace_merged'))), 'inplace_pso_ms': round(sum(f('inplace_pso_ms'))),
            'inplace_pso_max_ms': round(max(f('inplace_pso_max_ms')), 1) if f('inplace_pso_max_ms') else None,
            'pres_p50': round(statistics.median(f('present_p50_ms')), 2) if f('present_p50_ms') else None,
            'pres_p95': round(statistics.median(f('present_p95_ms')), 2) if f('present_p95_ms') else None,
            'pres_p99': round(max(f('present_p99_ms')), 2) if f('present_p99_ms') else None,
            'pres_max': round(max(f('present_max_ms')), 1) if f('present_max_ms') else None,
            'dropped': int(sum(f('present_dropped'))),
            'audio_underruns': int(sum(f('audio_underruns'))),
            'xlate_block_ms': round(sum(f('xlate_block_ms'))), 'xlate_max_pending': int(max(f('xlate_pending'))) if f('xlate_pending') else 0,
            'footprint_mb': round(max(f('footprint_mb'))) if f('footprint_mb') else None}
    if samples is not None:
        if not samples: raise RuntimeError('no per-frame samples in the measurement window')
        frames = sorted(float(r['frame_ms']) for r in samples)
        if any(not math.isfinite(t) or t <= 0 for t in frames): raise RuntimeError('invalid per-frame duration')
        quantile = lambda p: frames[min(int(p * len(frames)), len(frames) - 1)]
        gpu_samples = [float(r['gpu_ms']) for r in samples if float(r['gpu_ms']) >= 0]
        result.update(render_fps=round(1000 * len(frames) / sum(frames), 2), sample_count=len(frames),
                      p50_ms=round(quantile(.5), 3), p95_ms=round(quantile(.95), 3),
                      p99_ms=round(quantile(.99), 3), max_ms=round(max(frames), 3),
                      long50=sum(t > 50 for t in frames), long100=sum(t > 100 for t in frames),
                      gpu_ms=round(statistics.mean(gpu_samples), 3) if gpu_samples else None)
    return result


def extra_env(values):
    result = {}
    for kv in values:
        key, sep, value = kv.partition('=')
        if not key or not sep: raise SystemExit('--env requires NAME=VALUE')
        if key in UNCAPPED_ENV and value != UNCAPPED_ENV[key]:
            raise SystemExit(f'{key}={value} conflicts with uncapped renderer benchmarks')
        result[key] = value
    return result


def run(name, args, outdir):
    global PERF, GAME_PROC
    if game_running():
        if not args.quit_running: raise SystemExit('a game is running; quit it first (or pass --quit-running)')
        subprocess.run(['osascript', '-e', 'tell application id "org.macnativegames.totk" to quit'], capture_output=True)
        for _ in range(30):
            if not game_running(): break
            time.sleep(1)
        if game_running(): raise SystemExit('the running game did not quit')
    rd = outdir / name; rd.mkdir(parents=True, exist_ok=True)
    PERF = rd / 'perf.csv'
    # Fresh copy of the player's profile for every run: same starting point, the player's saves untouched.
    profile = DATA / 'tests/bench-profile'   # test data under the user data folder's tests/ (AGENTS.md)
    if profile.exists(): shutil.rmtree(profile)
    profile.parent.mkdir(parents=True, exist_ok=True); shutil.copytree(DATA / 'profile', profile)
    cmd_file = rd / 'input.txt'; cmd_file.write_text('')
    env = {'HOME': str(HOME), 'USER': os.environ.get('USER', ''), 'LOGNAME': os.environ.get('USER', ''), 'PATH': '/usr/bin:/bin:/usr/sbin:/sbin',
           'TMPDIR': os.environ.get('TMPDIR', '/tmp'), 'TOTK_PROFILE_DIR': str(profile), 'TOTK_INPUT_FILE': str(cmd_file),
           'TOTK_SLOT_OWNER': 'codex', 'TOTK_SLOT_PURPOSE': f'benchmark {name}', 'TOTK_ASSUME_FOCUS': '1', **config_env(name)}
    if args.hidden: env['TOTK_PAUSE_HIDDEN'] = '1'
    if args.play:   # on screen as the app's "unlimited" setting shows it (launcher.m), MetalFX upscaling on; the player drives
        for k in ('TOTK_PRESENT_SKIP', 'DM_PRESENT_HZ', 'DM_PRESENT_LINK'): env.pop(k, None)
        env.update(TOTK_UNCAPPED='1', TOTK_UNCAPPED_INTERVAL='1', TOTK_MFX_UPSCALE='1')
    env.update(extra_env(args.env))
    if args.cache_ab:
        env.update(TOTK_ARG_EPOCH='0', TOTK_TEXTURE_PLAN='0', TOTK_ARG_EPOCH_CHECK='0', TOTK_TEXTURE_PLAN_CHECK='0')
    if args.gpu_passes: env['TOTK_PASS_GPU_FILE'] = str(rd / 'gpu-passes.tsv')
    samples_path = rd / 'frames.csv'; env['TOTK_FRAME_SAMPLES'] = str(samples_path)
    env['TOTK_PERF_CSV'] = str(PERF)
    executable = APP / 'Contents/Resources/totk/build/totk-native' if args.app else TREE / 'build/totk-native'
    binary_sha256 = hashlib.sha256(executable.read_bytes()).hexdigest()
    if args.profile: env['TOTK_SYMBOL_MAP'] = str(rd / 'symbols.txt')
    perf_off = PERF.stat().st_size if PERF.exists() else 0
    log = rd / 'totk.log'
    if args.app:   # the packaged app's launcher (it writes ~/Library/Logs/MacNativeGames/totk.log)
        GAME_PROC = subprocess.Popen([str(APP / 'Contents/MacOS/TOTK')], env=env, cwd='/', stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        log = LOG
    else:          # what app/launcher.m sets up, from the work tree
        cache = DATA / f'shader-cache/rt-{CHAIN}'
        if 'xcold' in name.split('-'):   # a fresh, empty translation cache for this run only (the player's cache is not touched)
            cache = DATA / f'tests/xcold-cache/rt-{CHAIN}'
            if cache.parent.exists(): shutil.rmtree(cache.parent)
            cache.mkdir(parents=True)
        env.update(TOTK_SHADER_CACHE=str(cache), TOTK_GAME_SLOT_DIR=str(DATA / 'game-slot'), TOTK_ROMFS_PROOF_DIR=str(DATA),
                   TOTK_SHARED_FONTS='extracted/firmware/shared-fonts', TOTK_GPU_ALLOW_SUBMIT='1', TOTK_GPU_WIDEN='1', TOTK_GPU_BLOCKRT='1',
                   TOTK_GPU_MIDCLEAR='1', TOTK_GPU_BUDGET_MB='5000')
        binary = TREE / 'build/totk-native'
        if 'cold' in name.split('-'):   # macOS keeps compiled shaders and pipelines per executable path: a new path starts cold
            # next to the real one: the game finds its tools (shader translator) relative to its own build/ folder
            for old in (TREE / 'build').glob('totk-native-cold-*'): old.unlink()
            binary = TREE / f'build/totk-native-cold-{int(time.time())}'
            shutil.copy2(TREE / 'build/totk-native', binary)
        with open(log, 'w') as lf:
            GAME_PROC = subprocess.Popen([str(binary), 'rewritten/v0', 'boot'], env=env, cwd=TREE, stdout=lf, stderr=lf, start_new_session=True)
    phases, raw_phases = {}, {}
    clock_ns = lambda: time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    def phase(pname, seconds, action=None):
        send(cmd_file, f'settings benchstatus measuring {pname}')
        time.sleep(0.25)   # let the status update finish before the sample window
        start = len(perf_rows_since(perf_off))
        if action: action()
        begin_ns = clock_ns()
        time.sleep(seconds + 1.5)
        rows = perf_rows_since(perf_off)[start:start + seconds]
        samples = sample_rows(samples_path, begin_ns, begin_ns + seconds * 1_000_000_000)
        # Retain the raw render target for diagnostics, but show the cropped
        # presentation picture in the preview (raw targets can have padding).
        preview = rd / f'{name}-{pname}-preview.png'
        window_shot = rd / f'{name}-{pname}-window.png'
        send(cmd_file, f'shot {rd}/{name}-{pname}.png')
        send(cmd_file, f'wshot {preview}')
        if not wait_for(preview.exists, 15): raise RuntimeError('benchmark preview was not captured')
        send(cmd_file, f'settings benchpreview {preview}')
        send(cmd_file, f'settings benchstatus {pname} complete')
        send(cmd_file, f'settings benchshot {window_shot}')
        if not wait_for(window_shot.exists, 15): raise RuntimeError('benchmark window was not captured')
        phases[pname] = rows
        raw_phases[pname] = samples
        with open(rd / f'{pname}-frames.csv', 'w', newline='') as f:
            if samples:
                w = csv.DictWriter(f, fieldnames=list(samples[0])); w.writeheader(); w.writerows(samples)
        with open(rd / f'{pname}.csv', 'w', newline='') as f:
            if rows:
                w = csv.DictWriter(f, fieldnames=list(rows[0].keys())); w.writeheader(); w.writerows(rows)
    try:
        time.sleep(3)
        # Throughput mode never presents a drawable. Readiness is actual game
        # frame production, rather than the display's first-present callback.
        if not wait_for(lambda: any(float(r.get('fps') or 0) > 0 for r in perf_rows_since(perf_off)), 900): raise RuntimeError('no rendered frames')
        if not {'cold', 'xcold'} & set(name.split('-')): wait_for(lambda: '管线预热完成' in log.read_text(errors='replace'), 600)   # warm-up done
        time.sleep(args.settle)
        play(name, args, rd, cmd_file, perf_off, phase, phases, raw_phases, samples_path)
    finally:   # always close the game, also after an error
        send(cmd_file, 'close')
        for _ in range(30):
            if not game_running(): break
            time.sleep(1)
        if log != rd / 'totk.log' and log.exists(): shutil.copy(log, rd / 'totk.log')
    summary = {p: summarize(r, raw_phases.get(p)) for p, r in phases.items()}
    (rd / 'summary.json').write_text(json.dumps({'config': name, 'measurement': 'uncapped renderer throughput',
        'binary_sha256': binary_sha256, 'env': env, 'phases': summary,
        'cache_ab': {'order': [0, 1, 1, 0], 'flags': CACHE_FLAGS[args.cache_ab],
                     'settle_seconds': 2, 'seconds_per_window': args.seconds} if args.cache_ab else None,
        'gpu_diagnostic_frames': args.gpu_passes,
        'sample_clock': 'CLOCK_MONOTONIC; GPU retirement timestamps, approximately eight frames after rendering'}, indent=2))
    return summary


def play(name, args, rd, cmd_file, perf_off, phase, phases, raw_phases, samples_path):
    phase('title', args.seconds)
    if args.hidden:   # the window minimized, then shown again (no game input)
        phase('hidden', args.seconds, lambda: send(cmd_file, 'settings minimize'))
        send(cmd_file, 'settings restore'); time.sleep(4)
        phase('restored', args.seconds)
        return
    if not args.title_only:
        send(cmd_file, 'settings benchstatus loading saved game')
        t_press = time.time(); press_ns = time.clock_gettime_ns(time.CLOCK_MONOTONIC); press_row = len(perf_rows_since(perf_off))
        draws = lambda r: float(r.get('draws_per_frame') or 0)
        # Continue (and on a save list, the autosave) until a loading screen shows (under 100 draws a frame); then the
        # world: three seconds in a row of > 700 draws a frame after it.
        loading = lambda: any(draws(x) < 100 for x in perf_rows_since(perf_off)[press_row:])
        for i in range(4):
            send(cmd_file, f'key {KEY_A} 150'); send(cmd_file, f'shot {rd}/{name}-press{i + 1}.png')
            if wait_for(loading, 8): break
        if not loading(): raise RuntimeError('no loading screen after pressing A')
        def world():
            r = perf_rows_since(perf_off)[press_row:]
            k = max(i for i, x in enumerate(r) if draws(x) < 100)
            after = r[k + 1:]
            return len(after) >= 3 and all(draws(x) > 700 for x in after[-3:])
        if not wait_for(world, 180): raise RuntimeError('the world did not load after the loading screen')
        (rd / 'load.txt').write_text(f'{time.time() - t_press:.1f} s from the press to the world\n')
        time.sleep(5); phases['load'] = perf_rows_since(perf_off)[press_row:]   # the press, the loading screen and the first world seconds
        end_ns = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
        time.sleep(1.5)   # let the batched sample writer flush this window
        raw_phases['load'] = sample_rows(samples_path, press_ns, end_ns)
        time.sleep(args.settle)
        if args.cache_ab:
            for index, enabled in enumerate((0, 1, 1, 0), 1):
                for flag in CACHE_FLAGS[args.cache_ab]: send(cmd_file, f'flag {flag} {enabled}')
                time.sleep(2)   # input polling and eight-frame GPU retirement lag
                phase(f'cache-{index}-{"on" if enabled else "off"}', args.seconds)
        phase('idle', args.seconds)
        def pan():   # hold the right stick in 2.5 s steps (the input file allows 5 s per key command)
            for _ in range(max(1, args.seconds // 3)): send(cmd_file, f'key {KEY_RIGHT_STICK_RIGHT} 2500')
        phase('pan', args.seconds, pan)
        if args.gpu_passes:
            send(cmd_file, 'settings benchstatus profiling GPU passes')
            time.sleep(3)   # drain camera input and screenshot work first
            send(cmd_file, f'flag pass_gpu {args.gpu_passes}')
            gpu_path = Path(cmd_file).parent / 'gpu-passes.tsv'
            if not wait_for(lambda: gpu_path.exists() and len(gpu_path.read_text().splitlines()) > 1, 20):
                raise RuntimeError('GPU pass timing did not produce a report')
            time.sleep(1)   # allow remaining diagnostic frames to complete
        if args.profile:   # standing still, CPU profile with guest code named (scripts/profile_guest.py)
            send(cmd_file, 'settings benchstatus collecting CPU profile')
            log = rd / 'totk.log' if (rd / 'totk.log').exists() else LOG
            r = subprocess.run([sys.executable, '-I', str(Path(__file__).with_name('profile_guest.py')), '--log', str(log), '--symbols', str(rd / 'symbols.txt'),
                                *([] if args.profile_plain else ['--input-file', str(cmd_file)]), '--perf', str(PERF), '--seconds', str(args.profile), '--out', str(rd / 'sample.txt')], capture_output=True, text=True)
            (rd / 'profile.txt').write_text(r.stdout + r.stderr); print(r.stdout + r.stderr, flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--configs', nargs='+', default=['900p-unl', '1080p-unl', '1440p-unl'])
    ap.add_argument('--seconds', type=int, default=20, help='seconds per phase')
    ap.add_argument('--settle', type=int, default=8, help='seconds to wait before a phase')
    ap.add_argument('--title-only', action='store_true', help='no input: title screen only')
    ap.add_argument('--play', action='store_true', help='free play: on screen with the app\'s uncapped display, no scripted input '
                    '(implies --title-only; --seconds is the session length)')
    ap.add_argument('--quit-running', action='store_true', help='quit a running game first')
    ap.add_argument('--profile-plain', action='store_true', help='profile without host-call tracing (hletrace slows the game threads and shifts the bottleneck)')
    ap.add_argument('--env', action='append', default=[], metavar='NAME=VALUE', help='extra environment for the game (diagnostic switches)')
    ap.add_argument('--profile', type=int, default=0, help='after the pan: a CPU profile of N seconds standing still')
    ap.add_argument('--hidden', action='store_true', help='title, then minimized, then restored (no game input)')
    ap.add_argument('--app', action='store_true', help="run the packaged app's launcher instead of the work tree's binary")
    ap.add_argument('--cache-ab', nargs='?', const='both', choices=CACHE_FLAGS, help='quick same-process off/on/on/off comparison; both, arguments or texture')
    ap.add_argument('--gpu-passes', type=int, default=0, choices=range(0, 121), metavar='0..120', help='profile N GPU frames after timing windows; diagnostic only')
    args = ap.parse_args()
    if args.play: args.title_only = True
    for c in args.configs: config_env(c)
    extra_env(args.env)
    outdir = OUT / time.strftime('%Y%m%d-%H%M%S'); outdir.mkdir(parents=True)
    results = {}
    for c in args.configs:
        print(f'== {c}', flush=True)
        try: results[c] = run(c, args, outdir)
        except Exception as e: print(f'   {c}: {e}', flush=True); results[c] = {'error': str(e)}
        args.quit_running = True   # our own game from the previous config
    cols = ['render_fps', 'sample_count', 'p50_ms', 'p95_ms', 'p99_ms', 'max_ms', 'audio_underruns', 'xlate_block_ms', 'gpu_ms', 'cpu_pct', 'long50', 'long100', 'pipelines', 'async_skipped', 'footprint_mb']
    lines = ['Uncapped renderer throughput; no FPS targets. Frame percentiles use pooled per-frame samples.', '',
             '| config | phase | ' + ' | '.join(cols) + ' |', '|' + '---|' * (len(cols) + 2)]
    for c, r in results.items():
        if 'error' in r: lines.append(f'| {c} | error | {r["error"]} |'); continue
        for p, s in r.items():
            if not s: lines.append(f'| {c} | {p} | (no rows) |'); continue
            lines.append(f'| {c} | {p} | ' + ' | '.join(str(s.get(k, '')) for k in cols) + ' |')
    table = '\n'.join(lines)
    (outdir / 'results.md').write_text(table + '\n')
    print(table); print(f'\nresults: {outdir}')


if __name__ == '__main__':
    sys.exit(main())
