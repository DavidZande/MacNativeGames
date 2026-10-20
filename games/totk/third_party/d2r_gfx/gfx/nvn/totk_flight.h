/* TOTK_FLIGHT: flight recorder for play sessions (default off).
   The last ~20 s of finished frame records stay in a fixed ring; on a hitch (automatic) or F9 (player) the ring is
   copied to <root>/<YYYYMMDD-HHMMSS>-<reason>/ together with a screenshot and the per-pass GPU timings of the next
   frames, so a stutter can be explained after the fact. Off: one cached flag read per frame, nothing else.
   TOTK_FLIGHT=1 on; TOTK_FLIGHT_DIR output root (default ~/Library/Logs/MacNativeGames/flight); TOTK_FLIGHT_MS
   automatic hitch threshold in ms (default 50, 0 = hotkey only).
   Included by hle_nvn.c only (the definitions live here): the frame hooks below are the whole interface. */
#pragma once
#include <errno.h>
#include <os/signpost.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include "../../../../runtime/game_version.h"

#define TFL_LOG(...) do { fprintf(stderr, "[flight] " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* The recorder only reads: the player position through the survey's guarded guest-memory readers (no TOTK_SURVEY
   required), the screenshot and pass-timing requests through the graphics layer. All weak: a build without them
   (graphics unit tests) still links and simply skips those parts. */
extern int runtime_player_xyz(float out[3]) __attribute__((weak_import));   /* runtime/world_survey.c */
extern void dm_gfx_shot_at(long frames_ahead, const char *path) __attribute__((weak_import));   /* nvn_mtl.m */
extern int dm_gfx_pass_gpu_request(int frames, const char *path) __attribute__((weak_import));   /* nvn_mtl.m: sets TOTK_PASS_GPU_FILE, then the request */
extern int runtime_output_mode_get(void) __attribute__((weak_import));                          /* runtime/game_front.c */
/* Effective switch states, defined at the bottom of hle_nvn.c (same translation unit). */
extern int dm_nvn_async_on(void), dm_nvn_fence_lag_on(void);

enum { TFL_RING = 2400,          /* ~20 s at 120 FPS */
       TFL_POS_NS = 250000000ull,/* player position sampled at most 4 times a second */
       TFL_AUTO_NS = 10000000000ull,   /* at most one automatic event per 10 s */
       TFL_JOBS = 8 };           /* queued events; a burst of F9 beyond this is dropped */

/* One finished frame: the long-frame record (lf_rec in hle_nvn.c) plus what the flight ring adds. */
typedef struct {
    uint64_t f;
    double fr, nv, t_s, gpu_ms, make_ms, up_mb, up_ms, sync_ms, acq_ms, ucap_ms, mp_ms, hot_ms,
           m_lock, m_addr, m_core, m_host, m_sync, rd_ms, rd_mb;
    unsigned pso, lib, up, rup, faults, hot, majflt;
    int ls;
    uint64_t xq;                 /* translation-thread backlog (queued jobs) when the frame finished */
    float px, py, pz; int pos_ok;
    uint64_t wall_ns;            /* CLOCK_REALTIME at retirement, so the file lines up with other logs */
} tfl_rec;

static const char *const tfl_csv_head =
    "frame,uptime_s,wall_unix_ns,frame_ms,nvn_ms,gpu_ms,make_ms,pso,lib,up,rup,up_mb,up_ms,sync_ms,acq_ms,ucap_ms,"
    "mp_ms,faults,hot,hot_ms,m_lock_ms,m_addr_ms,m_core_ms,m_host_ms,m_sync_ms,rd_ms,rd_mb,majflt,ls,xq_depth,"
    "player_x,player_y,player_z,player_ok\n";

static _Atomic int tfl_on;                 /* 1 after the constructor enabled it; never changes afterwards */
static inline int totk_flight_on(void) { return atomic_load_explicit(&tfl_on, memory_order_relaxed); }   /* the one cached check on the frame path */
static double tfl_auto_ms = 50;
static char tfl_root[900];
static os_log_t tfl_log;
static tfl_rec tfl_ring[TFL_RING];
static uint64_t tfl_head;                   /* records finished so far (ring index = head % TFL_RING) */
static pthread_mutex_t tfl_ring_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t tfl_pos_t; static float tfl_px, tfl_py, tfl_pz; static int tfl_pos_ok;
static uint64_t tfl_last_auto;
static __thread os_signpost_id_t tfl_sp;

