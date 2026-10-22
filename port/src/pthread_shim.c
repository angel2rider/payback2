/* pthread_shim.c — Bionic pthreads on top of host primitives + guest TLS.
 *
 * Key facts this file is built on (all verified on this machine):
 *   - TPIDR_EL0 is never written.  Darwin's libsystem_malloc reads it:
 *     _xzm_xzone_find_and_malloc_from_freelist_chunk hashes (TPIDR_EL0 >> 12)
 *     into its segment-group slot index (slot = (mix & 0xff) * 0x340).  With
 *     the tiny kernel value a normal process sees (0x1..0x1005) the hash lands
 *     in the initialized region; pointing it at a heap TCB gives a random
 *     slot, often an uninitialized zero struct -> group->[0x18] == NULL ->
 *     SIGSEGV inside malloc.  Nothing needs the register: all 519 guest
 *     tpidr_el0 reads are patched to the image canary, and errno/TSD ride
 *     host pthread TLS.
 *   - Bionic TCB slots:  0 pthread_internal_t*, 1 thread id, 2 errno,
 *     5 stack canary (byte 0x28); guest TCBs exist only as guest memory.
 *   - Bionic mutex/cond/rwlock/attr sizes and constant values differ from
 *     Darwin's; guest objects are opaque, so a side table maps guest address
 *     -> host primitive.
 *   - Bionic cond vars default to CLOCK_MONOTONIC deadlines; Darwin's
 *     pthread_cond_timedwait takes CLOCK_REALTIME absolute deadlines.
 *
 * Guest threads are REAL host threads running guest code directly (same ISA).
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "pb.h"

/* ------------------------------------------------------------------ TCB */
typedef struct {
    uint64_t self;      /* +0x00 pthread record pointer            */
    uint64_t tid;       /* +0x08                                   */
    int      err_v;     /* +0x10 errno                             */
    int      pad;
    uint64_t opengl;    /* +0x18                                   */
    uint64_t opengl_api;/* +0x20                                   */
    uint64_t canary;    /* +0x28 stack guard                       */
} guest_tcb;

typedef struct pthread_rec {
    pthread_t          host;
    uint64_t           guest_t;     /* what the guest sees       */
    guest_tcb         *tcb;
    int                detached;
    int                is_main;
    char               name[24];
    struct pthread_rec *next;
} pthread_rec;

static pthread_key_t g_self_key;      /* pthread_rec* per thread     */
static pthread_key_t g_once_key;      /* active once record          */
static pthread_once_t g_keys_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_rec *g_threads;
static uint64_t g_next_tid = 1;

static inline uint64_t rd_tp(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v));
    return v;
}

static uint64_t mk_canary(void)
{
    /* stable per-thread canary */
    static uint64_t seed;
    if (!seed) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        seed = ((uint64_t)ts.tv_nsec << 21) ^ (uint64_t)getpid() ^ 0x9E3779B97F4A7C15ull;
        seed |= 1;
    }
    return seed;
}

static guest_tcb *alloc_tcb(pthread_rec *rec)
{
    guest_tcb *t = aligned_alloc(16, sizeof(guest_tcb));
    memset(t, 0, sizeof *t);
    t->self = (uint64_t)(uintptr_t)rec;
    t->tid = g_next_tid++;
    t->canary = mk_canary();
    return t;
}

static int g_tls_ready;

static pthread_rec *current_rec(void)
{
    pthread_rec *r = g_tls_ready ? pthread_getspecific(g_self_key) : NULL;
    if (r) return r;
    if (!g_tls_ready) return NULL;

    /* lazily register the calling (main) thread */
    pthread_mutex_lock(&g_reg_lock);
    r = pthread_getspecific(g_self_key);
    if (!r) {
        r = calloc(1, sizeof *r);
        r->host = pthread_self();
        r->guest_t = (uint64_t)(uintptr_t)r;
        r->tcb = alloc_tcb(r);
        r->is_main = 1;
        snprintf(r->name, sizeof r->name, "main");
        r->next = g_threads;
        g_threads = r;
        pthread_setspecific(g_self_key, r);
    }
    pthread_mutex_unlock(&g_reg_lock);
    return r;
}

