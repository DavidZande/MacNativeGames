/* Adapted from pinned D2R Mobile host/hle_thread.c, MIT.
 * Game-specific diagnostics and optional timing accounting removed.
 * Core affinity is virtual bookkeeping; Darwin schedules pthreads. */
// Horizon 内核的线程与同步 SVC，原生实现：每个客体线程 = 一个宿主 pthread，跑在 SDK 给的客体栈上。
// SDK 的 nn::os 原样运行，只把它下面的「内核」换成宿主：
//   线程   0x08 CreateThread 0x09 StartThread 0x0A ExitThread 0x0B SleepThread 0x0C/0x0D 优先级 0x0E/0x0F 核掩码
//          0x10 GetCurrentProcessorNumber 0x25 GetThreadId 0x19 CancelSynchronization
//   锁     0x1A ArbitrateLock 0x1B ArbitrateUnlock（标签字 = 持有者句柄 | 0x40000000 有等待者）
//   条件   0x1C WaitProcessWideKeyAtomic 0x1D SignalProcessWideKey
//   地址   0x34 WaitForAddress 0x35 SignalToAddress
//   事件   0x45 CreateEvent 0x11 SignalEvent 0x12 ClearEvent 0x17 ResetSignal 0x18 WaitSynchronization 0x16 CloseHandle
// 语义按公开 Horizon 内核行为（Atmosphère mesosphère 的 KConditionVariable / KAddressArbiter / KSynchronization）。
// 所有等待挂在一把全局锁 G 上，每线程一个条件变量；先求正确，性能后量。
// Thread and synchronization SVCs of the Horizon kernel, implemented natively: each guest thread = one host pthread, running on the guest stack given by the SDK.
// The SDK's nn::os runs as is; only the "kernel" underneath it is replaced by the host:
//   Threads    0x08 CreateThread 0x09 StartThread 0x0A ExitThread 0x0B SleepThread 0x0C/0x0D priority 0x0E/0x0F core mask
//              0x10 GetCurrentProcessorNumber 0x25 GetThreadId 0x19 CancelSynchronization
//   Locks      0x1A ArbitrateLock 0x1B ArbitrateUnlock (tag word = owner handle | 0x40000000 has waiters)
//   Condition  0x1C WaitProcessWideKeyAtomic 0x1D SignalProcessWideKey
//   Address    0x34 WaitForAddress 0x35 SignalToAddress
//   Events     0x45 CreateEvent 0x11 SignalEvent 0x12 ClearEvent 0x17 ResetSignal 0x18 WaitSynchronization 0x16 CloseHandle
// Semantics follow the public Horizon kernel behavior (KConditionVariable / KAddressArbiter / KSynchronization of Atmosphère mesosphère).
// All waits hang on one global lock G, with one condition variable per thread; correctness first, performance to be measured later.
#include "thread_compat.h"
#include "ipc.h"
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#if defined(__APPLE__)
#include <os/os_sync_wait_on_address.h>
#endif
#include <mach/mach.h>
#include <pthread/qos.h>
#include <dlfcn.h>

#define R_TIMEOUT     0xEA01u   // 内核模块 1，描述 117 | Kernel module 1, description 117
#define R_CANCELLED   0xEC01u   // 118
#define R_BAD_HANDLE  0xE401u   // 114
#define R_BAD_STATE   0xFA01u   // 125
#define R_OUT_OF_RES  0xCE01u   // 103
#define WAITERS_BIT   0x40000000u
#define H_CUR_THREAD  0xFFFF8000u

enum { O_FREE, O_THREAD, O_EVENT_R, O_EVENT_W, O_SESSION, O_TMEM };   // O_SESSION / O_TMEM: IPC objects owned by ipc.c
typedef struct event { int signaled; int refs; } event_t;
typedef struct thr {
    uint32_t handle; uint64_t id;
    uint64_t entry, arg, stack_top; uint64_t host_context[2]; int prio, core; uint64_t mask;
    uint64_t born_ns, exit_ns; uint32_t born_tick, exit_tick;   // 建立 / 退出的时刻与当时的游戏拍（现场导出用：作业工人是不是在卡住前退的） | Time of creation / exit and the game tick at that time (for the state dump: did the job worker exit before the stall)
    int started, exited, cancel;
    pthread_t pt; pthread_cond_t cv; uint32_t fx;   // fx: wake sequence for the futex-style wait (tw_wait)
    // 当前等待：kind 0=无 1=锁 2=条件 3=地址 4=同步对象
    // Current wait: kind 0=none 1=lock 2=condition 3=address 4=synchronization object
    int wkind, woke; uint64_t waddr, wkey; uint32_t wtag; int64_t wres;
    uint64_t wseq;             // 登记等待的先后序号：同优先级先到先醒（与 Horizon 内核 KThread 等待树同序） | Sequence number of wait registration: at equal priority first come, first woken (same order as the Horizon kernel's KThread wait tree)
    struct thr *next;          // 等待链 | Wait chain
    struct thr *all;           // 全部线程链（现场导出用） | All-threads chain (for the state dump)
    int last_svc; uint64_t last_lr, last_fp;
    int closed;                // 句柄已关：与 exited 同时成立才释放对象（Horizon：线程对象随最后一个引用消失） | Handle closed: the object is freed only when this holds together with exited (Horizon: the thread object goes away with the last reference)
    uint64_t nsvc;             // SVC 次数（现场导出用） | SVC count (for the state dump)
    mach_port_t mport;         // 宿主线程端口（取 CPU 时间） | Host thread port (to get CPU time)
    uint32_t qos;
    int cur_core;              // virtual core held while running guest code, -1 while blocked / not started
    int preempt;               // a higher-priority thread waits for this thread's core
    uint64_t cseq, core_since; struct thr *cnext;
    uint32_t woke_by;          // local change: handle of the thread whose call woke this one (TOTK_WAITSTAT)
    uint64_t tid;              // local change: host thread id (as `sample` names threads)
    uint64_t wake_at;          // local change: when wake() signalled this thread (handoff latency, perf.csv)
    uint64_t wait_t0;          // local change: when the current guest wait began (0 = not waiting; TOTK_STALL_DUMP)
    uint32_t wh[4]; int nwh;   // handles of the current WaitSynchronization (state dump)
    int last_hle;              // 1 + wrapper index of the last host call made while "hletrace" was on (state dump)
} thr_t;
static thr_t *g_all;

static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
#define MAX_H 4096
static struct { int type; void *obj; } g_h[MAX_H];
static pthread_key_t g_keythr;
static uint64_t g_next_id = 0x51;
static thr_t *g_wait;          // 全部在等的线程（锁 / 条件 / 地址 / 同步） | All waiting threads (lock / condition / address / synchronization)

static uint32_t h_new(int type, void *obj) {     // 调用方持 G | Caller holds G
    for (int i = 1; i < MAX_H; i++) if (!g_h[i].type) { g_h[i].type = type; g_h[i].obj = obj; return 0x1000 + i; }
    die("句柄表满");
}
static void *h_peek(uint32_t h, int type) {      // 只查不报 | Lookup only, no report
    if (h == H_CUR_THREAD && type == O_THREAD) return (g_keythr ? pthread_getspecific(g_keythr) : NULL);
    unsigned i = h - 0x1000;
    if (i == 0 || i >= MAX_H || g_h[i].type != type) return NULL;
    return g_h[i].obj;
}
static void *h_get(uint32_t h, int type) {       // 查不到即报（句柄错是真错误） | Reports if not found (a wrong handle is a real error)
    void *o = h_peek(h, type);
    if (!o) LOG("句柄无效 %#x（要类型 %d）", h, type);
    return o;
}
static thr_t *cur(void) {
    thr_t *t = (g_keythr ? pthread_getspecific(g_keythr) : NULL);
    if (!t) die("非客体线程调用了线程 SVC");
    return t;
}

// ---------- 等待原语（持 G 调用）----------
// ---------- Wait primitives (called holding G) ----------
static void wait_unlink(thr_t *t) {
    for (thr_t **p = &g_wait; *p; p = &(*p)->next) if (*p == t) { *p = t->next; break; }
    t->next = NULL;
}
// 按内核顺序挑等待者：优先级数值小者优先，同优先级先登记者优先（持 G）。kind 1 按锁地址、2 按条件键、3 按地址匹配
// Pick a waiter in kernel order: numerically lower priority first, at equal priority the earlier registered first (holding G). kind 1 matches by lock address, 2 by condition key, 3 by address
static uint64_t g_wseq;
static thr_t *pick(int kind, uint64_t a) {
    thr_t *b = NULL;
    for (thr_t *t = g_wait; t; t = t->next) {
        if (t->wkind != kind || (kind == 2 ? t->wkey : t->waddr) != a) continue;
        if (!b || t->prio < b->prio || (t->prio == b->prio && t->wseq < b->wseq)) b = t;
    }
    return b;
}
// Local change: handoff statistics for perf.csv. wake = signal -> woken thread runs again (holding G);
// core = time spent queued for a virtual core. [0..3] all guest threads, [4..7] the guest main thread.
static uint64_t g_hand[16];   // [13..15]: time each virtual core was held (core utilization)
   // [12]: guest main thread time away in blocking host services (runtime_core_away)
   // [8..11]: guest main thread time blocked in waits of kind 1..4 (lock, condition, address, sync object)
