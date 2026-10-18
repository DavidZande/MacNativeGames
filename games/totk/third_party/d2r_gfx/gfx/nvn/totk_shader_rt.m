// TOTK: runtime shader translation. A shader is identified by SHA-256(control 0x880 + code area); each
// translation ("variant") also records the translator answers it depended on (texture slot types, vertex
// input and colour output component types). A variant is reused only when those answers match the current
// bound state; otherwise a new one is produced by the translation service and cached on disk: the native program
// build/shader-service/totk_xlate --service when it is there (tools/totk_xlate/Service.cs, NativeAOT: no Python, no .NET
// runtime; 2026-10-17), else scripts/shader_service.py with the .NET translator. Same requests, replies and cache files.
#import <Foundation/Foundation.h>
#include <fcntl.h>
#include <signal.h>
#include <TargetConditionals.h>
#include "totk_shader_rt.h"
#include "dmsh_chain.h"
#include "../../host/hle.h"
#include <CommonCrypto/CommonDigest.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <stdatomic.h>
#include "totk_shader_seed.h"
extern void runtime_boot_status(const char *,const char *) __attribute__((weak_import));
int totk_depth_near_minus_w(void);   // hle_nvn.c: 1 when the game selected NVN_DEPTH_MODE_NEAR_IS_MINUS_W
uint64_t dm_nvn_frame_count(void);

extern char **environ;
static pthread_mutex_t g_rt_lock = PTHREAD_MUTEX_INITIALIZER;
static NSMutableDictionary<NSString *, NSMutableArray<NSDictionary *> *> *g_variants;
static NSMutableSet<NSString *> *g_msl_checked;
static FILE *g_svc_in, *g_svc_out;
static NSString *g_root, *g_dir;
static uint64_t g_translated;
/* The bottom-left box (nvn_mtl.m shader_overlay_prepare): variants new to the cache this run, when the last one came,
   and the variants in the cache (counted once in the background, plus the new ones since). */
static _Atomic uint64_t g_new_variants, g_new_variant_ns, g_new_at_count;
static _Atomic int64_t g_cached_variants = -1;

static NSString *root_dir(void) {   // native-port/ (the executable lives in native-port/build/)
    if (g_root) return g_root;
    char path[4096]; uint32_t n = sizeof path;
    if (_NSGetExecutablePath(path, &n)) die("executable path too long");
    g_root = [[[@(path) stringByResolvingSymlinksInPath] stringByDeletingLastPathComponent] stringByDeletingLastPathComponent];
    return g_root;
}
NSString *totk_rt_dir(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *e = getenv("TOTK_SHADER_CACHE");
#if TARGET_OS_IPHONE
        g_dir = e ? @(e) : [NSString stringWithFormat:@"shader-cache/rt-%s", DMSH_CHAIN];   // in Documents (the app's working directory)
#else
        g_dir = e ? @(e) : [root_dir() stringByAppendingFormat:@"/build/shader-cache/rt-%s", DMSH_CHAIN];
#endif
        [NSFileManager.defaultManager createDirectoryAtPath:g_dir withIntermediateDirectories:YES attributes:nil error:nil];
#if !TARGET_OS_IPHONE
        const char *seedMode = getenv("TOTK_SHADER_SEED");
        if (!(seedMode && !strcmp(seedMode, "off"))) {
            NSError *error = nil;
            if (runtime_boot_status) runtime_boot_status("检查着色器缓存","校验 App 附带的着色器，并按需导入本机缓存");
            NSDictionary *seed = totk_seed_import([root_dir() stringByAppendingPathComponent:@"build/shader-seed"], g_dir, &error);
            if (!seed) die("startup shader preparation: %s", error.localizedDescription.UTF8String);
            if ([seed[@"available"] boolValue]) {
                unsigned long long imported=[seed[@"imported_files"] unsignedLongLongValue];
                LOG("[shader-seed] 发现随 App 准备的着色器 %llu 个，导入 %llu 个文件；首次启动即可预热",
                    [seed[@"shaders"] unsignedLongLongValue], imported);
                if (runtime_boot_status) runtime_boot_status("着色器缓存已就绪",imported
                    ? [NSString stringWithFormat:@"已导入 %llu 个预置文件，开始预热",imported].UTF8String
                    : "本机缓存已存在，无需重新导入；开始预热");
            } else if (runtime_boot_status) runtime_boot_status("着色器缓存已就绪","没有内置着色器种子，继续初始化图形");
        }
#endif
    });
    return g_dir;
}

