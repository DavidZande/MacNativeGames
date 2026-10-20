// Included by nvn_mtl.m (before shader_load): runtime shaders precompiled to .metallib with Xcode's Metal compiler.
/* TOTK (2026-10-20): on macOS every translated shader (<key>.metal in totk_rt_dir()) was compiled from MSL source at each
   launch and the OS kept the result in its shared Metal cache. That cache (one per user, ~1 GB, shared by every app) is
   cleared or rotated now and then; the launch after that compiled all ~25,000 libraries again: 74 s before the first
   frame and ~3.5 min more on 8 threads during play (flight recorder 2026-10-20, the 173 ms hitch and the dip that
   locked TOTK_VRR at 40 Hz). Loading a compiled library takes ~0.2 ms instead of ~1 s cold.

   The iOS port already loads <key>.metallib next to the source (compiled on the Mac). Here the game does the same on
   macOS, and bakes the missing ones itself: after the warm-up, TOTK_RT_BAKE_THREADS (default 2) background-priority
   threads run Xcode's `metal` compiler (Metal Toolchain component) with the options the runtime uses (its default
   language version, safe math) and write <key>.metallib. The output carries the source's modification time; a library
   is used only while both times are equal, so a source rewritten later (new translation, seed import: atomic writes)
   is compiled from source again and re-baked. A zero-size stamped file marks a source the compiler rejects.
   scripts/bake_metallibs.sh bakes a whole cache at once with all cores.
   TOTK_RT_METALLIB=0: off (compile from source as before). Also off with TOTK_MSL_SALT or TOTK_FAST_MATH. */
#include <spawn.h>
#include <pthread/spawn.h>   // posix_spawnattr_set_qos_class_np
#include <sys/wait.h>

static int rtm_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TOTK_RT_METALLIB"), *s = getenv("TOTK_MSL_SALT"), *f = getenv("TOTK_FAST_MATH");
        v = !(e && !atoi(e)) && !(s && *s) && !(f && atoi(f));
        MLOG("precompiled runtime shaders (TOTK_RT_METALLIB): %s", v ? "on" : "off");
    }
    return v;
}
static int rtm_fresh(const char *src, const char *lib, int need_code) {
    struct stat a, b; if (stat(src, &a) || stat(lib, &b) || (need_code && b.st_size <= 0)) return 0;
    return a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec && a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec;
}
static int rtm_has(NSString *key) {
    if (!rtm_on()) return 0; NSString *d = totk_rt_dir();
    return rtm_fresh([d stringByAppendingFormat:@"/%@.metal", key].fileSystemRepresentation, [d stringByAppendingFormat:@"/%@.metallib", key].fileSystemRepresentation, 1);
}
static id<MTLLibrary> rtm_load(NSString *key) {
    if (!rtm_has(key)) return nil;
    NSError *e = nil; id<MTLLibrary> l = lib_from_file([totk_rt_dir() stringByAppendingFormat:@"/%@.metallib", key], &e);
    static _Atomic int bad; if (!l && atomic_fetch_add(&bad, 1) < 5) MLOG("【红】precompiled shader %s not loadable (%s): compiling the source", key.UTF8String, e.localizedDescription.UTF8String);
    return l;
}

