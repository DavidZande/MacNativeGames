#!/usr/bin/env python3
"""Prepare game shaders from the selected, verified local RomFS, without a played cache.

All Shader/ packages plus SDK and boot packages are scanned. Binding-dependent
variants are still discovered while playing. The output
contains game-derived data and belongs in the work folder, never in the repo.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import time

CHAIN = 'totk-native-v4-unverified'
MIN_SHADERS = 8000
ANSWERS = dict(tex={}, img={}, attr={}, fout={})
SUFFIX = hashlib.sha256(json.dumps(ANSWERS, sort_keys=True, separators=(',', ':')).encode()).hexdigest()[:12]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def checked(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError('shader container range outside file')
    return data[offset:offset+size]


def sarc(data):
    if data[:4] != b'SARC':
        raise ValueError('expected SARC')
    endian = '<' if data[6:8] == b'\xff\xfe' else '>'
    header, size, start = struct.unpack_from(endian+'HxxII', data, 4)
    if size != len(data) or header < 20 or checked(data, header, 4) != b'SFAT':
        raise ValueError('invalid SARC header')
    count = struct.unpack_from(endian+'H', data, header+6)[0]
    names = header+12+count*16
    if checked(data, names, 4) != b'SFNT':
        raise ValueError('invalid SARC string table')
    names += 8
    result = {}
    for i in range(count):
        _, attribute, lo, hi = struct.unpack_from(endian+'4I', checked(data, header+12+i*16, 16))
        off = names+(attribute & 0xffffff)*4
        if attribute >> 24 != 1 or not names <= off < start or hi < lo or start+lo < start:
            raise ValueError('invalid SARC member')
        name = data[off:data.index(0, off, start)].decode('utf-8')
        if name.startswith('/') or '\\' in name or any(p in ('', '.', '..') for p in name.split('/')) or name in result:
            raise ValueError('unsafe or duplicate SARC name')
        result[name] = checked(data, start+lo, hi-lo)
    return result


def bnsh_shaders(data):
    """BNSH absolute offsets are relative to each embedded BNSH, not BFSHA."""
    if data[:4] != b'BNSH' or checked(data, 12, 2) != b'\xff\xfe' or checked(data, 96, 4) != b'grsc':
        raise ValueError('unsupported BNSH header')
    size = struct.unpack_from('<I', data, 28)[0]
    data = checked(data, 0, size)
    count, variations = struct.unpack_from('<IQ', data, 124)
    checked(data, variations, count*64)
    for i in range(count):
        program = struct.unpack_from('<Q', data, variations+i*64+16)[0]
        checked(data, program, 56)
        for stage, relative in [('vs', 8), ('ps', 40), ('cs', 48)]:
            offset = struct.unpack_from('<Q', data, program+relative)[0]
            if not offset:
                continue
            control_off, code_off, code_size, control_size = struct.unpack_from('<QQII', checked(data, offset+8, 24))
            control = checked(data, control_off, control_size)
            code = checked(data, code_off, code_size)
            if len(control) != 0x880:
                # The SDK utility BNSH uses an older, shorter control layout.
                # Do not invent bytes to make it look like the game's NVN ABI.
                continue
            c1, length = struct.unpack_from('<II', control, 0x700)
            actual = struct.unpack_from('<I', control, 0x714)[0]
            if actual != {'vs': 0, 'ps': 1, 'cs': 5}[stage] or not 0 <= c1 <= length <= len(code) or not length:
                raise ValueError('invalid NVN shader bounds or stage')
            code = code[:length]
            yield digest(control+code), stage, control, code


def inventory(romfs, extra=False):
    import compression.zstd as zstd
    index = json.loads((romfs/'index.json').read_text())
    if index.get('status') != 'pass':
        raise ValueError('RomFS has not passed integrity verification')
    entries = {r['path']: r for r in index['files']}
    shaders = {}
    archives = []
    with (romfs/'romfs.bin').open('rb') as stream:
        def read(name):
            row = entries[name]
            stream.seek(row['offset'])
            raw = stream.read(row['size'])
            if len(raw) != row['size']:
                raise ValueError('short RomFS shader read')
            return raw
        dictionaries = {}
        for raw in sarc(zstd.decompress(read('Pack/ZsDic.pack.zs'))).values():
            d = zstd.ZstdDict(raw)
            dictionaries[d.dict_id] = d
        def decode(raw):
            info = zstd.get_frame_info(raw)
            if info.decompressed_size is None or info.decompressed_size > 512 << 20:
                raise ValueError('unbounded shader package')
            return zstd.decompress(raw, zstd_dict=dictionaries[info.dictionary_id] if info.dictionary_id else None)
        def visit(name, data):
            if data[:4] == b'SARC':
                for member, raw in sarc(data).items():
                    visit(name+'/'+member, raw)
            elif extra or data[:4] in (b'FSHA', b'BNSH'):   # effect files embed BNSH inside other containers
                # BFSHA contains complete, independently addressed BNSH files.
                pos = 0
                while True:
                    pos = data.find(b'BNSH\0\0\0\0', pos)
                    if pos < 0:
                        break
                    try:
                        size = struct.unpack_from('<I', checked(data, pos+28, 4))[0]
                        for key, stage, control, code in bnsh_shaders(checked(data, pos, size)):
                            shaders.setdefault(key, (stage, control, code, name))
                    except ValueError:
                        if not extra: raise
                        pos += 8; continue   # an unusual embedded binary in an effect file: skip it
                    pos += max(size, 8)
        startup = lambda n: (n == 'Lib/NintendoSDK/TextureShader.bnsh' or n == 'Pack/Bootup.Nin_NX_NVN.pack.zs' or
                             n.startswith('Shader/') and n.endswith('.zs'))
        # Local change, --extra: the effect and UI shaders (Effect/, UI/: about 15,000 more in 1.0.0) that the startup set
        # does not cover; they are met the first time an effect or a menu appears.
        names = [n for n in entries if (n.startswith(('Effect/', 'UI/')) if extra else startup(n))]
        for name in sorted(names):
            raw = read(name)
            data = decode(raw) if name.endswith('.zs') else raw
            try:
                visit(name, data)
            except ValueError:
                if not extra: raise   # an effect file with a damaged or unusual container is skipped, not fatal
            archives.append(dict(path=name, sha256=digest(raw)))
            print(f'discovered {len(shaders)} unique startup shaders after {name}', flush=True)
    if not shaders:
        raise ValueError('no startup shaders discovered')
    return index, shaders, archives


def prepare(romfs, output, translator, spirv_cross, version):
    identity = dict(schema=1, chain=CHAIN, version=version,
                    source_romfs_sha256=json.loads((romfs/'index.json').read_text())['romfs_sha256'],
                    translator_sha256=digest(translator.read_bytes()),
                    spirv_cross_sha256=digest(spirv_cross.read_bytes()),
                    preparation_sha256=digest(Path(__file__).read_bytes()))
    marker = output/'seed-manifest.json'
    if marker.exists():
        old = json.loads(marker.read_text())
        if all(old.get(k) == v for k, v in identity.items()) and old.get('shaders', 0) >= MIN_SHADERS and all(
                (output/name).is_file() and digest((output/name).read_bytes()) == sha for name, sha in old['files'].items()):
            print(f'reusing {old["shaders"]} verified prepared startup shaders', flush=True)
            return old
    staging = output.with_name(output.name+'.partial')
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)
    _, shaders, archives = inventory(romfs)
    process = subprocess.Popen([str(translator), '--service', '--spirv-cross', str(spirv_cross),
                                '--out', str(staging), '--chain', CHAIN],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    failed, rows, completed = [], [], []
    started = time.monotonic()
    try:
        for key, (stage, control, code, source) in sorted(shaders.items()):
            variant = key+'-'+SUFFIX
            c1, length = struct.unpack_from('<II', control, 0x700)
            code_path = staging/(variant+'.code')
            code_path.write_bytes(code[0 if stage == 'cs' else 0x30:length])
            request = dict(variant=variant, stage=stage, code=str(code_path.resolve()),
                           c1=code[c1:length].hex(), depth_minus_w=True, **ANSWERS)
            if stage == 'cs':
                request['local'] = list(struct.unpack_from('<III', control, 0x748))
            process.stdin.write(json.dumps(request)+'\n'); process.stdin.flush()
            response = process.stdout.readline()
            if not response:
                raise ValueError('shader preparation service exited unexpectedly')
            reply = json.loads(response)
            code_path.unlink()
            if reply.get('ok'):
                refl = json.loads((staging/(variant+'_b.json')).read_text())
                if refl['chain'] != CHAIN or refl['stage'] != stage:
                    raise ValueError('prepared shader identity mismatch')
                (staging/(key+'.variants')).write_text(json.dumps([dict(variant=variant, deps=refl['deps'])], separators=(',', ':'))+'\n')
                rows.append(f'S\t{variant}_b\t'+str({'vs': 0, 'ps': 1, 'cs': 5}[stage]))
                if stage == 'cs':
                    rows.append(f'C\t{variant}_b')
                completed.append(variant)
            else:
                failed.append(dict(key=key, stage=stage, source=source, error=reply.get('error')))
            if (len(completed)+len(failed)) % 100 == 0:
                print(f'prepared {len(completed)}/{len(shaders)}, skipped {len(failed)}; {time.monotonic()-started:.1f}s', flush=True)
    finally:
        process.stdin.close(); process.wait()
    if len(completed) < MIN_SHADERS:
        raise ValueError(f'only {len(completed)} shaders prepared; at least {MIN_SHADERS} are required for first-launch warm-up')
    # Only ship completed runtime inputs. Failed translations can leave MSL
    # behind before publishing reflection; those must not become cached shaders.
    wanted = set()
    for variant in completed:
        wanted.update([variant+'_b.metal', variant+'_b.json', variant.split('-')[0]+'.variants'])
    for p in staging.iterdir():
        if p.name not in wanted:
            p.unlink()
    (staging/'pso_manifest.tsv').write_text('# Prepared from this app\'s local game data; '+CHAIN+'\n'+'\n'.join(rows)+'\n')
    report = dict(**identity, shaders=len(completed), discovered=len(shaders), skipped=failed,
                  archives=archives, seconds=time.monotonic()-started,
                  files={p.name: digest(p.read_bytes()) for p in sorted(staging.iterdir())})
    (staging/'seed-manifest.json').write_text(json.dumps(report, indent=2)+'\n')
    if output.exists():
        shutil.rmtree(output)
    staging.rename(output)
    print(f'startup seed ready: {len(completed)} shaders, {len(failed)} skipped', flush=True)
    return report


def module_shaders(modules):
    """Shaders embedded in the decompressed game modules (main/sdk/subsdk0 .patched.bin): 40 in 1.0.0, all in main."""
    found = {}
    for path in sorted(Path(modules).glob('*.patched.bin')) if modules else []:
        data = path.read_bytes(); pos = 0
        while True:
            pos = data.find(b'BNSH\0\0\0\0', pos)
            if pos < 0: break
            try:
                size = struct.unpack_from('<I', checked(data, pos+28, 4))[0]
                for key, stage, control, code in bnsh_shaders(checked(data, pos, size)):
                    found.setdefault(key, (stage, control, code, path.name))
                pos += max(size, 8)
            except ValueError:
                pos += 8
    return found


def prepare_extra(romfs, output, translator, spirv_cross, version, jobs=8, modules=None):
    """Local change: the effect and UI shaders (Effect/, UI/) not in the startup set, translated with `jobs` translator
    processes in parallel. Shipped as a second seed; the runtime imports it into the cache and compiles it into the
    system's Metal cache in a low-priority background pass, without loading it into memory (prime_only)."""
    import concurrent.futures, threading
    identity = dict(schema=1, chain=CHAIN, version=version, prime_only=True, modules=bool(modules),
                    source_romfs_sha256=json.loads((romfs/'index.json').read_text())['romfs_sha256'],
                    translator_sha256=digest(translator.read_bytes()), spirv_cross_sha256=digest(spirv_cross.read_bytes()),
                    preparation_sha256=digest(Path(__file__).read_bytes()))
    marker = output/'seed-manifest.json'
    if marker.exists():
        old = json.loads(marker.read_text())
        if all(old.get(k) == v for k, v in identity.items()) and all(
                (output/name).is_file() and digest((output/name).read_bytes()) == sha for name, sha in old['files'].items()):
            print(f'reusing {old["shaders"]} verified prepared effect/UI shaders', flush=True)
            return old
    staging = output.with_name(output.name+'.partial')
    if staging.exists(): shutil.rmtree(staging)
    staging.mkdir(parents=True)
    _, startup, _ = inventory(romfs)
    _, found, archives = inventory(romfs, extra=True)
    for key, value in module_shaders(modules).items(): found.setdefault(key, value)   # plus the game code's own shaders
    shaders = {k: v for k, v in found.items() if k not in startup}
    print(f'{len(shaders)} effect/UI shaders not in the startup set ({len(found)} found)', flush=True)
    local = threading.local(); procs = []; lock = threading.Lock()
    failed, completed = [], []
    started = time.monotonic()
    def service():
        if not hasattr(local, 'p'):
            local.p = subprocess.Popen([str(translator), '--service', '--spirv-cross', str(spirv_cross), '--out', str(staging), '--chain', CHAIN],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
            with lock: procs.append(local.p)
        return local.p
    def one(item):
        key, (stage, control, code, source) = item
        variant = key+'-'+SUFFIX
        c1, length = struct.unpack_from('<II', control, 0x700)
        code_path = staging/(variant+'.code')
        code_path.write_bytes(code[0 if stage == 'cs' else 0x30:length])
        request = dict(variant=variant, stage=stage, code=str(code_path.resolve()), c1=code[c1:length].hex(), depth_minus_w=True, **ANSWERS)
        if stage == 'cs': request['local'] = list(struct.unpack_from('<III', control, 0x748))
        p = service(); p.stdin.write(json.dumps(request)+'\n'); p.stdin.flush()
        response = p.stdout.readline()
        if not response: raise ValueError('shader preparation service exited unexpectedly')
        reply = json.loads(response); code_path.unlink()
        with lock:
            if reply.get('ok'):
                refl = json.loads((staging/(variant+'_b.json')).read_text())
                if refl['chain'] != CHAIN or refl['stage'] != stage: raise ValueError('prepared shader identity mismatch')
                (staging/(key+'.variants')).write_text(json.dumps([dict(variant=variant, deps=refl['deps'])], separators=(',', ':'))+'\n')
                completed.append(variant)
            else:
                failed.append(dict(key=key, stage=stage, source=source, error=reply.get('error')))
            n = len(completed)+len(failed)
            if n % 500 == 0: print(f'prepared {len(completed)}/{len(shaders)}, skipped {len(failed)}; {time.monotonic()-started:.1f}s', flush=True)
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            list(pool.map(one, sorted(shaders.items())))
    finally:
        for p in procs: p.stdin.close(); p.wait()
    wanted = set()
    for variant in completed:
        wanted.update([variant+'_b.metal', variant+'_b.json', variant.split('-')[0]+'.variants'])
    for p in staging.iterdir():
        if p.name not in wanted: p.unlink()
    report = dict(**identity, shaders=len(completed), discovered=len(shaders), skipped=failed, archives=len(archives),
                  seconds=time.monotonic()-started, files={p.name: digest(p.read_bytes()) for p in sorted(staging.iterdir())})
    (staging/'seed-manifest.json').write_text(json.dumps(report, indent=2)+'\n')
    if output.exists(): shutil.rmtree(output)
    staging.rename(output)
    print(f'effect/UI seed ready: {len(completed)} shaders, {len(failed)} skipped, {time.monotonic()-started:.0f} s', flush=True)
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ['romfs', 'output', 'translator', 'spirv-cross']:
        p.add_argument('--'+name, type=Path, required=True)
    p.add_argument('--version', required=True)
    p.add_argument('--extra', action='store_true', help='the effect and UI shaders (Effect/, UI/) instead of the startup set')
    p.add_argument('--jobs', type=int, default=8)
    p.add_argument('--modules', type=Path, help='with --extra: folder of the decompressed modules (*.patched.bin) to scan too')
    a = p.parse_args()
    if a.extra: prepare_extra(a.romfs.resolve(), a.output.resolve(), a.translator.resolve(), a.spirv_cross.resolve(), a.version, a.jobs, a.modules.resolve() if a.modules else None)
    else: prepare(a.romfs.resolve(), a.output.resolve(), a.translator.resolve(), a.spirv_cross.resolve(), a.version)


if __name__ == '__main__':
    main()