typedef struct tfl_job {
    struct tfl_job *next;
    char stamp[24], reason[16];
    tfl_rec trig;
    tfl_rec *frames; unsigned n;
    uint64_t wall_ns;
} tfl_job;
static tfl_job *tfl_q_head, *tfl_q_tail; static unsigned tfl_q_n;
static pthread_mutex_t tfl_q_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tfl_q_cv = PTHREAD_COND_INITIALIZER;

static const char *tfl_env(const char *name, const char *dflt) { const char *v = getenv(name); return v && *v ? v : dflt; }
static uint64_t tfl_wall_ns(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static const char *tfl_switch(const char *name, int dflt_on) {
    const char *e = getenv(name);
    return !e || !*e ? (dflt_on ? "on (default)" : "off (default)") : atoi(e) ? "on" : "off";
}
/* mkdir -p; the leaf is created by the caller so it can tell a fresh folder from an existing one. */
static int tfl_mkdir_p(const char *path) {
    char b[1024]; size_t n = strlen(path);
    if (!n || n >= sizeof b) return -1;
    memcpy(b, path, n + 1);
    for (char *p = b + 1; *p; p++) if (*p == '/') { *p = 0; if (mkdir(b, 0755) && errno != EEXIST) return -1; *p = '/'; }
    return mkdir(b, 0755) && errno != EEXIST ? -1 : 0;
}
/* A folder per event; a second event in the same second gets a numeric suffix instead of sharing files. */
static int tfl_claim_dir(char *out, size_t cap, const char *stamp, const char *reason) {
    if (tfl_mkdir_p(tfl_root)) return -1;
    for (int n = 1; n <= 99; n++) {
        int k = n == 1 ? snprintf(out, cap, "%s/%s-%s", tfl_root, stamp, reason)
                       : snprintf(out, cap, "%s/%s-%s-%d", tfl_root, stamp, reason, n);
        if (k < 0 || (size_t)k >= cap) return -1;
        if (mkdir(out, 0755) == 0) return 0;
        if (errno != EEXIST) return -1;
    }
    return -1;
}
static void tfl_write_event(tfl_job *j) {
    char dir[1024], p[1200];
    if (tfl_claim_dir(dir, sizeof dir, j->stamp, j->reason)) { TFL_LOG("cannot create an event folder under %s: %s", tfl_root, strerror(errno)); return; }
    /* TOTK_FLIGHT: the requests go first, the folder already exists: the screenshot is taken on the next game frame
       and the pass capture must start before the next submissions, while the CSV (the bulk) is written after them. */
    if (dm_gfx_shot_at) { snprintf(p, sizeof p, "%s/shot.png", dir); dm_gfx_shot_at(1, p); }
    if (dm_gfx_pass_gpu_request) {
        snprintf(p, sizeof p, "%s/passes.tsv", dir);
        if (!dm_gfx_pass_gpu_request(3, p)) TFL_LOG("per-pass GPU timing already running; no passes.tsv for %s", dir);
    }
    if (!j->frames || !j->n) TFL_LOG("%s: the ring was still empty; only event.txt", dir);
    if (j->frames && j->n) {
        snprintf(p, sizeof p, "%s/frames.csv", dir);
        FILE *f = fopen(p, "w");
        if (!f) TFL_LOG("cannot write %s: %s", p, strerror(errno));
        else {
            fputs(tfl_csv_head, f);
            for (unsigned i = 0; i < j->n; i++) { const tfl_rec *r = &j->frames[i];
                fprintf(f, "%llu,%.3f,%llu,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%.3f,"
                           "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%d,%llu",
                        (unsigned long long)r->f, r->t_s, (unsigned long long)r->wall_ns, r->fr, r->nv, r->gpu_ms, r->make_ms,
                        r->pso, r->lib, r->up, r->rup, r->up_mb, r->up_ms, r->sync_ms, r->acq_ms, r->ucap_ms, r->mp_ms,
                        r->faults, r->hot, r->hot_ms, r->m_lock, r->m_addr, r->m_core, r->m_host, r->m_sync,
                        r->rd_ms, r->rd_mb, r->majflt, r->ls, (unsigned long long)r->xq);
                if (r->pos_ok) fprintf(f, ",%.3f,%.3f,%.3f,1\n", r->px, r->py, r->pz);
                else fputs(",,,0\n", f);
            }
            fclose(f);
        }
    }
    snprintf(p, sizeof p, "%s/event.txt", dir);
    FILE *f = fopen(p, "w");
    if (!f) { TFL_LOG("cannot write %s: %s", p, strerror(errno)); return; }
    time_t secs = (time_t)(j->wall_ns / 1000000000ull); struct tm tm; localtime_r(&secs, &tm);
    char ts[40]; strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
    fprintf(f, "reason=%s\n", j->reason);
    fprintf(f, "time=%s.%03u\n", ts, (unsigned)((j->wall_ns / 1000000ull) % 1000));
    fprintf(f, "trigger_frame=%llu\n", (unsigned long long)j->trig.f);
    fprintf(f, "trigger_frame_ms=%.3f\n", j->trig.fr);
    fprintf(f, "trigger_nvn_ms=%.3f\n", j->trig.nv);
    fprintf(f, "trigger_gpu_ms=%.3f\n", j->trig.gpu_ms);
    fprintf(f, "trigger_uptime_s=%.3f\n", j->trig.t_s);
    fprintf(f, "trigger_loading=%d\n", j->trig.ls);
    if (j->trig.pos_ok) fprintf(f, "player_x=%.3f\nplayer_y=%.3f\nplayer_z=%.3f\n", j->trig.px, j->trig.py, j->trig.pz);
    else fputs("player_position=unavailable\n", f);
    fprintf(f, "game_version=%s\n", TOTK_GAME_VERSION);
    fprintf(f, "TOTK_ASYNC_REPLAY=%s", dm_nvn_async_on() ? "on" : "off");
    { const char *e = getenv("TOTK_ASYNC_REPLAY"); if (e && *e) fprintf(f, " (env %s)", e); fputc('\n', f); }
    fprintf(f, "TOTK_FENCE_LAG=%s", dm_nvn_fence_lag_on() ? "on" : "off");
    { const char *e = getenv("TOTK_FENCE_LAG"); if (e && *e) fprintf(f, " (env %s)", e); fputc('\n', f); }
    fprintf(f, "TOTK_RT_FEEDBACK_FETCH=%s\n", tfl_switch("TOTK_RT_FEEDBACK_FETCH", 1));
    fprintf(f, "TOTK_ROMFS_CACHE=%s\n", tfl_switch("TOTK_ROMFS_CACHE", 1));
    { const char *rr = getenv("TOTK_RENDER_RES"), *pc = "unknown";
      if (runtime_output_mode_get) { int m = runtime_output_mode_get(); pc = m == 1 ? "1080p, MetalFX 2K" : m == 2 ? "native 2K" : m == 0 ? "1080p" : "unknown"; }
      fprintf(f, "render_resolution=%s\npicture=%s\n", rr && *rr ? rr : "game default (TOTK_RENDER_RES unset)", pc); }
    fprintf(f, "flight_threshold_ms=%.1f\nframes=%u\n", tfl_auto_ms, j->n);
    fclose(f);
}
static void *tfl_writer(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&tfl_q_mu);
        while (!tfl_q_head) pthread_cond_wait(&tfl_q_cv, &tfl_q_mu);
        tfl_job *j = tfl_q_head; tfl_q_head = j->next; if (!tfl_q_head) tfl_q_tail = NULL; tfl_q_n--;
        pthread_mutex_unlock(&tfl_q_mu);
        tfl_write_event(j);
        free(j->frames); free(j);
    }
    return NULL;
}
/* Snapshot the ring (oldest first) and hand it to the writer; called on the render thread for a hitch and on the
   main thread for F9, so it only takes the lock, copies and queues. */