static thr_t *g_main_thr;
void runtime_handoff_stats(uint64_t out[16]) { for (int i = 0; i < 16; i++) out[i] = __atomic_load_n(&g_hand[i], __ATOMIC_RELAXED); }
static void hand_note(thr_t *t, int which, uint64_t ns) {
    __atomic_fetch_add(&g_hand[which * 2], 1, __ATOMIC_RELAXED); __atomic_fetch_add(&g_hand[which * 2 + 1], ns, __ATOMIC_RELAXED);
    if (t == g_main_thr) { __atomic_fetch_add(&g_hand[4 + which * 2], 1, __ATOMIC_RELAXED); __atomic_fetch_add(&g_hand[5 + which * 2], ns, __ATOMIC_RELAXED); }
}
static thr_t *me_or_null(void);
/* Local change: a guest thread's sleep while holding G. Each core hand-off and wake signalled a per-thread condition
   variable (pthread_cond_signal / pthread_cond_wait). With TOTK_FUTEX (default on; =0: the condition variable) the
   thread instead waits on its own 32-bit sequence word with os_sync_wait_on_address and is woken with
   os_sync_wake_by_address_any (Apple's futex-style API, made for exactly this kind of sync emulation). The word is read
   with G held and bumped with G held, so a wake between the unlock and the wait changes it and the wait returns at once;
   every caller re-checks its condition under G, so spurious returns are harmless. deadline_ns: 0 = none, otherwise a
   now_ns() (mach absolute) time. Returns ETIMEDOUT once it has passed. */
static int futex_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_FUTEX"); v = !(e && !strcmp(e, "0")); } return v; }
static void tw_signal(thr_t *t) {   // holding G
#if defined(__APPLE__)
    if (futex_on()) { __atomic_fetch_add(&t->fx, 1, __ATOMIC_RELEASE); os_sync_wake_by_address_any(&t->fx, sizeof t->fx, OS_SYNC_WAKE_BY_ADDRESS_NONE); return; }
#endif
    pthread_cond_signal(&t->cv);
}
static int tw_wait(thr_t *t, uint64_t deadline_ns) {   // holding G; returns with G held
#if defined(__APPLE__)
    if (futex_on()) {
        uint32_t v = __atomic_load_n(&t->fx, __ATOMIC_ACQUIRE); uint64_t now = deadline_ns ? now_ns() : 0;
        if (deadline_ns && now >= deadline_ns) return ETIMEDOUT;
        pthread_mutex_unlock(&G);
        int r = deadline_ns ? os_sync_wait_on_address_with_timeout(&t->fx, v, sizeof t->fx, OS_SYNC_WAIT_ON_ADDRESS_NONE, OS_CLOCK_MACH_ABSOLUTE_TIME, deadline_ns - now)
                            : os_sync_wait_on_address(&t->fx, v, sizeof t->fx, OS_SYNC_WAIT_ON_ADDRESS_NONE);
        int err = r < 0 ? errno : 0;
        pthread_mutex_lock(&G);
        return err == ETIMEDOUT ? ETIMEDOUT : 0;
    }
#endif
    if (!deadline_ns) return pthread_cond_wait(&t->cv, &G);
    uint64_t now = now_ns(); if (now >= deadline_ns) return ETIMEDOUT;
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); uint64_t left = deadline_ns - now;
    dl.tv_sec += left / 1000000000; dl.tv_nsec += left % 1000000000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
    return pthread_cond_timedwait(&t->cv, &G, &dl);
}
static void wake(thr_t *t, int64_t res) { { thr_t *w = me_or_null(); t->woke_by = w ? w->handle : 0; } wait_unlink(t); t->wkind = 0; t->woke = 1; t->wres = res; t->wake_at = now_ns(); tw_signal(t); }
// 挂起直到被唤醒或超时（ns<0 无限、0 立即超时）；返回 wres 或 R_TIMEOUT
// Suspend until woken or timed out (ns<0 infinite, 0 immediate timeout); returns wres or R_TIMEOUT
static int64_t block_inner(thr_t *t, int kind, int64_t ns);
// ---------- Virtual cores ----------
// Horizon runs at most one thread per core, and TOTK keys per-core data (allocators, job queues) by
// GetCurrentCoreNumber without locks. Guest threads therefore hold one of the three application cores while they
// run guest code; a core is given up only where Horizon would switch threads (waits, sleep, yield, exit, blocking
// host services) and handed to the best waiting thread (lowest priority value, then first come). A thread that a
// higher-priority waiter wants to preempt gives its core up at its next SVC. TOTK_FREE_CORES=1 disables this.
#define NCORE 3
static thr_t *g_core_owner[NCORE];
static thr_t *g_core_wait;
static int g_cores_on = -1;
static int cores_enabled(void) { if (g_cores_on < 0) g_cores_on = getenv("TOTK_FREE_CORES") ? 0 : 1; return g_cores_on; }
static int core_allowed(const thr_t *t, int c) { return (int)((t->mask >> c) & 1); }
static void core_grant(int c) {   // holding G: give free core c to the best waiter
    thr_t *b = NULL, **bp = NULL;
    for (thr_t **p = &g_core_wait; *p; p = &(*p)->cnext)
        if (core_allowed(*p, c) && (!b || (*p)->prio < b->prio || ((*p)->prio == b->prio && (*p)->cseq < b->cseq))) { b = *p; bp = p; }
    if (!b) return;
    *bp = b->cnext; b->cnext = NULL; b->cur_core = c; b->core_since = now_ns(); g_core_owner[c] = b; tw_signal(b);
}
static void core_put(thr_t *t) {   // holding G
    if (t->cur_core < 0) return;
    int c = t->cur_core; t->cur_core = -1; t->preempt = 0; g_core_owner[c] = NULL;
    __atomic_fetch_add(&g_hand[13 + c], now_ns() - t->core_since, __ATOMIC_RELAXED);
    core_grant(c);
}
static void core_take(thr_t *t) {   // holding G; waits until a core in t's mask is handed over
    if (!cores_enabled() || t->cur_core >= 0) return;
    if (!(t->mask & ((1u << NCORE) - 1))) die("thread %#x core mask %#llx has no application core", t->handle, (unsigned long long)t->mask);
    int c = t->core >= 0 && t->core < NCORE && core_allowed(t, t->core) && !g_core_owner[t->core] ? t->core : -1;
    for (int i = 0; c < 0 && i < NCORE; i++) if (core_allowed(t, i) && !g_core_owner[i]) c = i;
    if (c >= 0) { t->cur_core = c; t->core_since = now_ns(); g_core_owner[c] = t; return; }
    t->cseq = ++g_wseq; t->cnext = g_core_wait; g_core_wait = t;
    for (int i = 0; i < NCORE; i++) if (core_allowed(t, i) && g_core_owner[i] && g_core_owner[i]->prio > t->prio) g_core_owner[i]->preempt = 1;
    uint64_t q0 = now_ns();
    while (t->cur_core < 0) tw_wait(t, 0);
    hand_note(t, 1, now_ns() - q0);
}
// Local change (TOTK_LOCKSTAT=1): which guest mutexes the guest main thread waits on, who holds them, how long.
// Holding G. Logged every 5 s as "[totk] main lock waits".
static void main_lock_note(uint64_t addr, uint32_t owner, uint64_t lr, uint64_t ns) {
    static int on = -1; if (on < 0) on = getenv("TOTK_LOCKSTAT") != NULL; if (!on) return;
    enum { N = 16 };
    static struct { uint64_t addr, lr, ns, n; uint32_t owner; } e[N]; static uint64_t last;
    int i = 0; while (i < N && e[i].n && (e[i].addr != addr || e[i].owner != owner)) i++;
    if (i == N) i = N - 1;
    if (e[i].addr != addr || e[i].owner != owner) { e[i].addr = addr; e[i].owner = owner; e[i].ns = e[i].n = 0; }
    e[i].lr = lr; e[i].ns += ns; e[i].n++;
    uint64_t now = now_ns(); if (!last) last = now; if (now - last < 5000000000ull) return;
    double secs = (now - last) / 1e9; last = now;
    static uint64_t dump_want[8]; static int dump_n = -1;
    if (dump_n < 0) { dump_n = 0; const char *e = getenv("TOTK_LOCK_DUMP"); if (e) for (const char *p = e; p && *p && dump_n < 8; ) { char *q; uint64_t v = strtoull(p, &q, 0); if (q == p) break; if (v) dump_want[dump_n++] = v; p = *q == ',' ? q + 1 : q; } }
    for (int k = 0; k < N && e[k].n; k++) {
        if (dump_n && e[k].n) {   // one-shot dump of every listed address: allocators keep headers/links nearby; following the list in later dumps traces the owning pool
            uint64_t list[8]; int nlist = dump_n;
            for (int d = 0; d < nlist; d++) list[d] = dump_want[d];
            dump_n = 0;
            for (int d = 0; d < nlist; d++) {
                for (int64_t w = -24; w < 24; w++) {
                    uint64_t a = list[d] + w * 8;
                    uint32_t lo = __atomic_load_n((uint32_t *)a, __ATOMIC_SEQ_CST), hi = __atomic_load_n((uint32_t *)(a + 4), __ATOMIC_SEQ_CST);
                    fprintf(stderr, "[totk] lockdump %#llx %+3ld x0 %#llx\n", (unsigned long long)a, (long)w * 8, (unsigned long long)((uint64_t)hi << 32 | lo));
                }
            }
        }
        thr_t *o = NULL; for (thr_t *x = g_all; x; x = x->all) if (x->handle == (e[k].owner & ~WAITERS_BIT)) { o = x; break; }
        fprintf(stderr, "[totk] main lock waits: lock %#llx owner %#x (entry %#llx arg %#llx prio %d core %d host thread %llu) %.1f ms/s %llu waits/s, caller lr %#llx\n",
                (unsigned long long)e[k].addr, e[k].owner, o ? (unsigned long long)o->entry : 0ull, o ? (unsigned long long)o->arg : 0ull, o ? o->prio : -1, o ? o->core : -1, o ? (unsigned long long)o->tid : 0ull,
                e[k].ns / 1e6 / secs, (unsigned long long)(e[k].n / secs), (unsigned long long)e[k].lr);
        e[k].ns = e[k].n = 0;
    }
}
static thr_t *me_or_null(void) { return g_keythr ? pthread_getspecific(g_keythr) : NULL; }
void runtime_thread_note_hle(unsigned i) { thr_t *t = me_or_null(); if (t) t->last_hle = (int)i + 1; }   // hle_stack.c, while tracing
uint32_t runtime_thread_current_handle(void) { thr_t *t = me_or_null(); return t ? t->handle : 0; }
// Host services that wait for other threads or the GPU give the core up around the wait (no host locks held).
int runtime_core_away(void) {
    thr_t *t = me_or_null(); if (!t || t->cur_core < 0) return 0;
    pthread_mutex_lock(&G); core_put(t); pthread_mutex_unlock(&G);
    if (t == g_main_thr) t->wake_at = now_ns() | 1;   // reused as the away start: block_inner never runs while away
    return 1;
}
void runtime_core_back(int *away) {
    thr_t *t = me_or_null(); if (!*away || !t) return;
    if (t == g_main_thr && t->wake_at) { __atomic_fetch_add(&g_hand[12], now_ns() - t->wake_at, __ATOMIC_RELAXED); t->wake_at = 0; }
    pthread_mutex_lock(&G); core_take(t); pthread_mutex_unlock(&G);
}
// Preemption point at SVC entry: a higher-priority thread waits for this core.
void runtime_core_checkpoint(void) {
    thr_t *t = me_or_null(); if (!t || !t->preempt) return;
    pthread_mutex_lock(&G); t->preempt = 0; core_put(t); core_take(t); pthread_mutex_unlock(&G);
}
int runtime_current_core(void) { thr_t *t = me_or_null(); return t ? (t->cur_core >= 0 ? t->cur_core : t->core) : -1; }