/* The compiler path, or nil without the Metal Toolchain (`xcrun -f metal` names a path even then: run it once). */
static NSString *rtm_compiler(void) {
    FILE *p = popen("/usr/bin/xcrun -f metal 2>/dev/null", "r"); char buf[1024] = {0};
    if (!p) return nil; int ok = fgets(buf, sizeof buf, p) != NULL; pclose(p); if (!ok) return nil;
    buf[strcspn(buf, "\n")] = 0;
    char *argv[] = { buf, "--version", NULL }; pid_t pid; int st = -1;
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0); posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    if (posix_spawn(&pid, buf, &fa, NULL, argv, NULL) == 0) waitpid(pid, &st, 0);
    posix_spawn_file_actions_destroy(&fa);
    return (st == 0) ? @(buf) : nil;
}
/* 1 baked, 0 rejected by the compiler (marked), -1 skipped (source changed meanwhile / spawn failed). */
static int rtm_bake_one(const char *cc, const char *stdflag, NSString *dir, NSString *key) {
    NSString *src = [dir stringByAppendingFormat:@"/%@.metal", key], *lib = [dir stringByAppendingFormat:@"/%@.metallib", key];
    NSString *tmp = [lib stringByAppendingFormat:@".tmp%d", getpid()];
    struct stat s0, s1; if (stat(src.fileSystemRepresentation, &s0)) return -1;
    char *argv[] = { (char *)cc, (char *)stdflag, "-fmetal-math-mode=safe", "-w", "-o", (char *)tmp.fileSystemRepresentation, (char *)src.fileSystemRepresentation, NULL };
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0); posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t at; posix_spawnattr_init(&at); posix_spawnattr_set_qos_class_np(&at, QOS_CLASS_BACKGROUND);   // efficiency cores: never compete with the game threads
    pid_t pid; int st = -1, r = posix_spawn(&pid, cc, &fa, &at, argv, NULL);
    posix_spawnattr_destroy(&at); posix_spawn_file_actions_destroy(&fa);
    if (r) return -1; waitpid(pid, &st, 0);
    if (stat(src.fileSystemRepresentation, &s1) || s1.st_mtimespec.tv_sec != s0.st_mtimespec.tv_sec || s1.st_mtimespec.tv_nsec != s0.st_mtimespec.tv_nsec) { unlink(tmp.fileSystemRepresentation); return -1; }
    int ok = st == 0;
    if (!ok) { int fd = open(tmp.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) return -1; ftruncate(fd, 0); close(fd); }
    struct timespec ts[2] = { { 0, UTIME_NOW }, s0.st_mtimespec };
    if (utimensat(AT_FDCWD, tmp.fileSystemRepresentation, ts, 0) || rename(tmp.fileSystemRepresentation, lib.fileSystemRepresentation)) { unlink(tmp.fileSystemRepresentation); return -1; }
    return ok;
}
static void rtm_bake_start(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *e = getenv("TOTK_RT_BAKE_THREADS"); int n = e ? atoi(e) : 2; if (n > 8) n = 8;
        if (!rtm_on() || n <= 0) return;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_BACKGROUND, 0), ^{ @autoreleasepool {
            NSString *cc = rtm_compiler();
            if (!cc) { MLOG("precompiled runtime shaders: no Metal compiler (install it once: xcodebuild -downloadComponent MetalToolchain); shaders stay compiled from source at launch"); return; }
            NSUInteger v = [MTLCompileOptions new].languageVersion;   // the runtime compiles with its default language version
            NSString *stdflag = [NSString stringWithFormat:@"-std=metal%lu.%lu", (unsigned long)(v >> 16), (unsigned long)(v & 0xffff)];
            NSString *dir = totk_rt_dir(); NSMutableArray<NSString *> *todo = [NSMutableArray array];
            for (NSString *f in [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil]) {
                if (![f hasSuffix:@".metal"]) continue; NSString *k = f.stringByDeletingPathExtension;
                if (!rtm_fresh([dir stringByAppendingPathComponent:f].fileSystemRepresentation, [dir stringByAppendingFormat:@"/%@.metallib", k].fileSystemRepresentation, 0)) [todo addObject:k];
            }
            if (!todo.count) return;
            MLOG("precompiled runtime shaders: baking %lu new ones in the background (%d threads, %s)", (unsigned long)todo.count, n, stdflag.UTF8String);
            __block _Atomic uint64_t baked = 0, rejected = 0; uint64_t t0 = bp_now(); NSUInteger count = todo.count;
            dispatch_apply((size_t)n, dispatch_get_global_queue(QOS_CLASS_BACKGROUND, 0), ^(size_t t) {
                for (NSUInteger i = t; i < count; i += (NSUInteger)n) @autoreleasepool {
                    int r = rtm_bake_one(cc.fileSystemRepresentation, stdflag.UTF8String, dir, todo[i]);
                    if (r > 0) atomic_fetch_add(&baked, 1); else if (r == 0) atomic_fetch_add(&rejected, 1);
                }
            });
            MLOG("precompiled runtime shaders: %llu baked, %llu rejected by the compiler, %.1f s", (unsigned long long)baked, (unsigned long long)rejected, (bp_now() - t0) / 1e9);
        } });
    });
}
