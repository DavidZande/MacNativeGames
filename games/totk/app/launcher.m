// The launcher inside the packaged app (build_app.py compiles it as Contents/MacOS/TOTK).
// The game itself is Contents/Resources/totk/build/totk-native; everything it writes goes to the player's own folders:
//   ~/Library/Application Support/MacNativeGames/TOTK/   profile (account, saves), shader cache, game slot, RomFS proof
//   ~/Library/Logs/MacNativeGames/totk.log             this run (totk.previous.log: the one before)
// If the game is already running, the launcher only brings it to the front.
#import <AppKit/AppKit.h>
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/proc.h>
#include <unistd.h>
#include <sys/clonefile.h>
#include "dmsh_chain.h"   // DMSH_CHAIN: the shader translation chain the cache folder is named after
#include "game_version.h"

/* Local change: rolling save backups. An APFS clone (clonefile: instant, shares the data) of the profile folder goes to
   <data>/backups/<yyyyMMdd-HHmmss>-<reason>; the newest 10 are kept. Shared with runtime/frontend.m (same rules). */
static void profile_backup(NSString *profile, NSString *reason) {
    NSFileManager *fm = NSFileManager.defaultManager; BOOL dir = NO;
    if (![fm fileExistsAtPath:profile isDirectory:&dir] || !dir) return;
    NSString *backups = [[profile stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"backups"];
    [fm createDirectoryAtPath:backups withIntermediateDirectories:YES attributes:nil error:nil];
    NSDateFormatter *f = [NSDateFormatter new]; f.dateFormat = @"yyyyMMdd-HHmmss"; f.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    NSString *dst = [backups stringByAppendingPathComponent:[NSString stringWithFormat:@"%@-%@", [f stringFromDate:NSDate.date], reason]];
    if (clonefile(profile.fileSystemRepresentation, dst.fileSystemRepresentation, 0) != 0) { fprintf(stderr, "[backup] clone of %s failed: %s\n", profile.UTF8String, strerror(errno)); return; }
    NSArray<NSString *> *all = [[fm contentsOfDirectoryAtPath:backups error:nil] sortedArrayUsingSelector:@selector(compare:)];
    NSMutableArray<NSString *> *mine = [NSMutableArray array];
    for (NSString *n in all) if (n.length > 15 && [n characterAtIndex:8] == '-' && ![n hasPrefix:@"."]) [mine addObject:n];
    for (NSUInteger i = 0; i + 10 < mine.count; i++) [fm removeItemAtPath:[backups stringByAppendingPathComponent:mine[i]] error:nil];
    fprintf(stderr, "[backup] %s\n", dst.UTF8String);
}

static void show_error(NSString *detail) {
    NSAlert *alert = [NSAlert new];
    alert.messageText = @"The game could not start";
    alert.informativeText = detail;
    [alert runModal];
}

static NSDictionary<NSString *, NSString *> *kv_read(NSString *path) {
    NSMutableDictionary *out = [NSMutableDictionary dictionary];
    NSString *text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
        NSRange eq = [line rangeOfString:@"="];
        if (eq.location == NSNotFound) continue;
        NSString *key = [line substringToIndex:eq.location];
        if (!out[key]) out[key] = [line substringFromIndex:eq.location + 1];
    }
    return out;
}

static uint64_t pid_start(pid_t pid) {   // 0 if gone or a zombie; with the pid it names one process exactly
    struct proc_bsdinfo bi;
    if (pid <= 0 || proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof bi) != (int)sizeof bi || bi.pbi_status == SZOMB) return 0;
    return bi.pbi_start_tvsec;
}

/* The game slot (runtime/game_slot.c): the running game holds an exclusive lock on <slot>/lock and names itself in
   <slot>/holder. Returns the running game's pid, or 0. */