// Local change (TOTK_WAITSTAT=<thread handle>, e.g. 0x1004): what one guest thread waits on (kind, address, the thread
// that woke it, the guest caller) and for how long. Holding G. Logged every 5 s as "[totk] waits of".
/* TOTK_WAITSTAT: a thread handle (0x1004), a comma list of handles, or "all" (every guest thread). Each watched thread
   keeps its own table (local change, 2026-10-19: it was one thread with one shared table). Holding G. */
enum { WS_THREADS = 48, WS_N = 24 };
typedef struct { uint64_t addr, lr, ns, n; uint32_t by; int kind; } ws_ent;
static struct { uint32_t handle; uint64_t last; ws_ent e[WS_N]; } g_ws[WS_THREADS];
static int g_ws_all; static uint32_t g_ws_list[8]; static int g_ws_nlist = -1;
static int ws_watched(uint32_t h) {
    if (g_ws_nlist < 0) {
        const char *e = getenv("TOTK_WAITSTAT"); g_ws_nlist = 0;
        if (e && !strcmp(e, "all")) g_ws_all = 1;
        else for (const char *p = e; p && *p && g_ws_nlist < 8; ) { char *q; uint32_t v = (uint32_t)strtoul(p, &q, 0); if (q == p) break; if (v) g_ws_list[g_ws_nlist++] = v; p = *q == ',' ? q + 1 : q; }
    }
    if (g_ws_all) return 1;
    for (int i = 0; i < g_ws_nlist; i++) if (g_ws_list[i] == h) return 1;
    return 0;
}
static void ws_note(thr_t *t, int kind, uint64_t addr, uint64_t ns) {
    int w = 0; while (w < WS_THREADS && g_ws[w].handle && g_ws[w].handle != t->handle) w++;
    if (w == WS_THREADS) return;
    g_ws[w].handle = t->handle; ws_ent *e = g_ws[w].e;
    uint32_t by = t->woke_by; int i = 0;
    while (i < WS_N && e[i].n && (e[i].addr != addr || e[i].by != by || e[i].kind != kind)) i++;
    if (i == WS_N) i = WS_N - 1;
    if (e[i].addr != addr || e[i].by != by || e[i].kind != kind) { e[i].addr = addr; e[i].by = by; e[i].kind = kind; e[i].ns = e[i].n = 0; }
    e[i].lr = t->last_lr; e[i].ns += ns; e[i].n++;
    uint64_t now = now_ns(); if (!g_ws[w].last) g_ws[w].last = now; if (now - g_ws[w].last < 5000000000ull) return;
    double secs = (now - g_ws[w].last) / 1e9; g_ws[w].last = now;
    for (int k = 0; k < WS_N && e[k].n; k++) {
        if (e[k].ns / secs > 1e6)
            fprintf(stderr, "[totk] waits of %#x (host thread %llu): kind %d addr %#llx woken by %#x %.1f ms/s %.0f waits/s, caller lr %#llx\n", t->handle, (unsigned long long)t->tid, e[k].kind,
                    (unsigned long long)e[k].addr, e[k].by, e[k].ns / 1e6 / secs, e[k].n / secs, (unsigned long long)e[k].lr);
        e[k].ns = e[k].n = 0;
    }
}
extern int totk_tl_on(void) __attribute__((weak_import));   // third_party/.../totk_timeline.c (TOTK_TIMELINE)
extern void totk_tl_at(int kind, uint64_t t_ns, uint64_t a, uint64_t b) __attribute__((weak_import));
static void tl_gblock(thr_t *t, int kind, uint64_t t0, uint32_t waker) {
    if (totk_tl_on && totk_tl_on()) totk_tl_at(17, t0, now_ns() - t0, (uint64_t)kind | (uint64_t)(t->handle & 0xffffff) << 8 | (uint64_t)waker << 32);
}
/* Local change (2026-10-19): guest waits that end on their own timeout (>= 10 ms asked), by wait kind, timeout, and the
   first sdk and main return addresses on the guest stack. The job-system stalls behind the play-time hitches last a
   near-constant ~110 ms at 80 and at 30 FPS; this found none, and the long-wait log below found the job thread they
   waited for (it was in the GPU write-back of mtl_pool_invalidate). TOTK_WAITLOG=1; logged every 5 s as
   "[totk] timed-out waits" / "[totk] long waits". Holding G (the table needs no lock of its own). */
enum { TO_N = 32 };
typedef struct { int kind; uint32_t ms; uint32_t handle; uint64_t sdk_off, main_off; uint64_t n, ns; uint32_t by, woken; } to_ent;
static to_ent g_to[TO_N]; static int g_to_used; static uint64_t g_to_last;
/* loader.c defines it in the game; a probe linked without the loader gets this fallback (no guest frames named) */
__attribute__((weak)) int runtime_module_offset(uint64_t pc, const char **name, uint64_t *off) { (void)pc; (void)name; (void)off; return 0; }
/* woken = 1 (TOTK, 2026-10-19): a wait of >= 50 ms that ended because another thread woke it (by = that thread's handle);
   recorded with the waker so a stalled job shows whom it waited for. ms is then the wait's own length in 10 ms steps. */