static void tfl_capture(const char *reason) {
    tfl_rec *frames = NULL; unsigned n = 0; tfl_rec trig; memset(&trig, 0, sizeof trig);
    pthread_mutex_lock(&tfl_ring_mu);
    uint64_t head = tfl_head;
    n = (unsigned)(head < TFL_RING ? head : TFL_RING);
    if (n) {
        frames = malloc((size_t)n * sizeof *frames);
        if (frames) for (unsigned i = 0; i < n; i++) frames[i] = tfl_ring[(head - n + i) % TFL_RING];
        trig = tfl_ring[(head - 1) % TFL_RING];
    }
    pthread_mutex_unlock(&tfl_ring_mu);
    if (n && !frames) { TFL_LOG("out of memory for a %u-frame snapshot; event dropped", n); return; }
    tfl_job *j = calloc(1, sizeof *j);
    if (!j) { free(frames); TFL_LOG("out of memory for an event; dropped"); return; }
    snprintf(j->reason, sizeof j->reason, "%s", reason);
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    strftime(j->stamp, sizeof j->stamp, "%Y%m%d-%H%M%S", &tm);
    j->trig = trig; j->frames = frames; j->n = n; j->wall_ns = tfl_wall_ns();
    if (tfl_log) os_signpost_event_emit(tfl_log, OS_SIGNPOST_ID_EXCLUSIVE, "trigger", "reason=%s frame=%llu frame_ms=%.2f",
                                        reason, (unsigned long long)trig.f, trig.fr);
    pthread_mutex_lock(&tfl_q_mu);
    if (tfl_q_n >= TFL_JOBS) { pthread_mutex_unlock(&tfl_q_mu); free(frames); free(j); TFL_LOG("event queue full; dropped %s", reason); return; }
    if (tfl_q_tail) tfl_q_tail->next = j; else tfl_q_head = j;
    tfl_q_tail = j; tfl_q_n++;
    pthread_cond_signal(&tfl_q_cv);
    pthread_mutex_unlock(&tfl_q_mu);
    TFL_LOG("%s event: %u frames, folder written by the background thread", reason, n);
}
/* F9: always an event; returns 0 when the recorder is off so the key reaches the game as before.
   Non-static: runtime/frontend.m calls it through its own declaration (this header is included once, by hle_nvn.c). */
