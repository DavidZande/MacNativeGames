// Local change: per-second performance log (~/Library/Logs/MacNativeGames/perf.csv), on by default, TOTK_PERF_LOG=0 off.
// hle_nvn.c reports each game frame once its GPU time is known (8 frames late); a background thread writes one CSV row
// a second: frame times, GPU time, process CPU, long frames, shader/pipeline builds on the submit thread, skipped draws,
// memory, whether the loading screen was up, and the active settings. A header line is written at every launch.
#include <mach/mach.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include "totk_perf.h"

extern uint64_t g_draw_skip[16];                                   /* nvn_mtl.m: draws dropped in the replay, by reason */
extern int mfxup_enabled(void), mfxup_output_enabled(void);        /* totk_mfx_up.m */
extern int mfx_requested(void);                                    /* totk_mfx.m */
extern void mtl_stats(uint64_t *passes, uint64_t *draws);          /* nvn_mtl.m: cumulative */
extern int mtl_world_seen(void);
extern uint64_t g_async_pso_skipped, g_async_pso_kicked;
extern uint64_t totk_rt_block_ns(void), totk_rt_pending(void);
extern void runtime_gate_counts(uint64_t *timer, uint64_t *svc) __attribute__((weak_import));   /* runtime/loader.c */     /* totk_shader_rt.m */           /* nvn_mtl.m: TOTK_ASYNC_PSO */                                   /* nvn_mtl.m: the first world frame was presented */
extern int runtime_output_mode_get(void) __attribute__((weak_import));
extern uint64_t runtime_audio_underruns(void) __attribute__((weak_import));   /* runtime/audio_mixer.c */   /* runtime/game_patches.c (absent in graphics tests) */

#define PERF_MAX 2048   /* frames a second, at most */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static double g_fr[PERF_MAX], g_gpu[PERF_MAX], g_pv[PERF_MAX];
static unsigned g_npv, g_pdrop;
static unsigned g_n, g_pso, g_lib, g_load;
static double g_make_ms;
static uint64_t g_frames;
static int g_on = -1;