static void service_spawn(FILE **in, FILE **out) {
    NSString *r = root_dir();
    int to[2], from[2];
    if (pipe(to) || pipe(from)) die("shader service pipes");
    /* Local change: several translators run at once now, so no file descriptor may leak into another one (a translator
       holding another's pipe ends never sees end-of-file and outlives the game): our ends are close-on-exec, and the
       child inherits only stdin, stdout and stderr (POSIX_SPAWN_CLOEXEC_DEFAULT). */
    fcntl(to[1], F_SETFD, FD_CLOEXEC); fcntl(from[0], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, to[0], 0); posix_spawn_file_actions_adddup2(&fa, from[1], 1);
    posix_spawn_file_actions_addinherit_np(&fa, 2);
    /* The background pool spawns from GCD worker threads, which block every signal; a child inherits that mask, and the
       .NET translator then never sees SIGCHLD and waits forever for its spirv-cross child. Start it with an empty mask
       and default handlers. */
    posix_spawnattr_t sa; posix_spawnattr_init(&sa);
    posix_spawnattr_setflags(&sa, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    sigset_t none, all; sigemptyset(&none); sigfillset(&all); sigdelset(&all, SIGKILL); sigdelset(&all, SIGSTOP);
    posix_spawnattr_setsigmask(&sa, &none); posix_spawnattr_setsigdefault(&sa, &all);
    NSString *sc = [r stringByAppendingString:@"/build/spirv-cross/spirv-cross"];
    // TOTK_SHADER_XLATE=<totk_xlate.dll> runs a trial translator build (e.g. build/totk_xlate-opt) without replacing the
    // shared one (through shader_service.py); TOTK_SHADER_DEBUGMODE=0|1 reaches either service through the environment.
    // TOTK_SHADER_SERVICE=python | native picks the service (default: native when build/shader-service/totk_xlate is there).
    const char *xe = getenv("TOTK_SHADER_XLATE"), *want = getenv("TOTK_SHADER_SERVICE"), *dm = getenv("TOTK_SHADER_DEBUGMODE");
    NSString *native = [r stringByAppendingString:@"/build/shader-service/totk_xlate"];
    int have_native = access(native.fileSystemRepresentation, X_OK) == 0;
    int use_native = have_native && !(xe && *xe) && !(want && !strcmp(want, "python"));
    if (want && !strcmp(want, "native") && !use_native) die("TOTK_SHADER_SERVICE=native but %s is not there (make shader-service)", native.UTF8String);
    pid_t pid;
    if (use_native) {
        const char *argv[] = {native.UTF8String, "--service", "--spirv-cross", sc.UTF8String, "--out", totk_rt_dir().UTF8String,
                              "--chain", DMSH_CHAIN, NULL};
        if (posix_spawn(&pid, argv[0], &fa, &sa, (char *const *)argv, environ)) die("start shader service %s", argv[0]);
        if (dm) LOG("[shader] native translator, DebugMode %s", dm);
    } else {
        NSString *script = [r stringByAppendingString:@"/scripts/shader_service.py"], *dotnet = [r stringByAppendingString:@"/tooling/dotnet/dotnet"];
        NSString *xlate = xe && *xe ? @(xe) : [r stringByAppendingString:@"/build/totk_xlate/totk_xlate.dll"];
        const char *argv[] = {"python3", "-I", script.UTF8String, "--dotnet", dotnet.UTF8String, "--xlate", xlate.UTF8String,
                              "--spirv-cross", sc.UTF8String, "--out", totk_rt_dir().UTF8String, "--chain", DMSH_CHAIN, NULL};
        if (posix_spawnp(&pid, "python3", &fa, &sa, (char *const *)argv, environ)) die("start shader service");
        if (xe || dm) LOG("[shader] translator %s, DebugMode %s", xlate.UTF8String, dm ? dm : "default");
    }
    posix_spawn_file_actions_destroy(&fa); posix_spawnattr_destroy(&sa);
    close(to[0]); close(from[1]);
    *in = fdopen(to[1], "w"); *out = fdopen(from[0], "r");
    if (!*in || !*out) die("shader service streams");
    LOG("[shader] runtime translation service started (pid %d, %s), cache %s", pid,
        use_native ? "native build/shader-service/totk_xlate" : "python3 scripts/shader_service.py + .NET", totk_rt_dir().UTF8String);
}

static void service_start(void) { if (!g_svc_in) service_spawn(&g_svc_in, &g_svc_out); }

/* Local change, TOTK_ASYNC_XLATE (default on; off with "Background shader compiling" in the settings): a variant that is
   not translated yet goes to a pool of TOTK_XLATE_THREADS translator processes (default 3) on a background queue, and
   totk_rt_variant returns nil at once, as on the iPhone: the program's draws are skipped (nvn_mtl.m, NvProg missing)
   until the translation lands; totk_rt_gen() then changes and the program is resolved again. The render thread no longer
   waits for the translator. A failed translation still stops the game, as before. */
#if !TARGET_OS_IPHONE
static int async_xlate_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_ASYNC_XLATE"); v = !(e && !atoi(e)); LOG("[shader] background translation (TOTK_ASYNC_XLATE): %s", v ? "on" : "off"); } return v; }
#define XLATE_MAX 8
static struct { FILE *in, *out; int busy; } g_xp[XLATE_MAX];
static int g_xn;
static pthread_mutex_t g_xp_mu = PTHREAD_MUTEX_INITIALIZER;
static dispatch_semaphore_t g_xp_sem;
static NSMutableSet<NSString *> *g_xlate_pending;
static _Atomic uint64_t g_xlate_gen, g_xlate_done, g_xlate_ns, g_xlate_prime_ns;
static dispatch_queue_t xlate_queue(void) {
    static dispatch_queue_t q; static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *e = getenv("TOTK_XLATE_THREADS"); g_xn = e ? atoi(e) : 3; if (g_xn < 1) g_xn = 1; if (g_xn > XLATE_MAX) g_xn = XLATE_MAX;
        g_xp_sem = dispatch_semaphore_create(g_xn);
        q = dispatch_queue_create("totk.shader-translate", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_CONCURRENT, QOS_CLASS_USER_INITIATED, 0));
        LOG("[shader] background translation: %d translator processes", g_xn);
    });
    return q;
}
static NSMutableArray *variants_of(NSString *shader);
static NSString *index_path(NSString *shader);
static NSString *json_string(id obj);
void mtl_prime_library(NSString *key);   /* nvn_mtl.m */
static void xlate_run(NSString *shader, NSString *variant, NSString *stage_name, NSString *request, BOOL existed) {
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    dispatch_semaphore_wait(g_xp_sem, DISPATCH_TIME_FOREVER);
    pthread_mutex_lock(&g_xp_mu); int k = 0; while (g_xp[k].busy) k++; g_xp[k].busy = 1; pthread_mutex_unlock(&g_xp_mu);
    if (!g_xp[k].in) service_spawn(&g_xp[k].in, &g_xp[k].out);
    if (getenv("TOTK_XLATE_TRACE")) { LOG("[xlate-trace] slot %d send %s (fd %d)", k, variant.UTF8String, fileno(g_xp[k].in));
        [request writeToFile:[totk_rt_dir() stringByAppendingFormat:@"/%@.request.json", variant] atomically:YES encoding:NSUTF8StringEncoding error:nil]; }
    fprintf(g_xp[k].in, "%s\n", request.UTF8String); fflush(g_xp[k].in);
    char line[2048];
    if (!fgets(line, sizeof line, g_xp[k].out)) die("shader service exited while translating %s", variant.UTF8String);
    if (getenv("TOTK_XLATE_TRACE")) LOG("[xlate-trace] slot %d reply %.80s", k, line);
    pthread_mutex_lock(&g_xp_mu); g_xp[k].busy = 0; pthread_mutex_unlock(&g_xp_mu);
    dispatch_semaphore_signal(g_xp_sem);
    NSDictionary *reply = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:line length:strlen(line)] options:0 error:nil];
    if (![reply[@"ok"] boolValue]) die("shader %s (%s) translation failed: %s", variant.UTF8String, stage_name.UTF8String, [[reply[@"error"] description] UTF8String]);
    NSData *jd = [NSData dataWithContentsOfFile:[totk_rt_dir() stringByAppendingFormat:@"/%@_b.json", variant]];
    NSDictionary *refl = jd ? [NSJSONSerialization JSONObjectWithData:jd options:0 error:nil] : nil;
    if (![refl isKindOfClass:[NSDictionary class]]) die("shader %s: reflection missing", variant.UTF8String);
    uint64_t tc = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    { static int prime = -1; if (prime < 0) { const char *e = getenv("TOTK_XLATE_PRIME"); prime = !(e && !atoi(e)); }
      if (prime) mtl_prime_library([variant stringByAppendingString:@"_b"]); }   /* before publishing: the render thread then loads it from the compiler cache */
    atomic_fetch_add(&g_xlate_prime_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - tc);
    pthread_mutex_lock(&g_rt_lock);
    NSMutableArray *vs = variants_of(shader);
    [vs addObject:@{@"variant": variant, @"deps": refl[@"deps"] ?: @{}}];
    [g_msl_checked addObject:variant];
    [[json_string(vs) dataUsingEncoding:NSUTF8StringEncoding] writeToFile:index_path(shader) atomically:YES];
    g_translated++;
    if (!existed) { atomic_fetch_add(&g_new_variants, 1); atomic_store(&g_new_variant_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW)); }
    [g_xlate_pending removeObject:variant];
    uint64_t n = atomic_fetch_add(&g_xlate_done, 1) + 1;
    pthread_mutex_unlock(&g_rt_lock);
    atomic_fetch_add(&g_xlate_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0);
    atomic_fetch_add(&g_xlate_gen, 1);
    if (n <= 8 || n % 100 == 0) LOG("[shader] translated in the background %llu: %s %s (%.0f ms)", (unsigned long long)n, stage_name.UTF8String, variant.UTF8String, (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e6);
}
#endif
static _Atomic uint64_t g_xlate_block_ns;   /* time the caller (the render thread) waited for synchronous translations */
uint64_t totk_rt_gen(void) {
#if !TARGET_OS_IPHONE
    return atomic_load(&g_xlate_gen);
#else
    return 0;
#endif
}
uint64_t totk_rt_block_ns(void) { return atomic_load(&g_xlate_block_ns); }
uint64_t totk_rt_pending(void) {
#if !TARGET_OS_IPHONE
    pthread_mutex_lock(&g_rt_lock); uint64_t n = g_xlate_pending.count; pthread_mutex_unlock(&g_rt_lock); return n;
#else
    return 0;
#endif
}