int totk_flight_hotkey(void) {
    if (!totk_flight_on()) return 0;
    tfl_capture("player");
    return 1;
}
/* One finished frame (GPU time known): store it in the ring, then check for an automatic hitch. */
static void totk_flight_record(const tfl_rec *in) {
    if (!totk_flight_on()) return;
    tfl_rec r = *in;
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    r.wall_ns = tfl_wall_ns();
    /* TOTK_FLIGHT: guest memory is read at most four times a second; a hitch does not need finer positions. */
    if (runtime_player_xyz && (!tfl_pos_ok || now - tfl_pos_t >= TFL_POS_NS)) {
        float p[3];
        tfl_pos_ok = runtime_player_xyz(p);
        if (tfl_pos_ok) { tfl_px = p[0]; tfl_py = p[1]; tfl_pz = p[2]; }
        tfl_pos_t = now;
    }
    r.px = tfl_px; r.py = tfl_py; r.pz = tfl_pz; r.pos_ok = tfl_pos_ok;
    pthread_mutex_lock(&tfl_ring_mu);
    tfl_ring[tfl_head % TFL_RING] = r; tfl_head++;
    pthread_mutex_unlock(&tfl_ring_mu);
    /* TOTK_FLIGHT: a frame behind the loading screen is not a hitch the player sees. */
    if (tfl_auto_ms > 0 && !r.ls && r.f >= 300 && r.fr > tfl_auto_ms &&   /* TOTK_FLIGHT: not during boot (the loading flag is not set yet there) */ (!tfl_last_auto || now - tfl_last_auto >= TFL_AUTO_NS)) {
        tfl_last_auto = now;
        tfl_capture("hitch");
    }
}
/* Instruments: one interval per frame, so the trigger events and the frames line up. */
static void totk_flight_frame_begin(void) {
    if (!totk_flight_on()) return;
    tfl_sp = os_signpost_id_generate(tfl_log);
    if (tfl_sp) os_signpost_interval_begin(tfl_log, tfl_sp, "frame");
}
static void totk_flight_frame_end(void) {
    if (!tfl_sp) return;
    os_signpost_interval_end(tfl_log, tfl_sp, "frame");
    tfl_sp = 0;
}
__attribute__((constructor)) static void tfl_start(void) {
    const char *e = getenv("TOTK_FLIGHT");
    if (!e || atoi(e) <= 0) return;
    const char *ms = getenv("TOTK_FLIGHT_MS"); tfl_auto_ms = ms && *ms ? atof(ms) : 50; if (!(tfl_auto_ms >= 0)) tfl_auto_ms = 50;
    const char *dir = getenv("TOTK_FLIGHT_DIR");
    if (dir && *dir == '~' && dir[1] == '/') snprintf(tfl_root, sizeof tfl_root, "%s%s", tfl_env("HOME", "/tmp"), dir + 1);
    else if (dir && *dir) snprintf(tfl_root, sizeof tfl_root, "%s", dir);
    else snprintf(tfl_root, sizeof tfl_root, "%s/Library/Logs/MacNativeGames/flight", tfl_env("HOME", "/tmp"));
    tfl_log = os_log_create("org.macnativegames.totk", "PointsOfInterest");
    pthread_t th;
    if (pthread_create(&th, NULL, tfl_writer, NULL)) { TFL_LOG("【red】cannot start the writer thread; recorder off"); return; }
    pthread_detach(th);
    atomic_store_explicit(&tfl_on, 1, memory_order_relaxed);   /* last: everything above is ready */
    TFL_LOG("on: %d frames in the ring, automatic hitch > %.1f ms (0 = F9 only), output %s", (int)TFL_RING, tfl_auto_ms, tfl_root);
}