void pb_tls_init(void)
{
    pthread_key_create(&g_self_key, NULL);
    pthread_key_create(&g_once_key, NULL);
    g_tls_ready = 1;
    pthread_rec *r = current_rec();
    uint64_t after = rd_tp();
    if (getenv("PB_TRACE_SHIM"))
        fprintf(stderr, "[tls] self_key=%lu rec=%p tpidr_el0=%#llx (tcb %p)\n",
                (unsigned long)g_self_key, (void *)r, (unsigned long long)after,
                r ? (void *)r->tcb : NULL);
}

/* errno via the Bionic TCB slot.  The TCB pointer comes from *host* TLS
 * (pthread_getspecific rides TPIDRRO_EL0, which Darwin maintains), never
 * from TPIDR_EL0 - the kernel clobbers that with junk on every syscall. */
static int *tls_errno(void)
{
    pthread_rec *r = current_rec();
    if (!r || !r->tcb) return &errno;
    return &r->tcb->err_v;
}

static int *shim___errno(void) { return tls_errno(); }
int *pb_errno_ptr(void) { return tls_errno(); }

void pb_tls_check(const char *label)
{
    if (!getenv("PB_TLS_CHECK")) return;
    pthread_rec *r = current_rec();
    fprintf(stderr, "[tls] %-24s rec=%p tcb=%p (tpidr junk now %#llx)\n", label,
            (void *)r, r ? (void *)r->tcb : NULL, (unsigned long long)rd_tp());
}

/* ------------------------------------------------------- object tables */

enum { SY_MUTEX = 1, SY_COND, SY_RWLOCK, SY_ONCE, SY_ATTR, SY_MUTEXATTR };

typedef struct sync_rec {
    uint64_t addr;
    int kind;
    union {
        pthread_mutex_t mutex;
        pthread_cond_t cond;
        pthread_rwlock_t rwlock;
        struct {
            pthread_once_t once;
            void (*fn)(void);
        } once;
        struct {
            int detach;
            size_t stacksize;
        } attr;
        struct { int type; } mattr;
    };
    struct sync_rec *next;
} sync_rec;

static sync_rec *g_sync;
static pthread_mutex_t g_sync_lock = PTHREAD_MUTEX_INITIALIZER;

static sync_rec *sync_find(uint64_t addr, int kind)
{
    for (sync_rec *r = g_sync; r; r = r->next)
        if (r->addr == addr && r->kind == kind) return r;
    return NULL;
}

static sync_rec *sync_new(uint64_t addr, int kind)
{
    sync_rec *r = calloc(1, sizeof *r);
    r->addr = addr;
    r->kind = kind;
    r->next = g_sync;
    g_sync = r;
    return r;
}

/* ------------------------------------------------------------- mutex */

#define B_MUTEX_RECURSIVE 1
#define B_MUTEX_ERRORCHECK 2

static sync_rec *mutex_get(uint64_t m, int create)
{
    pthread_mutex_lock(&g_sync_lock);
    sync_rec *r = sync_find(m, SY_MUTEX);
    if (!r && create) {
        r = sync_new(m, SY_MUTEX);
        pthread_mutex_init(&r->mutex, NULL);
    }
    pthread_mutex_unlock(&g_sync_lock);
    return r;
}