static pid_t running_game(NSString *slot) {
    int fd = open([slot stringByAppendingPathComponent:@"lock"].fileSystemRepresentation, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int held = flock(fd, LOCK_EX | LOCK_NB) != 0;
    close(fd);
    if (!held) return 0;
    NSDictionary *h = kv_read([slot stringByAppendingPathComponent:@"holder"]);
    /* A player launch must reach runtime/game_slot.c to preempt an agent's
       test. Only an existing player game (or another test for a test request)
       is brought forward here. */
    const char *requestOwner = getenv("TOTK_SLOT_OWNER");
    if (![h[@"owner"] isEqualToString:@"user"] &&
        (!requestOwner || !*requestOwner || !strcmp(requestOwner, "user"))) return 0;
    pid_t pid = [h[@"game_pid"] intValue];
    return pid > 0 && pid_start(pid) == (uint64_t)[h[@"game_start"] longLongValue] ? pid : 0;
}

static BOOL make_dir(NSString *path) {
    return [NSFileManager.defaultManager createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:nil];
}

int main(int argc, char **argv) {
    @autoreleasepool {
        NSString *root = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"totk"];
        NSString *binary = [root stringByAppendingPathComponent:@"build/totk-native"];
        /* --check (build_app.py, once after packaging): only look that the game is there and exit. The first run of a new
           app is when macOS scans the whole bundle (minutes for 16 GB, with nothing on screen); doing it during the build
           makes the player's first start immediate. */
        if (argc > 1 && !strcmp(argv[1], "--check")) {
            int ok = access(binary.fileSystemRepresentation, X_OK) == 0;
            NSString *declared = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
            ok = ok && [declared isEqualToString:@TOTK_GAME_VERSION];
            printf("%s: game version %s\n", ok ? "ok" : "missing binary or version mismatch", TOTK_GAME_VERSION);
            return ok ? 0 : 1;
        }
        if (access(binary.fileSystemRepresentation, X_OK) != 0) {
            show_error(@"The game files are missing from this app. Build it again with build_app.py.");
            return 1;
        }
        NSString *home = NSHomeDirectory();
        NSString *data = [home stringByAppendingPathComponent:@"Library/Application Support/MacNativeGames/TOTK"];
        NSString *logs = [home stringByAppendingPathComponent:@"Library/Logs/MacNativeGames"];
        /* Folders the environment already names are kept (open --env TOTK_PROFILE_DIR=... runs a test on a save copy,
           TOTK_GAME_SLOT_DIR shares one game slot with another build of the game: one game at a time on the Mac). */
        NSString *(^env_or)(const char *, NSString *) = ^NSString *(const char *name, NSString *fallback) {
            const char *v = getenv(name); return v && *v ? @(v) : fallback; };
        NSString *profile = env_or("TOTK_PROFILE_DIR", [data stringByAppendingPathComponent:@"profile"]);
        NSString *slot = env_or("TOTK_GAME_SLOT_DIR", [data stringByAppendingPathComponent:@"game-slot"]);
        NSString *cache = env_or("TOTK_SHADER_CACHE", [data stringByAppendingPathComponent:[NSString stringWithFormat:@"shader-cache/rt-%s", DMSH_CHAIN]]);
        if (!make_dir(profile) || !make_dir(slot) || !make_dir(cache) || !make_dir(logs)) {
            show_error([NSString stringWithFormat:@"Cannot create %@.", data]);
            return 1;
        }
        pid_t running = running_game(slot);
        if (running) {
            [[NSRunningApplication runningApplicationWithProcessIdentifier:running] activateWithOptions:NSApplicationActivateAllWindows];
            return 0;
        }
#if defined(TOTK_GAME_V112) || defined(TOTK_GAME_V143)
        NSString *backup = [profile stringByAppendingFormat:@"-before-%s", TOTK_GAME_VERSION];
        NSFileManager *fm = NSFileManager.defaultManager;
        if (![fm fileExistsAtPath:backup]) {
            NSString *temporary = [backup stringByAppendingFormat:@".copy-%d", getpid()];
            NSError *error = nil;
            if (![fm copyItemAtPath:profile toPath:temporary error:&error] ||
                ![fm moveItemAtPath:temporary toPath:backup error:&error]) {
                [fm removeItemAtPath:temporary error:nil];
                show_error([@"Cannot back up saves before upgrading: " stringByAppendingString:error.localizedDescription ?: @"unknown error"]);
                return 1;
            }
        }
#endif
        /* Local change: a restore chosen in the settings window happens here, before the game opens any save file; then
           the profile is backed up at every player launch. */
        if (!getenv("TOTK_SLOT_OWNER") || !strcmp(getenv("TOTK_SLOT_OWNER"), "user")) {
            NSString *backups = [[profile stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"backups"];
            NSString *request = [backups stringByAppendingPathComponent:@"restore-request"];
            NSString *pick = [[NSString stringWithContentsOfFile:request encoding:NSUTF8StringEncoding error:nil] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
            if (pick.length) {
                [NSFileManager.defaultManager removeItemAtPath:request error:nil];
                NSString *src = [backups stringByAppendingPathComponent:pick.lastPathComponent];
                if ([NSFileManager.defaultManager fileExistsAtPath:src]) {
                    profile_backup(profile, @"before-restore");
                    NSString *aside = [profile stringByAppendingFormat:@".replaced-%d", getpid()];
                    if (rename(profile.fileSystemRepresentation, aside.fileSystemRepresentation) == 0 &&
                        clonefile(src.fileSystemRepresentation, profile.fileSystemRepresentation, 0) == 0) [NSFileManager.defaultManager removeItemAtPath:aside error:nil];
                    else { rename(aside.fileSystemRepresentation, profile.fileSystemRepresentation); show_error([@"Restoring the backup failed: " stringByAppendingString:@(strerror(errno))]); }
                }
            } else profile_backup(profile, @"launch");
        }
        /* Local change: the RomFS lives outside the signed app (build_app.py step_app), in <data>/game-data/<version>/romfs. */
        if (![NSFileManager.defaultManager fileExistsAtPath:[root stringByAppendingPathComponent:@"extracted/" TOTK_GAME_DIR "/romfs/romfs.bin"]]) {
            NSString *store = [[NSString stringWithContentsOfFile:[root stringByAppendingPathComponent:@"game-data.path"] encoding:NSUTF8StringEncoding error:nil]
                               stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] ?: @"";   /* written by build_app.py */
            if (![NSFileManager.defaultManager fileExistsAtPath:[store stringByAppendingPathComponent:@"romfs.bin"]]) {
                show_error([NSString stringWithFormat:@"The game data is missing from %@. Build the app again with build_app.py.", store]);
                return 1;
            }
            setenv("TOTK_ROMFS", store.fileSystemRepresentation, 0);
        }
        if (chdir(root.fileSystemRepresentation) != 0) {   // the game reads its data relative to this folder
            show_error(@"Cannot open the game folder inside the app.");
            return 1;
        }
        setenv("TOTK_PROFILE_DIR", profile.fileSystemRepresentation, 1);
        setenv("TOTK_SHADER_CACHE", cache.fileSystemRepresentation, 1);
        setenv("TOTK_GAME_SLOT_DIR", slot.fileSystemRepresentation, 1);
        setenv("TOTK_ROMFS_PROOF_DIR", data.fileSystemRepresentation, 1);   // the app bundle stays read-only
        setenv("TOTK_SHARED_FONTS", "extracted/firmware/shared-fonts", 1);
        setenv("TOTK_GPU_ALLOW_SUBMIT", "1", 1);
        setenv("TOTK_GPU_WIDEN", "1", 1);
        setenv("TOTK_GPU_BLOCKRT", "1", 1);
        setenv("TOTK_GPU_MIDCLEAR", "1", 1);
        setenv("TOTK_GPU_BUDGET_MB", "5000", 1);
        setenv("TOTK_SLOT_OWNER", "user", 0);
        setenv("TOTK_SLOT_PURPOSE", "player", 0);
        unsetenv("TOTK_AUTOLOAD");   // the game starts at its title screen, as on the console
        /* Local change: the settings window (runtime/frontend.m) saves its startup options here; they become the environment
           variables the game already reads. A variable already set (open --env ...) wins (setenv without overwrite). */
        const char *settingsMode = getenv("TOTK_SETTINGS");   /* off: benchmarks run with their own environment only */
        if (!(settingsMode && !strcmp(settingsMode, "off"))) { NSUserDefaults *d = [[NSUserDefaults alloc] initWithSuiteName:@"org.macnativegames.totk.settings"];
          if ([[d stringForKey:@"mode"] isEqualToString:@"handheld"]) setenv("TOTK_OPERATION_MODE", "handheld", 0);
          NSString *res = [d stringForKey:@"renderRes"] ?: @"900";
          NSDictionary *sizes = @{@"540": @"960x540", @"720": @"1280x720", @"1080": @"1920x1080"};   /* 900: the game's own (dynamic, up to 1600x900) */
          if (sizes[res]) setenv("TOTK_RENDER_RES", [sizes[res] UTF8String], 0);
          else if ([res isEqualToString:@"custom"]) {
              NSInteger w = [d integerForKey:@"customWidth"], h = [d integerForKey:@"customHeight"];
              if (w >= 64 && h >= 64 && w <= 4096 && h <= 4096) setenv("TOTK_RENDER_RES", [NSString stringWithFormat:@"%ldx%ld", (long)w, (long)h].UTF8String, 0);
          }
          if ([d boolForKey:@"metalfxUpscale"]) setenv("TOTK_MFX_UPSCALE", "1", 0);
          if ([d objectForKey:@"perfLog"] && ![d boolForKey:@"perfLog"]) setenv("TOTK_PERF_LOG", "0", 0);
          if ([d objectForKey:@"asyncCompile"] && ![d boolForKey:@"asyncCompile"]) { setenv("TOTK_ASYNC_PSO", "0", 0); setenv("TOTK_ASYNC_XLATE", "0", 0); setenv("TOTK_ASYNC_SHLOAD", "0", 0); }
          if ([d objectForKey:@"pauseHidden"] && ![d boolForKey:@"pauseHidden"]) setenv("TOTK_PAUSE_HIDDEN", "0", 0);
          NSString *fps = [d stringForKey:@"fpsCap"] ?: @"unlimited";
          if ([fps isEqualToString:@"60"]) setenv("DM_PRESENT_HZ", "60", 0);
          else if ([fps isEqualToString:@"30"]) { setenv("TOTK_FPS60", "0", 0); setenv("DM_PRESENT_HZ", "60", 0); }
          if ([d objectForKey:@"fpsCounter"] && ![d boolForKey:@"fpsCounter"]) setenv("TOTK_FPS_OVERLAY", "0", 0);
          if ([d objectForKey:@"shaderCounter"] && ![d boolForKey:@"shaderCounter"]) setenv("TOTK_SHADER_OVERLAY", "0", 0);
          if ([d objectForKey:@"loadCounter"] && ![d boolForKey:@"loadCounter"]) setenv("TOTK_LOAD_OVERLAY", "0", 0);
          NSInteger threads = [d integerForKey:@"warmThreads"], gib = [d integerForKey:@"aliasGiB"];
          if (threads > 0) setenv("TOTK_WARM_THREADS", [NSString stringWithFormat:@"%ld", (long)threads].UTF8String, 0);
          if (gib > 0) setenv("TOTK_ALIAS_GIB", [NSString stringWithFormat:@"%ld", (long)gib].UTF8String, 0); }
        /* runtime command channel (runtime/input.c): echo "shot ~/Desktop/shot.png" >> "<data>/input.txt" */
        { NSString *cmds = env_or("TOTK_INPUT_FILE", [data stringByAppendingPathComponent:@"input.txt"]);
          FILE *cf = fopen(cmds.fileSystemRepresentation, "w"); if (cf) fclose(cf);
          setenv("TOTK_INPUT_FILE", cmds.fileSystemRepresentation, 1); }

        NSString *log = [logs stringByAppendingPathComponent:@"totk.log"];
        [NSFileManager.defaultManager removeItemAtPath:[logs stringByAppendingPathComponent:@"totk.previous.log"] error:nil];
        [NSFileManager.defaultManager moveItemAtPath:log toPath:[logs stringByAppendingPathComponent:@"totk.previous.log"] error:nil];
        FILE *lf = fopen(log.fileSystemRepresentation, "w");
        if (lf) {
            dup2(fileno(lf), STDOUT_FILENO);
            dup2(fileno(lf), STDERR_FILENO);
            fclose(lf);
        }
        char *const arguments[] = {(char *)binary.fileSystemRepresentation, "rewritten/" TOTK_GAME_DIR, "boot", NULL};
        execv(arguments[0], arguments);
        fprintf(stderr, "[launcher] execv failed: %s\n", strerror(errno));
        show_error([NSString stringWithFormat:@"Starting the game failed. See %@.", log]);
        return 1;
    }
}