static int waitlog_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_WAITLOG"); v = e && atoi(e); } return v; }   /* TOTK_WAITLOG=1: the two logs above */
static void timeout_note(thr_t *t, int kind, int64_t ns, uint64_t waited, int woken, uint32_t by) {
    uint64_t sdk_off = 0, main_off = 0; uint64_t *fp = (uint64_t *)__builtin_frame_address(0);
    for (int i = 0; i < 32 && fp && !((uintptr_t)fp & 7) && !main_off; i++) {
        const char *nm; uint64_t off;
        if (runtime_module_offset(fp[1], &nm, &off)) { if (!strcmp(nm, "main")) main_off = off; else if (!sdk_off) sdk_off = off | (uint64_t)(nm[0] == 's' && nm[1] == 'u') << 63; }
        if ((uint64_t *)fp[0] <= fp) break; fp = (uint64_t *)fp[0];
    }
    uint32_t ms = woken ? (uint32_t)(waited / 10000000) * 10 : (uint32_t)(ns / 1000000); int k = 0;
    for (; k < g_to_used; k++) if (g_to[k].kind == kind && g_to[k].ms == ms && g_to[k].main_off == main_off && g_to[k].sdk_off == sdk_off && g_to[k].woken == (uint32_t)woken && g_to[k].by == by && g_to[k].handle == t->handle) break;
    if (k == g_to_used) {
        if (g_to_used == TO_N) { int z = -1; for (int i = 0; i < TO_N; i++) if (!g_to[i].n) { z = i; break; } if (z < 0) return; k = z; }   // reuse a slot emptied by the last report
        else k = g_to_used++;
        g_to[k] = (to_ent){kind, ms, t->handle, sdk_off, main_off, 0, 0, by, (uint32_t)woken};
    }
    g_to[k].n++; g_to[k].ns += waited;
    uint64_t now = now_ns(); if (!g_to_last) g_to_last = now;
    if (now - g_to_last < 5000000000ull) return;
    g_to_last = now;
    static const char *kinds[] = {"?", "lock", "condvar", "address", "sync", "?", "?", "?"};
    for (int r = 0; r < 12; r++) {
        int b = -1; for (int i = 0; i < g_to_used; i++) if (g_to[i].n && (b < 0 || g_to[i].ns > g_to[b].ns)) b = i;
        if (b < 0) break;
        if (g_to[b].woken)
            fprintf(stderr, "[totk] long waits (5 s): %llu x %s wait of %u-%u ms on thread %#x woken by %#x, %.1f ms each, from %s+%#llx / main+%#llx\n",
                    (unsigned long long)g_to[b].n, kinds[g_to[b].kind & 7], g_to[b].ms, g_to[b].ms + 10, g_to[b].handle, g_to[b].by, g_to[b].ns / 1e6 / (double)g_to[b].n,
                    g_to[b].sdk_off >> 63 ? "subsdk0" : "sdk", (unsigned long long)(g_to[b].sdk_off & ~(1ull << 63)), (unsigned long long)g_to[b].main_off);
        else
            fprintf(stderr, "[totk] timed-out waits (5 s): %llu x %s wait of %u ms on thread %#x, %.1f ms each, from %s+%#llx / main+%#llx\n",
                    (unsigned long long)g_to[b].n, kinds[g_to[b].kind & 7], g_to[b].ms, g_to[b].handle, g_to[b].ns / 1e6 / (double)g_to[b].n,
                    g_to[b].sdk_off >> 63 ? "subsdk0" : "sdk", (unsigned long long)(g_to[b].sdk_off & ~(1ull << 63)), (unsigned long long)g_to[b].main_off);
        g_to[b].n = 0; g_to[b].ns = 0;
    }
}
static int64_t block(thr_t *t,int kind,int64_t ns) {
    if (ns == 0) return block_inner(t, kind, ns);
    uint64_t to0 = now_ns();
    uint64_t b0 = t == g_main_thr && kind >= 1 && kind <= 4 ? now_ns() : 0;
    uint64_t w0 = ws_watched(t->handle) ? now_ns() : 0, wa = kind == 2 ? t->wkey : t->waddr;
    t->woke_by = 0;
    uint64_t tl0 = now_ns();
    core_put(t); __atomic_store_n(&t->wait_t0, to0, __ATOMIC_RELAXED);
    int64_t r = block_inner(t, kind, ns); __atomic_store_n(&t->wait_t0, 0, __ATOMIC_RELAXED);
    if (waitlog_on()) { uint64_t waited = now_ns() - to0;
      if (r == R_TIMEOUT) { if (ns >= 10000000) timeout_note(t, kind, ns, waited, 0, 0); }
      else if (waited >= 50000000ull) timeout_note(t, kind, ns, waited, 1, t->woke_by); }
    if (b0) __atomic_fetch_add(&g_hand[7 + kind], now_ns() - b0, __ATOMIC_RELAXED);
    if (w0) ws_note(t, kind, wa, now_ns() - w0);
    tl_gblock(t, kind, tl0, t->woke_by);
    uint64_t c0 = now_ns(); core_take(t); if (now_ns() - c0 > 20000) tl_gblock(t, 6, c0, 0);
    return r;
}
static int64_t block_inner(thr_t *t, int kind, int64_t ns) {
    t->wkind = kind; t->woke = 0; t->next = g_wait; g_wait = t; t->wseq = ++g_wseq;
    if (ns == 0) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    int det_wait = ns > 0 && dm_detclock_enabled();
    uint64_t guest_deadline = det_wait ? guest_ns() + (uint64_t)ns : 0;
    uint64_t deadline = ns > 0 && !det_wait ? now_ns() + (uint64_t)ns : 0;
    while (!t->woke) {
        int rc;
        if (det_wait) {
            uint64_t now = guest_ns();
            if (now >= guest_deadline) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
            (void)(guest_deadline - now);
            rc = tw_wait(t, 0);
        } else rc = tw_wait(t, deadline);
        if (rc == ETIMEDOUT && !t->woke && (!det_wait || guest_ns() >= guest_deadline)) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    }
    if (t->wake_at) { uint64_t n = now_ns(); hand_note(t, 0, n > t->wake_at ? n - t->wake_at : 0); t->wake_at = 0; }
    return t->wres;
}

// ---------- 锁 ----------
// ---------- Locks ----------
static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)a, __ATOMIC_SEQ_CST); }
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)a, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t a, uint32_t *exp, uint32_t v) { return __atomic_compare_exchange_n((uint32_t *)a, exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }

// 释放 addr 处的锁：交给等在它上面的第一个线程，或清零（持 G）
// Release the lock at addr: hand it to the first thread waiting on it, or clear it to zero (holding G)
static void lock_handoff(uint64_t addr) {
    thr_t *w = pick(1, addr); int more = 0;
    if (!w) { st32(addr, 0); return; }
    for (thr_t *t = g_wait; t; t = t->next) if (t != w && t->wkind == 1 && t->waddr == addr) { more = 1; break; }
    st32(addr, w->wtag | (more ? WAITERS_BIT : 0));
    wake(w, 0);
}
// 让 t 去拿 addr 处的锁：空闲就直接拿到并唤醒，否则置等待者位并转为锁等待（持 G）
// Make t take the lock at addr: if free, it gets it directly and is woken, otherwise set the waiters bit and turn into a lock wait (holding G)
static void lock_acquire_for(thr_t *t, uint64_t addr, uint32_t tag) {
    uint32_t v = ld32(addr);
    for (;;) {
        if (v == 0) { if (cas32(addr, &v, tag)) { wake(t, 0); return; } continue; }
        if (cas32(addr, &v, v | WAITERS_BIT)) break;
    }
    t->wkind = 1; t->waddr = addr; t->wtag = tag;      // 仍在 g_wait 里，改为锁等待 | Still in g_wait, changed to a lock wait
    t->wseq = ++g_wseq;                                  // 内核把它新插进锁持有者的等待队列：同优先级排在已有锁等待者之后 | The kernel inserts it anew into the lock owner's wait queue: at equal priority it goes after the existing lock waiters
}

// ---------- 线程 ----------
// ---------- Threads ----------
extern void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp,uint64_t *host_context);
extern _Noreturn void runtime_thread_leave(const uint64_t *host_context,void *arg,void(*fn)(void *));   // entry.S
uint32_t (*dm_thread_tick_fn)(void);   // 取当前游戏拍；diag.c 初始化时登记（单测不登记，拍号记 0） | Gets the current game tick; registered when diag.c initializes (unit tests do not register it, the tick number is recorded as 0)
void crash_thread_init(void);
void guest_thread_init(void);   // thread.c：每线程 TSD 槽（门、x18、TLS、TLR）唯一设置点 | thread.c: the single place that sets the per-thread TSD slots (gate, x18, TLS, TLR)
__attribute__((weak)) void runtime_hle_stack_set(uintptr_t top) { (void)top; }   // standalone thread probe: no host-stack switch
static void *thread_main(void *p) {
    thr_t *t = p;
    pthread_setspecific(g_keythr, t);
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0;
    pthread_get_qos_class_np(pthread_self(), &qc, &rp); t->qos = (uint32_t)qc;
    t->mport = pthread_mach_thread_np(pthread_self()); pthread_threadid_np(NULL, &t->tid);
    if (getenv("TOTK_WAITSTAT") || getenv("TOTK_LOCKSTAT")) fprintf(stderr, "[totk] guest thread %#x = host thread %llu (entry %#llx arg %#llx prio %d core %d)\n", t->handle, (unsigned long long)t->tid, (unsigned long long)t->entry, (unsigned long long)t->arg, t->prio, t->core);

    guest_thread_init();
    pthread_mutex_lock(&G); while (!t->started) tw_wait(t, 0); core_take(t); pthread_mutex_unlock(&G);
    { volatile char marker = 0; runtime_hle_stack_set((uintptr_t)&marker - 1024); }   // host services of this thread run below this frame
    dm_thread_enter(t->entry, t->arg, t->stack_top,t->host_context);   // 不返回：客体须经 ExitThread 退出 | Does not return: the guest must exit through ExitThread
    die("客体线程入口返回");
}
static void thr_free(thr_t *t) {   // 持 G；从全部线程链摘下并释放 | Holding G; unlink from the all-threads chain and free
    for (thr_t **p = &g_all; *p; p = &(*p)->all) if (*p == t) { *p = t->all; break; }
    pthread_cond_destroy(&t->cv); free(t);
}

static _Noreturn void thread_exit_on_host(void *argument) {
    thr_t *t=argument;
    t->exit_ns = now_ns(); t->exit_tick = dm_thread_tick_fn ? dm_thread_tick_fn() : 0;
    // 这里不打日志：退出发生在客体线程自己的栈上（有的只有十几 KiB），带浮点的 printf 会把栈撑爆（真机冒烟：LibcurlResolver 退出时 SIGBUS，回溯 fprintf → __dtoa）。时刻只记下来，现场导出时再打
    // No logging here: the exit happens on the guest thread's own stack (some are only a dozen or so KiB), and a printf with floating point would blow the stack (real-device smoke test: SIGBUS when LibcurlResolver exits, backtrace fprintf → __dtoa). Only the time is recorded, and it is printed later in the state dump
    pthread_mutex_lock(&G);
    core_put(t);
    t->exited = 1;
    for (thr_t *w = g_wait; w; ) { thr_t *n = w->next; if (w->wkind == 4) wake(w, -1); w = n; }   // 同步等待者重查 | Synchronization waiters re-check
    pthread_setspecific(g_keythr, NULL);
    if (t->closed) thr_free(t);
    pthread_mutex_unlock(&G);
    pthread_exit(NULL);
}
static _Noreturn void thread_exit(thr_t *t) {
 if(t->host_context[0])runtime_thread_leave(t->host_context,t,thread_exit_on_host);
 thread_exit_on_host(t);
}
void dm_thread_returned(void) { die("客体线程入口函数返回（Horizon 要求 ExitThread）"); }