static int sh_mutex_init(uint64_t m, uint64_t attr)
{
    int linux_type = 0;
    pthread_mutex_lock(&g_sync_lock);
    sync_rec *a = attr ? sync_find(attr, SY_MUTEXATTR) : NULL;
    if (a) linux_type = a->mattr.type;
    sync_rec *r = sync_find(m, SY_MUTEX);
    if (!r) r = sync_new(m, SY_MUTEX);
    else { pthread_mutex_destroy(&r->mutex); }
    int darwin_type = 0;
    if (linux_type == B_MUTEX_RECURSIVE) darwin_type = 2;        /* Darwin RECURSIVE */
    else if (linux_type == B_MUTEX_ERRORCHECK) darwin_type = 1;  /* Darwin ERRORCHECK */
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_settype(&ma, darwin_type);
    pthread_mutex_init(&r->mutex, &ma);
    pthread_mutexattr_destroy(&ma);
    pthread_mutex_unlock(&g_sync_lock);
    return 0;
}
static int sh_mutex_lock(uint64_t m)     { sync_rec *r = mutex_get(m, 1); return pthread_mutex_lock(&r->mutex); }
static int sh_mutex_trylock(uint64_t m)  { sync_rec *r = mutex_get(m, 1); return pthread_mutex_trylock(&r->mutex); }
static int sh_mutex_unlock(uint64_t m)   { sync_rec *r = mutex_get(m, 0); return r ? pthread_mutex_unlock(&r->mutex) : 0; }
static int sh_mutex_destroy(uint64_t m)  { sync_rec *r = mutex_get(m, 0); return r ? pthread_mutex_destroy(&r->mutex) : 0; }

static int sh_mutexattr_init(uint64_t a)
{
    pthread_mutex_lock(&g_sync_lock);
    if (!sync_find(a, SY_MUTEXATTR)) { sync_rec *r = sync_new(a, SY_MUTEXATTR); r->mattr.type = 0; }
    pthread_mutex_unlock(&g_sync_lock);
    return 0;
}
static int sh_mutexattr_settype(uint64_t a, int type)
{
    pthread_mutex_lock(&g_sync_lock);
    sync_rec *r = sync_find(a, SY_MUTEXATTR);
    if (!r) { r = sync_new(a, SY_MUTEXATTR); }
    r->mattr.type = type;
    pthread_mutex_unlock(&g_sync_lock);
    return 0;
}
static int sh_mutexattr_destroy(uint64_t a) { (void)a; return 0; }

/* -------------------------------------------------------------- cond */

static sync_rec *cond_get(uint64_t c)
{
    pthread_mutex_lock(&g_sync_lock);
    sync_rec *r = sync_find(c, SY_COND);
    if (!r) {
        r = sync_new(c, SY_COND);
        pthread_cond_init(&r->cond, NULL);
    }
    pthread_mutex_unlock(&g_sync_lock);
    return r;
}

static int sh_cond_init(uint64_t c, uint64_t attr)
{ (void)attr; cond_get(c); return 0; }
static int sh_cond_destroy(uint64_t c) { sync_rec *r = cond_get(c); return pthread_cond_destroy(&r->cond); }
static int sh_cond_signal(uint64_t c) { sync_rec *r = cond_get(c); return pthread_cond_signal(&r->cond); }
static int sh_cond_broadcast(uint64_t c) { sync_rec *r = cond_get(c); return pthread_cond_broadcast(&r->cond); }

static int sh_cond_wait(uint64_t c, uint64_t m)
{
    sync_rec *cr = cond_get(c);
    sync_rec *mr = mutex_get(m, 0);
    if (!mr) return 1;
    return pthread_cond_wait(&cr->cond, &mr->mutex);
}

static int sh_cond_timedwait(uint64_t c, uint64_t m, const struct timespec *abstime)
{
    sync_rec *cr = cond_get(c);
    sync_rec *mr = mutex_get(m, 0);
    if (!mr) return 1;

    /* guest deadline is CLOCK_MONOTONIC-based (Bionic default); convert */
    struct timespec mono_now, rt_now, rt_deadline;
    clock_gettime(CLOCK_MONOTONIC, &mono_now);
    clock_gettime(CLOCK_REALTIME, &rt_now);
    int64_t dsec = abstime->tv_sec - mono_now.tv_sec;
    int64_t dnsec = abstime->tv_nsec - mono_now.tv_nsec;
    rt_deadline.tv_sec = rt_now.tv_sec + dsec;
    rt_deadline.tv_nsec = rt_now.tv_nsec + dnsec;
    while (rt_deadline.tv_nsec >= 1000000000) { rt_deadline.tv_nsec -= 1000000000; rt_deadline.tv_sec++; }
    while (rt_deadline.tv_nsec < 0) { rt_deadline.tv_nsec += 1000000000; rt_deadline.tv_sec--; }

    int r = pthread_cond_timedwait(&cr->cond, &mr->mutex, &rt_deadline);
    if (r == ETIMEDOUT) return 110;      /* Bionic ETIMEDOUT */
    return r;
}