static NSString *hex(const uint8_t *p, size_t n) {
    NSMutableString *s = [NSMutableString stringWithCapacity:n * 2];
    for (size_t i = 0; i < n; i++) [s appendFormat:@"%02x", p[i]];
    return s;
}
static NSString *json_string(id obj) {
    NSData *d = [NSJSONSerialization dataWithJSONObject:obj options:NSJSONWritingSortedKeys error:nil];
    if (!d) die("shader request JSON");
    return [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
}
static NSString *index_path(NSString *shader) { return [totk_rt_dir() stringByAppendingFormat:@"/%@.variants", shader]; }
static NSMutableArray *variants_of(NSString *shader) {
    if (!g_variants) g_variants = [NSMutableDictionary dictionary];
    NSMutableArray *v = g_variants[shader];
    if (v) return v;
    NSData *d = [NSData dataWithContentsOfFile:index_path(shader)];
    NSArray *a = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
    v = [NSMutableArray arrayWithArray:[a isKindOfClass:[NSArray class]] ? a : @[]];
    g_variants[shader] = v;
    return v;
}
static int deps_match(NSDictionary *deps, NSDictionary *ans) {
    NSDictionary *defaults = @{@"tex": @"2d", @"img": @{}, @"attr": @"float", @"fout": @"float"};
    for (NSString *kind in defaults) {
        NSDictionary *want = deps[kind], *have = ans[kind];
        for (NSString *slot in want) { id h = have[slot] ?: defaults[kind]; if (![h isEqual:want[slot]]) return 0; }
    }
    return 1;
}

NSString *totk_rt_variant(NSString *shader, int stage, const uint8_t *control, const uint8_t *code, NSDictionary *answers, NSArray *local) {
    uint64_t started = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    pthread_mutex_lock(&g_rt_lock);
    NSMutableArray *vs = variants_of(shader);
    for (NSUInteger i = 0; i < vs.count; i++) {
        NSDictionary *v = vs[i];
        if (!deps_match(v[@"deps"], answers)) continue;
        NSString *found = v[@"variant"];
        if (!g_msl_checked) g_msl_checked = [NSMutableSet set];
        BOOL checked = [g_msl_checked containsObject:found];
        NSString *msl = checked ? nil :
            [NSString stringWithContentsOfFile:[totk_rt_dir() stringByAppendingFormat:@"/%@_b.metal", found]
                                             encoding:NSUTF8StringEncoding error:nil];
        if (!checked && (!msl || [msl containsString:@"double "] || [msl containsString:@"unsupported_GLSLstd450"])) {
            [vs removeObjectAtIndex:i];  /* regenerate missing or previously un-compilable Metal variant */
            break;
        }
        [g_msl_checked addObject:found];
        pthread_mutex_unlock(&g_rt_lock);
        return found;
    }
    uint32_t c1o, c1t; memcpy(&c1o, control + 0x700, 4); memcpy(&c1t, control + 0x704, 4);
    if (!c1t || c1t > 16u << 20 || c1o > c1t || (stage != 5 && c1o < 0x30)) die("shader %s: code area %#x / c1 %#x", shader.UTF8String, c1t, c1o);
    NSString *stage_name = stage == 0 ? @"vs" : stage == 1 ? @"ps" : stage == 5 ? @"cs" : nil;
    if (!stage_name) die("shader %s: NVN stage %d has no translation chain (tessellation/geometry)", shader.UTF8String, stage);
    uint8_t md[32]; NSData *ad = [json_string(answers) dataUsingEncoding:NSUTF8StringEncoding]; CC_SHA256(ad.bytes, (CC_LONG)ad.length, md);
    NSString *variant = [NSString stringWithFormat:@"%@-%@", shader, hex(md, 6)];
#if !TARGET_OS_IPHONE
    /* Only vertex and fragment shaders go to the background: a skipped draw only leaves an object out for a frame, but a
       skipped compute dispatch leaves buffers unwritten that later passes read as addresses (GPU page faults, then every
       command buffer fails). Compute shaders are translated in place, as before. */
    /* Nor during the first frames: with every draw of the first frame skipped, the present read a canvas that was never
       rendered (GPU page fault at start-up). Shaders met before TOTK_ASYNC_XLATE_AFTER frames (default 120, about 2 s at
       the title) are translated in place. */
    static long after = -1; if (after < 0) { const char *e = getenv("TOTK_ASYNC_XLATE_AFTER"); after = e ? atol(e) : 120; }
    int async_ok = async_xlate_on() && stage != 5 && (long)dm_nvn_frame_count() >= after;
    if (async_ok && [g_xlate_pending containsObject:variant]) { pthread_mutex_unlock(&g_rt_lock); return nil; }   /* still translating */
#endif
    NSString *code_path = [totk_rt_dir() stringByAppendingFormat:@"/%@.code", variant];
    // Graphics code drops the 0x30-byte NVN header (the translator wants SPH + instructions); compute keeps it, as in D2R's build.
    size_t skip = stage == 5 ? 0 : 0x30;
    if (![[NSData dataWithBytes:code + skip length:c1t - skip] writeToFile:code_path atomically:YES]) die("write shader code %s", code_path.UTF8String);
    NSMutableDictionary *rq = [NSMutableDictionary dictionaryWithDictionary:@{@"variant": variant, @"stage": stage_name, @"code": code_path, @"c1": hex(code + c1o, c1t - c1o)}];
    [rq addEntriesFromDictionary:answers];
    if (local) rq[@"local"] = local;
    rq[@"depth_minus_w"] = totk_depth_near_minus_w() ? @YES : @NO;   // nvnDeviceSetDepthMode: NEAR_IS_MINUS_W needs the GL -> Metal clip-z conversion in every vertex shader
#if !TARGET_OS_IPHONE
    if (async_ok) {
        BOOL existed_ = [NSFileManager.defaultManager fileExistsAtPath:[totk_rt_dir() stringByAppendingFormat:@"/%@_b.metal", variant]];
        if (!g_xlate_pending) g_xlate_pending = [NSMutableSet set];
        [g_xlate_pending addObject:variant];
        NSString *request = json_string(rq), *stg = stage_name;
        dispatch_async(xlate_queue(), ^{ xlate_run(shader, variant, stg, request, existed_); });
        pthread_mutex_unlock(&g_rt_lock);
        return nil;
    }
#endif
#if TARGET_OS_IPHONE
    // No translator on the phone (Python + .NET + SPIRV-Cross): the variant cache is translated on the Mac and copied over.
    // A miss records the complete request (with the guest code bytes) under shader-requests/ for ios/tools/fill_shaders.py.
    rq[@"code_hex"] = hex(code + skip, c1t - skip);
    [NSFileManager.defaultManager createDirectoryAtPath:@"shader-requests" withIntermediateDirectories:YES attributes:nil error:nil];
    /* A miss no longer stops the game: the caller skips the draws / dispatches that need this variant (draw-skip reason
       "shader-missing"). The request is written once per variant; after fill_shaders.py pushed the translation, the next
       start compiles it in the background (totk_rt_warm_filled) and moves the request to shader-requests/done/. */
    static NSMutableSet<NSString *> *missed; if (!missed) missed = [NSMutableSet set];
    if (![missed containsObject:variant]) {
        [missed addObject:variant];
        [[json_string(rq) dataUsingEncoding:NSUTF8StringEncoding] writeToFile:[NSString stringWithFormat:@"shader-requests/%@.json", variant] atomically:YES];
        LOG("[shader-miss] %s %s 不在手机的翻译缓存里：跳过用到它的绘制，请求已存 shader-requests/（第 %lu 个；在 Mac 上 ios/tools/fill_shaders.py 补上后下次启动预热）",
            stage_name.UTF8String, variant.UTF8String, (unsigned long)missed.count);
    }
    pthread_mutex_unlock(&g_rt_lock);
    return nil;
#endif
    BOOL existed = [NSFileManager.defaultManager fileExistsAtPath:[totk_rt_dir() stringByAppendingFormat:@"/%@_b.metal", variant]];   // a re-translation of a broken variant is not a new one
    service_start();
    fprintf(g_svc_in, "%s\n", json_string(rq).UTF8String); fflush(g_svc_in);
    char line[2048];
    if (!fgets(line, sizeof line, g_svc_out)) die("shader service exited while translating %s", variant.UTF8String);
    NSDictionary *reply = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:line length:strlen(line)] options:0 error:nil];
    if (![reply[@"ok"] boolValue]) {
        LOG("[survey-shader] frame %llu translation FAILED %s %s after %.2f ms: %s",
            (unsigned long long)dm_nvn_frame_count(), stage_name.UTF8String, variant.UTF8String,
            (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - started) / 1e6, [[reply[@"error"] description] UTF8String]);
        die("shader %s (%s) translation failed: %s", variant.UTF8String, stage_name.UTF8String, [[reply[@"error"] description] UTF8String]);
    }
    NSData *jd = [NSData dataWithContentsOfFile:[totk_rt_dir() stringByAppendingFormat:@"/%@_b.json", variant]];
    NSDictionary *refl = jd ? [NSJSONSerialization JSONObjectWithData:jd options:0 error:nil] : nil;
    if (![refl isKindOfClass:[NSDictionary class]]) die("shader %s: reflection missing", variant.UTF8String);
    [vs addObject:@{@"variant": variant, @"deps": refl[@"deps"] ?: @{}}];
    [g_msl_checked addObject:variant];
    [[json_string(vs) dataUsingEncoding:NSUTF8StringEncoding] writeToFile:index_path(shader) atomically:YES];
    g_translated++;
    if (!existed) { atomic_fetch_add(&g_new_variants, 1); atomic_store(&g_new_variant_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW)); }   // discovered, translated, in the cache
    if (getenv("TOTK_SURVEY_SHADER_LOG")) LOG("[survey-shader] frame %llu translated %s %s in %.2f ms%s",
        (unsigned long long)dm_nvn_frame_count(), stage_name.UTF8String, variant.UTF8String,
        (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - started) / 1e6,
        [refl[@"softfloat64"] boolValue] ? " (FP64 fallback)" : "");
    if (g_translated <= 8 || g_translated % 100 == 0) LOG("[shader] translated %llu: %s %s", (unsigned long long)g_translated, stage_name.UTF8String, variant.UTF8String);
    atomic_fetch_add(&g_xlate_block_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - started);
    pthread_mutex_unlock(&g_rt_lock);
    return variant;
}

void totk_rt_counts(uint64_t *fresh, int64_t *total, uint64_t *last_new_ns) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSString *dir = totk_rt_dir();
        atomic_store(&g_new_at_count, atomic_load(&g_new_variants));
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{   // ~45,000 directory entries: not on the present thread
            int64_t n = 0; DIR *d = opendir(dir.fileSystemRepresentation); struct dirent *e;
            if (d) { while ((e = readdir(d))) { size_t l = strlen(e->d_name); if (l > 8 && !strcmp(e->d_name + l - 8, "_b.metal")) n++; } closedir(d); }
            atomic_store(&g_cached_variants, n);
        });
    });
    uint64_t f = atomic_load(&g_new_variants); int64_t t = atomic_load(&g_cached_variants);
    if (fresh) *fresh = f;
    if (total) *total = t < 0 ? -1 : t + (int64_t)(f - atomic_load(&g_new_at_count));
    if (last_new_ns) *last_new_ns = atomic_load(&g_new_variant_ns);
}