// 主线程登记，返回其句柄（交给 nnosInitialize 作主线程句柄）
// Register the main thread and return its handle (passed to nnosInitialize as the main thread handle)
uint32_t svc_thread_main_init(void) {
    pthread_key_create(&g_keythr, NULL);
    thr_t *t = calloc(1, sizeof *t);
    pthread_cond_init(&t->cv, NULL);
    t->started = 1; t->prio = 44; t->core = 0; t->mask = 7; t->pt = pthread_self(); t->mport = pthread_mach_thread_np(t->pt); pthread_threadid_np(NULL, &t->tid);
    t->cur_core = -1;
    pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; core_take(t); g_main_thr = t; pthread_mutex_unlock(&G);
    if (getenv("TOTK_WAITSTAT") || getenv("TOTK_LOCKSTAT")) fprintf(stderr, "[totk] guest main thread %#x = host thread %llu\n", t->handle, (unsigned long long)t->tid);
    pthread_setspecific(g_keythr, t);
    qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0;
    pthread_get_qos_class_np(pthread_self(), &qc, &rp); t->qos = (uint32_t)qc;
    return t->handle;
}

static _Atomic uint64_t g_main_yield_n, g_main_yield_ns;   // 只有客体主线程写；诊断行在同一线程读 | Written only by the guest main thread; the diagnostic line reads it on the same thread
void dm_main_yield_stats(uint64_t *n, uint64_t *ns) { *n = g_main_yield_n; *ns = g_main_yield_ns; }
uint32_t dm_guest_work_qos(unsigned *count) {
    uint32_t qos = QOS_CLASS_UNSPECIFIED; unsigned n = 0;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t; t = t->all) {
        if (t->handle == 0x1001) continue;
        if (!n) qos = t->qos; else if (qos != t->qos) qos = UINT32_MAX;
        n++;
    }
    pthread_mutex_unlock(&G);
    if (count) *count = n;
    return qos;
}

// ---------- 同步对象 ----------
// ---------- Synchronization objects ----------
static int sig_obj(uint32_t h, int *bad) {    // 1=已触发（持 G） | 1 = signaled (holding G)
    unsigned i = h - 0x1000;
    if (h == H_CUR_THREAD) return 0;
    if (i == 0 || i >= MAX_H) { *bad = 1; return 0; }
    switch (g_h[i].type) {
    case O_THREAD: return ((thr_t *)g_h[i].obj)->exited;
    case O_EVENT_R: return ((event_t *)g_h[i].obj)->signaled;
    default: *bad = 1; return 0;
    }
}
static void wake_sync_waiters(void) {
    for (thr_t *w = g_wait; w; ) { thr_t *n = w->next; if (w->wkind == 4) wake(w, -1); w = n; }
}

static int64_t now_mono_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000000000ll + ts.tv_nsec; }
void dm_guest_clock_advanced(void) {
    if (!dm_detclock_enabled()) return;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_wait; t; t = t->next) if (t->wkind) tw_signal(t);
    pthread_mutex_unlock(&G);
}