/* ------------------------------------------------------------ rwlock */

static sync_rec *rw_get(uint64_t w)
{
    pthread_mutex_lock(&g_sync_lock);
    sync_rec *r = sync_find(w, SY_RWLOCK);
    if (!r) {
        r = sync_new(w, SY_RWLOCK);
        pthread_rwlock_init(&r->rwlock, NULL);
    }
    pthread_mutex_unlock(&g_sync_lock);
    return r;
}
static int sh_rw_rdlock(uint64_t w) { return pthread_rwlock_rdlock(&rw_get(w)->rwlock); }
static int sh_rw_wrlock(uint64_t w) { return pthread_rwlock_wrlock(&rw_get(w)->rwlock); }
static int sh_rw_unlock(uint64_t w) { return pthread_rwlock_unlock(&rw_get(w)->rwlock); }

/* ------------------------------------------------------------- once */

static void once_tramp(void)
{
    sync_rec *r = pthread_getspecific(g_once_key);
    if (r && r->once.fn) r->once.fn();
}

static int sh_pthread_once(uint64_t o, void (*fn)(void))
{
    pthread_mutex_lock(&g_sync_lock);
    sync_rec *r = sync_find(o, SY_ONCE);
    if (!r) { r = sync_new(o, SY_ONCE); r->once.fn = fn; }
    pthread_mutex_unlock(&g_sync_lock);
    void *prev = pthread_getspecific(g_once_key);
    pthread_setspecific(g_once_key, r);
    int ret = pthread_once(&r->once.once, once_tramp);
    pthread_setspecific(g_once_key, prev);
    return ret;
}

/* -------------------------------------------------------- keys (map) */

#define GKEY_MAX 256
static struct { int used; pthread_key_t host; void (*dtor)(void *); } g_keys[GKEY_MAX];
static pthread_mutex_t g_keys_lock = PTHREAD_MUTEX_INITIALIZER;