static int perf_on(void) { if (g_on < 0) { const char *e = getenv("TOTK_PERF_LOG"); g_on = !(e && !strcmp(e, "0")); } return g_on; }
void totk_perf_frame(double frame_ms, double gpu_ms, unsigned pso, unsigned lib, double make_ms, int loading) {
    if (!perf_on()) return;
    pthread_mutex_lock(&g_mu);
    if (g_n < PERF_MAX) { g_fr[g_n] = frame_ms; g_gpu[g_n] = gpu_ms; g_n++; }
    g_pso += pso; g_lib += lib; g_make_ms += make_ms; g_load |= loading != 0; g_frames++;
    pthread_mutex_unlock(&g_mu);
}
/* Presented intervals (the time between two frames on screen; hle_nvn.c, 8 presents late; -1 = dropped by the system). */
void dm_perf_present(double iv_ms) {
    if (!perf_on()) return;
    pthread_mutex_lock(&g_mu);
    if (iv_ms < 0) g_pdrop++; else if (iv_ms > 0 && g_npv < PERF_MAX) g_pv[g_npv++] = iv_ms;
    pthread_mutex_unlock(&g_mu);
}
static int dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static double pct(const double *v, unsigned n, double p) { if (!n) return -1; unsigned i = (unsigned)(p * n); return v[i < n ? i : n - 1]; }
static double cpu_s(void) { struct rusage u; getrusage(RUSAGE_SELF, &u); return u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6; }
static double footprint_mb(void) {
    task_vm_info_data_t vi; mach_msg_type_number_t c = TASK_VM_INFO_COUNT;
    return task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &c) == KERN_SUCCESS ? vi.phys_footprint / 1048576.0 : -1;
}
static const char *env_or(const char *name, const char *dflt) { const char *v = getenv(name); return v && *v ? v : dflt; }
static void *writer(void *arg) {
    (void)arg;
    char path[1024]; snprintf(path, sizeof path, "%s/Library/Logs/MacNativeGames/perf.csv", env_or("HOME", "/tmp"));
    FILE *f = fopen(path, "a"); if (!f) return NULL;
    time_t now = time(NULL); char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", localtime(&now));
    fprintf(f, "# launch %s pid %d\n", ts, getpid());
    fprintf(f, "time,uptime_s,frames,fps,frame_p50_ms,frame_p95_ms,frame_p99_ms,frame_max_ms,present_p50_ms,present_p95_ms,present_p99_ms,present_max_ms,present_dropped,gpu_avg_ms,gpu_p95_ms,cpu_pct,long50,long100,"
               "new_pipelines,new_libraries,build_ms,skipped_draws,skipped_missing,footprint_mb,loading,world,draws_per_frame,async_queued,async_skipped,audio_underruns,xlate_block_ms,xlate_pending,timer_reads,svc_calls,"
               "mode,render_res,fps60,present_hz,mfx_upscale,mfx_output,picture,interpolation,warm_threads,async_pso\n");
    fflush(f);
    static double fr[PERF_MAX], gp[PERF_MAX], pv[PERF_MAX];
    double t0 = (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9, last_cpu = cpu_s(), last_t = t0;
    uint64_t last_skip = 0, last_miss = 0, last_draws = 0, last_frames = 0, last_aq = 0, last_as = 0, last_au = 0, last_xb = 0, last_tr = 0, last_sv = 0, tr = 0, sv = 0;
    for (;;) {
        sleep(1);
        pthread_mutex_lock(&g_mu);
        unsigned n = g_n; memcpy(fr, g_fr, n * sizeof *fr); memcpy(gp, g_gpu, n * sizeof *gp);
        unsigned npv = g_npv, pdrop = g_pdrop; memcpy(pv, g_pv, npv * sizeof *pv); g_npv = g_pdrop = 0;
        unsigned pso = g_pso, lib = g_lib, load = g_load; double make = g_make_ms; uint64_t frames = g_frames;
        g_n = g_pso = g_lib = g_load = 0; g_make_ms = 0;
        pthread_mutex_unlock(&g_mu);
        double t = (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9, c = cpu_s();
        double cpu = t > last_t ? 100.0 * (c - last_cpu) / (t - last_t) : 0; last_cpu = c; last_t = t;
        unsigned l50 = 0, l100 = 0, ng = 0; double gs = 0;
        for (unsigned i = 0; i < n; i++) { if (fr[i] > 50) l50++; if (fr[i] > 100) l100++; if (gp[i] >= 0) { gp[ng++] = gp[i]; gs += gp[i]; } }
        qsort(fr, n, sizeof *fr, dcmp); qsort(gp, ng, sizeof *gp, dcmp); qsort(pv, npv, sizeof *pv, dcmp);
        uint64_t skip = 0; for (int i = 0; i < 16; i++) skip += g_draw_skip[i];
        uint64_t miss = g_draw_skip[15], passes, draws; mtl_stats(&passes, &draws);
        if (runtime_gate_counts) runtime_gate_counts(&tr, &sv);
        double dpf = frames > last_frames ? (double)(draws - last_draws) / (double)(frames - last_frames) : 0; last_draws = draws; last_frames = frames;
        now = time(NULL); strftime(ts, sizeof ts, "%H:%M:%S", localtime(&now));
        fprintf(f, "%s,%.1f,%llu,%u,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%u,%.2f,%.2f,%.0f,%u,%u,%u,%u,%.1f,%llu,%llu,%.0f,%u,%d,%.0f,%llu,%llu,%llu,%.1f,%llu,%llu,%llu,%s,%s,%s,%s,%d,%d,%d,%d,%s,%s\n",
                ts, t - t0, (unsigned long long)frames, n, pct(fr, n, .5), pct(fr, n, .95), pct(fr, n, .99), n ? fr[n - 1] : -1, pct(pv, npv, .5), pct(pv, npv, .95), pct(pv, npv, .99), npv ? pv[npv - 1] : -1, pdrop,
                ng ? gs / ng : -1, pct(gp, ng, .95), cpu, l50, l100, pso, lib, make,
                (unsigned long long)(skip - last_skip), (unsigned long long)(miss - last_miss), footprint_mb(), load, mtl_world_seen(), dpf, (unsigned long long)(g_async_pso_kicked - last_aq), (unsigned long long)(g_async_pso_skipped - last_as), (unsigned long long)((runtime_audio_underruns ? runtime_audio_underruns() : 0) - last_au), (totk_rt_block_ns() - last_xb) / 1e6, (unsigned long long)totk_rt_pending(), (unsigned long long)(tr - last_tr), (unsigned long long)(sv - last_sv),
                env_or("TOTK_OPERATION_MODE", "docked"), env_or("TOTK_RENDER_RES", "game"), env_or("TOTK_FPS60", "1"), env_or("DM_PRESENT_HZ", "display"),
                mfxup_enabled(), mfxup_output_enabled(), runtime_output_mode_get ? runtime_output_mode_get() : -1, mfx_requested(), env_or("TOTK_WARM_THREADS", "auto"), env_or("TOTK_ASYNC_PSO", "0"));
        last_aq = g_async_pso_kicked; last_as = g_async_pso_skipped; last_au = runtime_audio_underruns ? runtime_audio_underruns() : 0; last_xb = totk_rt_block_ns(); last_tr = tr; last_sv = sv;
        fflush(f);
        last_skip = skip; last_miss = miss;
    }
    return NULL;
}
__attribute__((constructor)) static void perf_start(void) {
    if (!perf_on() || getenv("TOTK_HEADLESS")) return;
    pthread_t th; if (!pthread_create(&th, NULL, writer, NULL)) pthread_detach(th);
}