// 处理线程 / 同步类 SVC；返回 1 = 已处理
// Handle thread / synchronization SVCs; returns 1 = handled
int svc_thread(int num, uint64_t *regs) {
    { thr_t *me = (g_keythr ? pthread_getspecific(g_keythr) : NULL); if (me) { me->last_svc = num; me->last_lr = regs[30]; me->last_fp = regs[29]; me->nsvc++; } }

    switch (num) {
    case 0x08: {  // CreateThread(→w1 句柄; x1 入口, x2 参数, x3 栈顶, w4 优先级, w5 核) | CreateThread(→w1 handle; x1 entry, x2 argument, x3 stack top, w4 priority, w5 core)
        if(!regs[1]||!regs[3]||(regs[3]&15)||(int)regs[4]<28||(int)regs[4]>59||(int)regs[5]<-3||(int)regs[5]>2){regs[0]=R_BAD_STATE;return 1;}
        thr_t *t = calloc(1, sizeof *t);
        if(!t)die("thread allocation");
        t->cur_core = -1;
        pthread_cond_init(&t->cv, NULL);
        t->entry = regs[1]; t->arg = regs[2]; t->stack_top = regs[3] & ~0xFull;
        t->prio = (int)regs[4]; t->core = (int)regs[5]; t->mask = t->core >= 0 ? 1ull << t->core : 7;
        t->born_ns = now_ns(); t->born_tick = dm_thread_tick_fn ? dm_thread_tick_fn() : 0;
        pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; pthread_mutex_unlock(&G);
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8 << 20);   // also the host-service stack of this guest thread
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);   // 退出即回收宿主资源（客体经句柄等待，不 join） | Host resources are reclaimed on exit (the guest waits through the handle, no join)
        if (pthread_create(&t->pt, &a, thread_main, t)) die("pthread_create");
        pthread_attr_destroy(&a);
        LOG("CreateThread h=%#x entry=%#llx sp=%#llx", t->handle,t->entry,t->stack_top);
        regs[0] = 0; regs[1] = t->handle; return 1;
    }
    case 0x09: {  // StartThread(w0)
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { t->started = 1; tw_signal(t); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x0A: thread_exit(cur()); return 1;
    case 0x0B: {  // SleepThread(x0 ns)；0/-1/-2 = 各类让出 | SleepThread(x0 ns); 0/-1/-2 = the various kinds of yield
        int64_t ns = (int64_t)regs[0];

        thr_t *self = cur(); uint64_t sl0 = now_ns();
        if (ns > 0) { pthread_mutex_lock(&G); core_put(self); pthread_mutex_unlock(&G); }
        if (ns > 0 && dm_detclock_enabled()) {
            uint64_t deadline = guest_ns() + (uint64_t)ns;
            while (guest_ns() < deadline) { struct timespec ts = {0, 1000000}; nanosleep(&ts, 0); }
        } else if (ns > 0) { struct timespec ts = {ns / 1000000000, ns % 1000000000}; nanosleep(&ts, 0); }
        else {   // 让出（0 / -1 / -2）。主线程的让出次数与耗时记账：游戏渲染任务图「等全部做完」是纯轮询 + 让出（main+0x15ee898），量它占一帧多少 | Yield (0 / -1 / -2). Count and time of the main thread's yields are recorded: the game's render task graph "wait for everything to finish" is pure polling + yield (main+0x15ee898); measure how much of a frame it takes
            thr_t *me = (g_keythr ? pthread_getspecific(g_keythr) : NULL);
            uint64_t y0 = now_ns();
            pthread_mutex_lock(&G);   // Horizon yield: hand the core to a waiting thread of equal or higher priority
            int give = 0; for (thr_t *w = g_core_wait; w && !give; w = w->cnext) give = me->cur_core >= 0 && core_allowed(w, me->cur_core) && w->prio <= me->prio;
            if (give) { core_put(me); core_take(me); }
            pthread_mutex_unlock(&G);
            /* Local change: with no guest thread waiting for this core, Horizon's yield returns at once. sched_yield gave the
               host CPU away instead, for up to 10 ms on macOS and possibly to an efficiency core, inside the game's
               poll-and-yield loops (the render task graph's "wait until all done", main+0x15ee898). A spin hint keeps the
               thread on its core; TOTK_HOST_YIELD=1 restores sched_yield. */
            static int host_yield = -1; if (host_yield < 0) { const char *e = getenv("TOTK_HOST_YIELD"); host_yield = e && atoi(e); }
            if (!give) { if (host_yield) sched_yield(); else __builtin_arm_yield(); }
            if (me && me->handle == 0x1001) { g_main_yield_ns += now_ns() - y0; g_main_yield_n++; }
        }
        if (ns > 0) { pthread_mutex_lock(&G); core_take(self); pthread_mutex_unlock(&G); tl_gblock(self, 5, sl0, 0); }

        return 1;
    }
    case 0x0C: { pthread_mutex_lock(&G);thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)t->prio : 0; pthread_mutex_unlock(&G);return 1; }
    case 0x0D: { if(regs[1]<28||regs[1]>59){regs[0]=R_BAD_STATE;return 1;}pthread_mutex_lock(&G);thr_t *t = h_get((uint32_t)regs[0], O_THREAD); if (t) t->prio = (int)regs[1]; regs[0] = t ? 0 : R_BAD_HANDLE;pthread_mutex_unlock(&G); return 1; }
    case 0x0E: { pthread_mutex_lock(&G);thr_t *t = h_get((uint32_t)regs[2], O_THREAD);
                 regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)(uint32_t)t->core : 0; regs[2] = t ? t->mask : 0;pthread_mutex_unlock(&G); return 1; }
    case 0x0F: {  // SetThreadCoreMask(w0, w1 理想核, x2 掩码)：-1 = 用进程缺省、-2/-3 = 不改理想核；宿主只记账 | SetThreadCoreMask(w0, w1 ideal core, x2 mask): -1 = use the process default, -2/-3 = leave the ideal core unchanged; the host only keeps the bookkeeping
        pthread_mutex_lock(&G);thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { int c = (int)regs[1]; if (c >= 0) t->core = c; else if (c == -1) t->core = 0; if (regs[2]) t->mask = regs[2];
                 if (t == cur() && t->cur_core >= 0 && !core_allowed(t, t->cur_core)) { core_put(t); core_take(t); } }   // migrate off a core no longer allowed
        regs[0] = t ? 0 : R_BAD_HANDLE;pthread_mutex_unlock(&G); return 1;
    }
    case 0x10: { thr_t *t = cur(); regs[0] = (uint64_t)(uint32_t)(t->cur_core >= 0 ? t->cur_core : t->core); return 1; }   // GetCurrentProcessorNumber
    case 0x25: { pthread_mutex_lock(&G);thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? t->id : 0;pthread_mutex_unlock(&G); return 1; }

    case 0x1A: {  // ArbitrateLock(w0 持有者句柄, x1 地址, w2 本线程标签) | ArbitrateLock(w0 owner handle, x1 address, w2 this thread's tag)
        thr_t *t = cur(); uint64_t addr = regs[1];
        // Local change (TOTK_LOCK_SPIN=ns, 0 = off): when the value still shows the queued owner, spin for a bounded
        // window before queueing. The release usually beats a block→wake→core-handoff round trip (~12 µs at idle on the
        // recorder-queue mutex). G is not held here, so the spin needs no G gymnastics (entry invariant: the SVC
        // dispatcher does not hold G — the original code made the same fresh pthread_mutex_lock below). If the value
        // moves, this becomes the no-op return an ArbitrateLock gets for a value that changed under it; the userland
        // loop in the guest retries exactly like everywhere else.
        static int spin_ns = -1; if (spin_ns < 0) { const char *e = getenv("TOTK_LOCK_SPIN"); spin_ns = (int)(strtoul(e ? e : "0", 0, 0) / 1000); if (spin_ns > 1000) spin_ns = 1000; }
        if (spin_ns > 0 && ld32(addr) == ((uint32_t)regs[0] | WAITERS_BIT)) {
            uint64_t d = now_ns() + (uint64_t)spin_ns * 1000;
            while (now_ns() < d) { uint32_t v = ld32(addr); if (!(v & WAITERS_BIT) || (v & ~WAITERS_BIT) != (uint32_t)regs[0]) break; __builtin_arm_yield(); }
        }
        pthread_mutex_lock(&G);
        int64_t r = 0;
        if (ld32(addr) == ((uint32_t)regs[0] | WAITERS_BIT)) {
            uint64_t w0 = t == g_main_thr ? now_ns() : 0;
            t->waddr = addr; t->wtag = (uint32_t)regs[2]; r = block(t, 1, -1);
            if (w0) main_lock_note(addr, (uint32_t)regs[0], regs[30], now_ns() - w0);
        }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1B: pthread_mutex_lock(&G); lock_handoff(regs[0]); pthread_mutex_unlock(&G); regs[0] = 0; return 1;
    case 0x1C: {  // WaitProcessWideKeyAtomic(x0 锁地址, x1 条件键, w2 标签, x3 超时) | WaitProcessWideKeyAtomic(x0 lock address, x1 condition key, w2 tag, x3 timeout)
        thr_t *t = cur(); uint64_t addr = regs[0], key = regs[1];
        pthread_mutex_lock(&G);
        // 顺序要紧：先置条件键 = 1，再放互斥锁。SDK 的 InternalConditionVariable::Signal 在用户态先读条件键、为 0 就不发 svc 0x1D；
        // 用户态抢互斥锁也不经过 G。原先先 lock_handoff 后置键：放锁后、置键前，别的线程可在用户态抢到锁、改完条件、读到键 0 就不唤醒，
        // 本线程随后挂上去永远等不到（并行负载下资源表读写锁 main+0x320e5e0 卡死：读者计数 0、锁空闲、写者条件键 1、写者仍在等）。
        // 挂进等待链在 block 里、仍持 G：svc 0x1D 要取 G，不会落在这之前
        // The order matters: first set the condition key = 1, then release the mutex. The SDK's InternalConditionVariable::Signal first reads the condition key in user mode, and does not issue svc 0x1D if it is 0;
        // grabbing the mutex in user mode does not go through G either. It used to do lock_handoff first and set the key after: after the release and before the key was set, another thread could grab the lock in user mode, change the condition, read key 0 and not wake anyone,
        // and this thread would then link itself in and wait forever (under parallel load the resource table read-write lock main+0x320e5e0 deadlocked: reader count 0, lock free, writer condition key 1, writer still waiting).
        // Linking into the wait chain happens inside block, still holding G: svc 0x1D has to take G, so it cannot land before this
#ifdef DM_TEST_HOOKS
#define CV_TEST_HOOK() { extern void (*dm_test_cv_after_release)(void); if (dm_test_cv_after_release) dm_test_cv_after_release(); }   // 单测：紧跟放锁之后、挂链之前 | Unit test: right after releasing the lock, before linking into the wait chain
#else
#define CV_TEST_HOOK()
#endif
#ifndef DM_TEST_CV_OLD_ORDER
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
        lock_handoff(addr); CV_TEST_HOOK();
#else   // 仅单测 t_condvar 的负对照：还原修复前「先放锁、后置键」的顺序 | Negative control for the unit test t_condvar only: restores the pre-fix order "release the lock first, set the key after"
        lock_handoff(addr); CV_TEST_HOOK();
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
#endif
        int64_t r = block(t, 2, (int64_t)regs[3]);   // 超时：不重新拿锁（用户态自己拿） | Timeout: the lock is not re-acquired (user mode takes it itself)
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1D: {  // SignalProcessWideKey(x0 条件键, w1 个数，<=0 全部) | SignalProcessWideKey(x0 condition key, w1 count, <=0 all)
        uint64_t key = regs[0]; int cnt = (int)regs[1], n = 0;
        pthread_mutex_lock(&G);
        for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(2, key)); n++) lock_acquire_for(w, w->waddr, w->wtag);   // 按内核顺序；转为锁等待或被唤醒后不再是 kind 2 | In kernel order; once turned into a lock wait or woken it is no longer kind 2
        int left = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 2 && w->wkey == key) { left = 1; break; }
        if (!left) st32(key, 0);
        pthread_mutex_unlock(&G);
        regs[0] = 0; return 1;
    }

    case 0x34: {  // WaitForAddress(x0 地址, w1 类型, w2 值, x3 超时)：0 小于则等、1 小于则减一再等、2 相等则等 | WaitForAddress(x0 address, w1 type, w2 value, x3 timeout): 0 wait if less than, 1 decrement if less than then wait, 2 wait if equal
        thr_t *t = cur(); uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2];
        pthread_mutex_lock(&G);
        int64_t r; uint32_t v = ld32(addr);
        int ok = type == 2 ? (int32_t)v == val : (int32_t)v < val;
        if (ok && type == 1) { while (!cas32(addr, &v, v - 1)) if (!((int32_t)v < val)) { ok = 0; break; } }
        if (!ok) r = R_BAD_STATE;
        else { t->waddr = addr; r = block(t, 3, (int64_t)regs[3]); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x35: {  // SignalToAddress(x0 地址, w1 类型, w2 值, w3 个数)：0 唤醒、1 相等则加一并唤醒、2 相等则按等待数改值并唤醒 | SignalToAddress(x0 address, w1 type, w2 value, w3 count): 0 wake, 1 increment if equal and wake, 2 if equal modify the value by waiter count and wake
        uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2]; int cnt = (int)regs[3];
        pthread_mutex_lock(&G);
        int nw = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 3 && w->waddr == addr) nw++;
        int64_t r = 0;
        if (type != 0) {
            int32_t nv = val + 1;
            if (type == 2) {
                if (cnt <= 0) nv = nw > 0 ? val - 1 : val + 1;
                else nv = nw == 0 ? val + 1 : nw <= cnt ? val - 1 : val;
            }
            uint32_t e = (uint32_t)val;
            if (!cas32(addr, &e, (uint32_t)nv)) r = R_BAD_STATE;
        }
        if (!r) { int n = 0;
            for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(3, addr)); n++) wake(w, 0); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }

    case 0x45: {  // CreateEvent(→w1 写端, w2 读端) | CreateEvent(→w1 write end, w2 read end)
        event_t *e = calloc(1, sizeof *e); e->refs = 2;
        pthread_mutex_lock(&G); uint32_t hw = h_new(O_EVENT_W, e), hr = h_new(O_EVENT_R, e); pthread_mutex_unlock(&G);
        regs[0] = 0; regs[1] = hw; regs[2] = hr; return 1;
    }
    case 0x11: case 0x12: case 0x17: {  // SignalEvent(写端) / ClearEvent(任一端) / ResetSignal(读端，未触发报错) | SignalEvent(write end) / ClearEvent(either end) / ResetSignal(read end, error if not signaled)
        pthread_mutex_lock(&G);
        // SignalEvent 只认写端；ClearEvent 两端都认；ResetSignal 只认读端
        // SignalEvent accepts only the write end; ClearEvent accepts both ends; ResetSignal accepts only the read end
        uint32_t h = (uint32_t)regs[0];
        event_t *e = num == 0x11 ? h_peek(h, O_EVENT_W) : num == 0x17 ? h_peek(h, O_EVENT_R)
                   : (h_peek(h, O_EVENT_W) ? h_peek(h, O_EVENT_W) : h_peek(h, O_EVENT_R));
        int64_t r = 0;
        if (!e) { r = (num == 0x17 && h_peek(h, O_THREAD)) ? R_BAD_STATE : R_BAD_HANDLE;
                  if (r == R_BAD_HANDLE) LOG("事件 SVC %#x 句柄无效 %#x", num, h); }
        else if (num == 0x11) { e->signaled = 1; wake_sync_waiters(); }
        else if (num == 0x17 && !e->signaled) r = R_BAD_STATE;
        else e->signaled = 0;
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x18: {  // WaitSynchronization(→w1 下标; x1 句柄数组, w2 个数, x3 超时) | WaitSynchronization(→w1 index; x1 handle array, w2 count, x3 timeout)
        thr_t *t = cur(); const uint32_t *hs = (const uint32_t *)regs[1]; int n = (int)regs[2]; int64_t ns = (int64_t)regs[3];
        if(n<0||n>64||(!hs&&n)){regs[0]=R_BAD_STATE;return 1;}
        int64_t deadline = ns > 0 ? (dm_detclock_enabled() ? (int64_t)guest_ns() : now_mono_ns()) + ns : 0;
        pthread_mutex_lock(&G);
        t->nwh = n; for (int i = 0; i < n && i < 4; i++) t->wh[i] = hs[i];
        int64_t r; int idx = -1;
        for (;;) {
            if (t->cancel) { t->cancel = 0; r = R_CANCELLED; break; }
            int bad = 0;
            for (int i = 0; i < n && idx < 0; i++) if (sig_obj(hs[i], &bad)) idx = i;
            if (bad) { r = R_BAD_HANDLE; break; }
            if (idx >= 0) { r = 0; break; }
            int64_t left = ns < 0 ? -1 : ns == 0 ? 0 : deadline - (dm_detclock_enabled() ? (int64_t)guest_ns() : now_mono_ns());
            if (ns > 0 && left <= 0) { r = R_TIMEOUT; break; }
            if (block(t, 4, left) == R_TIMEOUT) { r = R_TIMEOUT; break; }
        }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; regs[1] = idx >= 0 ? (uint64_t)idx : 0; return 1;
    }
    case 0x19: {  // CancelSynchronization(w0 线程) | CancelSynchronization(w0 thread)
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { t->cancel = 1; if (t->wkind == 4) wake(t, -1); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x16: {  // CloseHandle(w0)：事件两端都关才释放；线程句柄关闭后槽位回收，对象在线程也退出后释放 | CloseHandle(w0): an event is freed only when both ends are closed; after a thread handle is closed the slot is recycled, and the object is freed once the thread has also exited
        pthread_mutex_lock(&G);
        unsigned i = (uint32_t)regs[0] - 0x1000;int valid=0;
        if (i > 0 && i < MAX_H && (g_h[i].type == O_EVENT_R || g_h[i].type == O_EVENT_W)) {
            valid=1;event_t *e = g_h[i].obj; if (--e->refs == 0) free(e);
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
        } else if (i > 0 && i < MAX_H && (g_h[i].type == O_SESSION || g_h[i].type == O_TMEM)) {
            valid=1;void *o = g_h[i].obj; int session = g_h[i].type == O_SESSION;
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
            pthread_mutex_unlock(&G);
            if (session) runtime_ipc_session_closed(o); else runtime_ipc_tmem_closed(o);
            pthread_mutex_lock(&G);
        } else if (i > 0 && i < MAX_H && g_h[i].type == O_THREAD) {
            valid=1;thr_t *t = g_h[i].obj;
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
            t->closed = 1;
            if (t->exited) thr_free(t);
        }
        pthread_mutex_unlock(&G);
        regs[0] = valid?0:R_BAD_HANDLE; return 1;
    }
    }
    return 0;
}

// ---------- 服务层的 SystemEvent ----------
// 真机：服务把内核事件的读端交给客户端，客体经 SDK nn::os 等待。宿主：在客体线程上调 SDK 自己的
// CreateSystemEvent(跨进程=true)，SDK 经 SVC 0x45 建出内核事件；记下写端句柄，之后任何线程（含宿主音频线程）都可触发。
// ---------- SystemEvent of the service layer ----------
// Real device: the service hands the read end of a kernel event to the client, and the guest waits on it through SDK nn::os. Host: on a guest thread, call the SDK's own
// CreateSystemEvent(inter-process=true), and the SDK creates the kernel event through SVC 0x45; the write-end handle is recorded, after which any thread (including the host audio thread) can signal it.
static struct { void *ev; uint32_t wh; } g_se[512];
static int g_nse;
static pthread_mutex_t g_se_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t se_find(void *ev) {
    for (int i = 0; i < g_nse; i++) if (g_se[i].ev == ev) return g_se[i].wh;
    return 0;
}
void hle_bind_system_event(void *ev, int autoclear) {      // 必须在客体线程上调（要执行 SDK 代码） | Must be called on a guest thread (it has to execute SDK code)
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    if (wh) return;
    if (!(g_keythr ? pthread_getspecific(g_keythr) : NULL)) die("hle_bind_system_event 须在客体线程上调用");
    static uint32_t (*create)(void *, int, int); static uint32_t (*getw)(const void *);
    if (!create) { create = guest_sym("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
                   getw = guest_sym("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE"); }
    uint32_t r = create(ev, autoclear, 1);
    if (r) die("CreateSystemEvent 失败 %#x", r);
    wh = getw(ev);
    pthread_mutex_lock(&g_se_lock);
    if (g_nse >= (int)(sizeof g_se / sizeof g_se[0])) die("SystemEvent 绑定表满");
    g_se[g_nse].ev = ev; g_se[g_nse++].wh = wh;
    pthread_mutex_unlock(&g_se_lock);
}
void hle_signal_system_event(void *ev) {   // 未绑定的按手动清除即时绑定（只能在客体线程上发生） | An unbound one is bound on the spot as manual-clear (can only happen on a guest thread)
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    if (!wh) { hle_bind_system_event(ev, 0); pthread_mutex_lock(&g_se_lock); wh = se_find(ev); pthread_mutex_unlock(&g_se_lock); }
    pthread_mutex_lock(&G);
    event_t *e = h_get(wh, O_EVENT_W);
    if (!e) die("SystemEvent 写端句柄失效 %#x", wh);
    e->signaled = 1; wake_sync_waiters();
    pthread_mutex_unlock(&G);
}

// ---------- IPC support (ipc.c): sessions, transfer memory and service-owned events ----------
// Weak fallbacks keep standalone thread tests linkable; the runtime links the real ipc.c versions.
__attribute__((weak)) void runtime_ipc_session_closed(void *s) { (void)s; die("IPC session closed without ipc.c"); }
__attribute__((weak)) void runtime_ipc_tmem_closed(void *t) { (void)t; die("transfer memory closed without ipc.c"); }
uint32_t runtime_handle_new(int session, void *obj) { pthread_mutex_lock(&G); uint32_t h = h_new(session ? O_SESSION : O_TMEM, obj); pthread_mutex_unlock(&G); return h; }
void *runtime_handle_get(uint32_t h, int session) { pthread_mutex_lock(&G); void *o = h_peek(h, session ? O_SESSION : O_TMEM); pthread_mutex_unlock(&G); return o; }
// Service-side event: returns the writable handle kept by the service.
uint32_t runtime_event_new(void) {
    event_t *e = calloc(1, sizeof *e); if (!e) die("event allocation"); e->refs = 1;
    pthread_mutex_lock(&G); uint32_t hw = h_new(O_EVENT_W, e); pthread_mutex_unlock(&G); return hw;
}
// Copy-handle transfer of the readable end: each call gives the client its own handle to the same event.
uint32_t runtime_event_readable(uint32_t hw) {
    pthread_mutex_lock(&G); event_t *e = h_get(hw, O_EVENT_W); if (!e) die("service event handle %#x", hw);
    e->refs++; uint32_t hr = h_new(O_EVENT_R, e); pthread_mutex_unlock(&G); return hr;
}
void runtime_event_signal(uint32_t hw) {
    pthread_mutex_lock(&G); event_t *e = h_get(hw, O_EVENT_W); if (!e) die("service event handle %#x", hw);
    e->signaled = 1; wake_sync_waiters(); pthread_mutex_unlock(&G);
}

// ---------- Diagnostics: TOTK_DUMP_AFTER=<seconds> prints every guest thread's wait state and guest return chain ----------
#include "mach_vm_compat.h"
#include <unistd.h>
__attribute__((weak)) void runtime_describe_address(uint64_t a, char *out, size_t n) { snprintf(out, n, "%#llx", (unsigned long long)a); }   // loader.c names modules
__attribute__((weak)) uint64_t runtime_account_lock_owner(void) { return 0; }   // account.c is not linked into the thread probe
__attribute__((weak)) void runtime_hle_describe(unsigned i, char *out, size_t n) { snprintf(out, n, "wrapper %u", i); }   // hle_stack.c names it
static void dump_chain(uint64_t lr, uint64_t fp) {
    char where[96]; runtime_describe_address(lr, where, sizeof where); fprintf(stderr, "    %s", where);
    for (int i = 0; i < 10 && fp && !(fp & 15); i++) {
        uint64_t f[2]; mach_vm_size_t got = 0;
        if (mach_vm_read_overwrite(mach_task_self(), fp, 16, (mach_vm_address_t)(uintptr_t)f, &got) != KERN_SUCCESS || got != 16) break;
        runtime_describe_address(f[1], where, sizeof where); fprintf(stderr, " <- %s", where);
        if (f[0] == fp) break; fp = f[0];   // frames may move between the guest and host stacks
    }
    fputc('\n', stderr);
}
static void dump_threads(unsigned secs);
static void *dump_after(void *arg) { unsigned secs = (unsigned)(uintptr_t)arg; sleep(secs); dump_threads(secs); return NULL; }
/* TOTK_LISTCHECK=<heap-relative object offset>: watch the two intrusive lists at +0x48/+0x60 of one game allocator
   object (seen corrupted after the startup logo) and dump all threads the moment a node's next link is null. */
uint64_t runtime_heap_base(void) __attribute__((weak));
uint64_t runtime_heap_base(void) { return 0; }
static void *list_check(void *arg) {
    uint64_t off = (uint64_t)(uintptr_t)arg;
    for (;;) {
        usleep(200);
        uint64_t base = runtime_heap_base(); if (!base) continue;
        const uint64_t obj = base + off;
        for (int l = 0; l < 2; l++) {
            uint64_t sentinel = obj + (l ? 0x60 : 0x48), head[2]; mach_vm_size_t got = 0;
            if (mach_vm_read_overwrite(mach_task_self(), sentinel, 16, (mach_vm_address_t)(uintptr_t)head, &got) != KERN_SUCCESS) continue;
            uint64_t node = head[1];
            for (int n = 0; n < 100000 && node && node != sentinel; n++) {
                uint64_t link[2];
                if (mach_vm_read_overwrite(mach_task_self(), node, 16, (mach_vm_address_t)(uintptr_t)link, &got) != KERN_SUCCESS) break;
                if (!link[1]) {
                    usleep(50); uint64_t again[2];
                    if (mach_vm_read_overwrite(mach_task_self(), node, 16, (mach_vm_address_t)(uintptr_t)again, &got) == KERN_SUCCESS && !again[1]) {
                        fprintf(stderr, "[totk-listcheck] list +%#x node %#llx has null next (prev %#llx)\n", l ? 0x60 : 0x48, (unsigned long long)node, (unsigned long long)again[0]);
                        dump_threads(0); return NULL;
                    }
                    break;
                }
                node = link[1];
            }
        }
    }
}
/* Local change (2026-10-19, TOTK_TIMELINE): at each frame end, every guest thread's CPU time (a, ns), and b = handle |
   wait kind << 32 | (current core + 1) << 40, as timeline event 18 ("tcpu"). Shows whether a thread that blocks nothing
   for 100 ms is computing or stuck in host code. */
void runtime_threads_tl_cpu(void) {
    if (!totk_tl_on || !totk_tl_on() || !totk_tl_at) return;
    uint64_t now = now_ns();
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t; t = t->all) {
        if (!t->mport || t->exited) continue;
        thread_basic_info_data_t bi; mach_msg_type_number_t cnt = THREAD_BASIC_INFO_COUNT;
        if (thread_info(t->mport, THREAD_BASIC_INFO, (thread_info_t)&bi, &cnt) != KERN_SUCCESS) continue;
        uint64_t cpu = ((uint64_t)bi.user_time.seconds + (uint64_t)bi.system_time.seconds) * 1000000000ull + ((uint64_t)bi.user_time.microseconds + (uint64_t)bi.system_time.microseconds) * 1000ull;
        totk_tl_at(18, now, cpu, (uint64_t)t->handle | (uint64_t)(t->wkind & 0xff) << 32 | (uint64_t)(t->cur_core + 1 & 0xff) << 40);
    }
    pthread_mutex_unlock(&G);
}
static void dump_threads(unsigned secs) {
    int locked = 0; for (int i = 0; i < 100 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(10000);
    fprintf(stderr, "[totk-dump] global thread lock %s\n", locked ? "acquired" : "HELD BY ANOTHER THREAD (dumping without it)");
    fprintf(stderr, "[totk-dump] guest threads after %u s\n", secs);
    { fprintf(stderr, "[totk-dump] account lock owner host tid %llu\n", (unsigned long long)runtime_account_lock_owner()); }
    for (int c = 0; c < NCORE; c++) fprintf(stderr, "[totk-dump] core %d owner %#x for %.1f ms\n", c, g_core_owner[c] ? g_core_owner[c]->handle : 0,
                                            g_core_owner[c] ? (now_ns() - g_core_owner[c]->core_since) / 1e6 : 0.0);
    for (thr_t *w = g_core_wait; w; w = w->cnext) fprintf(stderr, "[totk-dump] core waiter %#x prio %d mask %#llx ideal %d\n", w->handle, w->prio, (unsigned long long)w->mask, w->core);
    thr_t *run[64]; int nrun = 0;
    for (thr_t *t = g_all; t; t = t->all) {
        uint32_t word = 0; if (t->wkind == 1 && t->waddr) memcpy(&word, (const void *)(uintptr_t)t->waddr, 4);
        uint64_t htid = 0; if (t->pt) pthread_threadid_np(t->pt, &htid);
        fprintf(stderr, "[totk-dump] tid=%llu ", (unsigned long long)htid);
        fprintf(stderr, "core=%d mask=%#llx ", t->cur_core, (unsigned long long)t->mask);
        fprintf(stderr, "h=%#x prio=%d %s wait=%d addr=%#llx key=%#llx tag=%#x word=%#x last_svc=%#x nsvc=%llu\n", t->handle, t->prio,
                t->exited ? "exited" : t->started ? "run" : "new", t->wkind, (unsigned long long)t->waddr, (unsigned long long)t->wkey, t->wtag, word, t->last_svc, (unsigned long long)t->nsvc);
        if (t->wkind == 4) { fprintf(stderr, "    waiting on %d handle(s):", t->nwh); for (int i = 0; i < t->nwh && i < 4; i++) fprintf(stderr, " %#x", t->wh[i]); fputc('\n', stderr); }
        if (t->last_hle) { char nm[256]; runtime_hle_describe((unsigned)t->last_hle - 1, nm, sizeof nm); fprintf(stderr, "    last host call while tracing: %s\n", nm); }
        dump_chain(t->last_lr, t->last_fp);
        if (!t->wkind && !t->exited && t->mport && nrun < 64) run[nrun++] = t;
    }
    if (locked) pthread_mutex_unlock(&G);
    for (int i = 0; i < nrun; i++) { thr_t *t = run[i]; fprintf(stderr, "[totk-dump] live h=%#x\n", t->handle);
            for (int k = 0; k < 3; k++) {
                arm_thread_state64_t st; mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
                thread_suspend(t->mport);
                kern_return_t kr = thread_get_state(t->mport, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt);
                thread_resume(t->mport);
                if (kr != KERN_SUCCESS) break;
                char a[96], b[96]; runtime_describe_address(arm_thread_state64_get_pc(st), a, sizeof a); runtime_describe_address(arm_thread_state64_get_lr(st), b, sizeof b);
                fprintf(stderr, "    live pc=%s lr=%s x0=%#llx x1=%#llx x8=%#llx\n", a, b, (unsigned long long)st.__x[0], (unsigned long long)st.__x[1], (unsigned long long)st.__x[8]);
                dump_chain(arm_thread_state64_get_lr(st), arm_thread_state64_get_fp(st));
                usleep(20000);
            }
    }
}
void runtime_dump_threads(void) { dump_threads(0); }   // TOTK_INPUT_FILE "threads"
/* Local change (2026-10-19), TOTK_STALL_DUMP=<ms>: when the main guest thread has been in one guest wait for <ms>, every
   guest thread that is not in a guest wait (running guest code, or blocked in host code) is sampled once: its pc / lr and
   frame chain, guest and host frames named. Once per stall; at most 40 stalls. To see what a job thread that blocks
   nothing for ~100 ms while the game waits for it is doing. Registers are read with the thread suspended, then it is
   resumed before anything is named (naming can allocate). */
static void *stall_watch(void *arg) {
    uint64_t lim = (uint64_t)(uintptr_t)arg * 1000000ull, last = 0; int dumps = 0;
    for (;;) {
        usleep(5000);
        thr_t *m = g_main_thr; if (!m) continue;
        uint64_t t0 = __atomic_load_n(&m->wait_t0, __ATOMIC_RELAXED), now = now_ns();
        if (!t0 || t0 == last || now - t0 < lim) continue;
        last = t0; if (++dumps > 40) return NULL;
        fprintf(stderr, "[stall] main thread in a guest wait (kind %d) for %.1f ms; threads not in a guest wait:\n", m->wkind, (now - t0) / 1e6);
        for (thr_t *t = g_all; t; t = t->all) {
            if (t->exited || !t->started || !t->mport || t->wkind || __atomic_load_n(&t->wait_t0, __ATOMIC_RELAXED)) continue;
            arm_thread_state64_t st; mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
            thread_suspend(t->mport); kern_return_t kr = thread_get_state(t->mport, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt); thread_resume(t->mport);
            if (kr != KERN_SUCCESS) continue;
            char a[96], b[96]; runtime_describe_address(arm_thread_state64_get_pc(st), a, sizeof a); runtime_describe_address(arm_thread_state64_get_lr(st), b, sizeof b);
            fprintf(stderr, "[stall]   h=%#x core=%d pc=%s lr=%s\n", t->handle, t->cur_core, a, b);
            dump_chain(arm_thread_state64_get_lr(st), arm_thread_state64_get_fp(st));
        }
    }
    return NULL;
}
__attribute__((constructor)) static void dump_init(void) {
    { const char *sd = getenv("TOTK_STALL_DUMP"); if (sd && atoi(sd) > 0) { pthread_t th; pthread_create(&th, NULL, stall_watch, (void *)(uintptr_t)atoi(sd)); pthread_detach(th); } }
    const char *c = getenv("TOTK_LISTCHECK");
    if (c) { pthread_t th; pthread_create(&th, NULL, list_check, (void *)(uintptr_t)strtoull(c, NULL, 0)); pthread_detach(th); }
    const char *e = getenv("TOTK_DUMP_AFTER"); if (!e) return;
    pthread_t th; pthread_create(&th, NULL, dump_after, (void *)(uintptr_t)strtoul(e, NULL, 10)); pthread_detach(th);
}