static int sh_key_create(int *gk, void (*dtor)(void *))
{
    pthread_mutex_lock(&g_keys_lock);
    for (int i = 1; i < GKEY_MAX; i++) {
        if (!g_keys[i].used) {
            pthread_key_t hk;
            if (pthread_key_create(&hk, NULL) != 0) { pthread_mutex_unlock(&g_keys_lock); return 11; }
            g_keys[i].used = 1;
            g_keys[i].host = hk;
            g_keys[i].dtor = dtor;
            *gk = i;
            pthread_mutex_unlock(&g_keys_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_keys_lock);
    return 11;  /* EAGAIN */
}

static int sh_key_delete(int gk)
{
    if (gk <= 0 || gk >= GKEY_MAX) return 22;
    pthread_mutex_lock(&g_keys_lock);
    if (g_keys[gk].used) { pthread_key_delete(g_keys[gk].host); g_keys[gk].used = 0; }
    pthread_mutex_unlock(&g_keys_lock);
    return 0;
}

static int sh_setspecific(int gk, void *v)
{
    if (gk <= 0 || gk >= GKEY_MAX || !g_keys[gk].used) return 22;
    return pthread_setspecific(g_keys[gk].host, v);
}

static void *sh_getspecific(int gk)
{
    if (gk <= 0 || gk >= GKEY_MAX || !g_keys[gk].used) return NULL;
    return pthread_getspecific(g_keys[gk].host);
}

/* ----------------------------------------------------- thread create */

typedef struct {
    void *(*fn)(void *);
    void   *arg;
    pthread_rec *rec;
} start_pack;

static void *thread_tramp(void *p)
{
    start_pack *sp = p;
    pthread_rec *rec = sp->rec;
    pthread_setspecific(g_self_key, rec);

    if (getenv("PB_TRACE_THREADS"))
        fprintf(stderr, "[thr] start fn=0x%llx arg=0x%llx\n",
                (unsigned long long)(uintptr_t)sp->fn, (unsigned long long)(uintptr_t)sp->arg);

    void *r = sp->fn(sp->arg);

    /* run guest key destructors (Bionic behaviour) */
    for (int i = 1; i < GKEY_MAX; i++) {
        if (g_keys[i].used && g_keys[i].dtor) {
            void *v = pthread_getspecific(g_keys[i].host);
            if (v) { pthread_setspecific(g_keys[i].host, NULL); g_keys[i].dtor(v); }
        }
    }
    if (getenv("PB_TRACE_THREADS"))
        fprintf(stderr, "[thr] exit fn=0x%llx ret=0x%llx\n",
                (unsigned long long)(uintptr_t)sp->fn, (unsigned long long)(uintptr_t)r);
    free(sp);
    return r;
}

static int sh_pthread_create(uint64_t *t, uint64_t attr, void *(*fn)(void *), void *arg)
{
    pthread_rec *rec = calloc(1, sizeof *rec);
    rec->guest_t = (uint64_t)(uintptr_t)rec;
    rec->tcb = alloc_tcb(rec);
    snprintf(rec->name, sizeof rec->name, "guest-%llu", (unsigned long long)rec->tcb->tid);

    pthread_attr_t ha;
    pthread_attr_init(&ha);
    pthread_attr_setstacksize(&ha, 4u << 20);

    pthread_mutex_lock(&g_sync_lock);
    sync_rec *a = attr ? sync_find(attr, SY_ATTR) : NULL;
    int detach = a ? a->attr.detach : 0;
    pthread_mutex_unlock(&g_sync_lock);
    if (detach) pthread_attr_setdetachstate(&ha, PTHREAD_CREATE_DETACHED);
    rec->detached = detach;

    start_pack *sp = calloc(1, sizeof *sp);
    sp->fn = fn; sp->arg = arg; sp->rec = rec;

    pthread_mutex_lock(&g_reg_lock);
    rec->next = g_threads;
    g_threads = rec;
    pthread_mutex_unlock(&g_reg_lock);

    int r = pthread_create(&rec->host, &ha, thread_tramp, sp);
    pthread_attr_destroy(&ha);
    if (r) return r;
    if (t) *t = rec->guest_t;
    if (getenv("PB_TRACE_THREADS"))
        fprintf(stderr, "[thr] create fn=0x%llx arg=0x%llx -> t=0x%llx\n",
                (unsigned long long)(uintptr_t)fn, (unsigned long long)(uintptr_t)arg,
                (unsigned long long)rec->guest_t);
    return 0;
}

static pthread_rec *rec_lookup(uint64_t gt)
{
    pthread_mutex_lock(&g_reg_lock);
    pthread_rec *r = g_threads;
    while (r && r->guest_t != gt) r = r->next;
    pthread_mutex_unlock(&g_reg_lock);
    return r;
}

static int sh_pthread_join(uint64_t gt, void **retval)
{
    pthread_rec *r = rec_lookup(gt);
    if (!r) return 3;   /* ESRCH */
    if (getenv("PB_TRACE_THREADS"))
        fprintf(stderr, "[thr] join begin t=0x%llx (caller t=0x%llx)\n",
                (unsigned long long)gt, (unsigned long long)pb_cur_guest_thread());
    int rc = pthread_join(r->host, retval);
    if (getenv("PB_TRACE_THREADS"))
        fprintf(stderr, "[thr] join end   t=0x%llx rc=%d\n",
                (unsigned long long)gt, rc);
    return rc;
}

uint64_t pb_cur_guest_thread(void)
{
    pthread_rec *rec = pthread_getspecific(g_self_key);
    return rec ? rec->guest_t : 0;
}

static int sh_pthread_detach(uint64_t gt)
{
    pthread_rec *r = rec_lookup(gt);
    if (!r) return 3;
    r->detached = 1;
    return pthread_detach(r->host);
}

static uint64_t sh_pthread_self(void)
{
    pthread_rec *r = current_rec();
    return r ? r->guest_t : 0;
}

static int sh_attr_init(uint64_t a)
{
    pthread_mutex_lock(&g_sync_lock);
    if (!sync_find(a, SY_ATTR)) { sync_rec *r = sync_new(a, SY_ATTR); r->attr.detach = 0; }
    pthread_mutex_unlock(&g_sync_lock);
    return 0;
}
static int sh_attr_setdetachstate(uint64_t a, int st)
{
    pthread_mutex_lock(&g_sync_lock);
    sync_rec *r = sync_find(a, SY_ATTR);
    if (!r) { r = sync_new(a, SY_ATTR); }
    r->attr.detach = st;
    pthread_mutex_unlock(&g_sync_lock);
    return 0;
}
static int sh_getschedparam(uint64_t t, int *policy, void *param)
{
    (void)t;
    if (policy) *policy = 0;                        /* SCHED_OTHER */
    if (param) ((int *)param)[0] = 0;               /* sched_priority */
    return 0;
}

/* ------------------------------------------------------- registration */

void pb_pthread_register(void)
{
    pb_tls_init();

    pb_shim_register("__errno", (void *)shim___errno, false);
    pb_shim_register("pthread_create", (void *)sh_pthread_create, false);
    pb_shim_register("pthread_join", (void *)sh_pthread_join, false);
    pb_shim_register("pthread_detach", (void *)sh_pthread_detach, false);
    pb_shim_register("pthread_self", (void *)sh_pthread_self, false);
    pb_shim_register("pthread_attr_init", (void *)sh_attr_init, false);
    pb_shim_register("pthread_attr_setdetachstate", (void *)sh_attr_setdetachstate, false);
    pb_shim_register("pthread_getschedparam", (void *)sh_getschedparam, false);
    pb_shim_register("pthread_mutex_init", (void *)sh_mutex_init, false);
    pb_shim_register("pthread_mutex_lock", (void *)sh_mutex_lock, false);
    pb_shim_register("pthread_mutex_trylock", (void *)sh_mutex_trylock, false);
    pb_shim_register("pthread_mutex_unlock", (void *)sh_mutex_unlock, false);
    pb_shim_register("pthread_mutex_destroy", (void *)sh_mutex_destroy, false);
    pb_shim_register("pthread_mutexattr_init", (void *)sh_mutexattr_init, false);
    pb_shim_register("pthread_mutexattr_settype", (void *)sh_mutexattr_settype, false);
    pb_shim_register("pthread_mutexattr_destroy", (void *)sh_mutexattr_destroy, false);
    pb_shim_register("pthread_cond_init", (void *)sh_cond_init, false);
    pb_shim_register("pthread_cond_wait", (void *)sh_cond_wait, false);
    pb_shim_register("pthread_cond_timedwait", (void *)sh_cond_timedwait, false);
    pb_shim_register("pthread_cond_signal", (void *)sh_cond_signal, false);
    pb_shim_register("pthread_cond_broadcast", (void *)sh_cond_broadcast, false);
    pb_shim_register("pthread_cond_destroy", (void *)sh_cond_destroy, false);
    pb_shim_register("pthread_rwlock_rdlock", (void *)sh_rw_rdlock, false);
    pb_shim_register("pthread_rwlock_wrlock", (void *)sh_rw_wrlock, false);
    pb_shim_register("pthread_rwlock_unlock", (void *)sh_rw_unlock, false);
    pb_shim_register("pthread_once", (void *)sh_pthread_once, false);
    pb_shim_register("pthread_key_create", (void *)sh_key_create, false);
    pb_shim_register("pthread_key_delete", (void *)sh_key_delete, false);
    pb_shim_register("pthread_setspecific", (void *)sh_setspecific, false);
    pb_shim_register("pthread_getspecific", (void *)sh_getspecific, false);
}
