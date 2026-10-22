/* shims.c — import resolution: overrides first, then macOS libc via dlsym.
 *
 * Anything with subtle Bionic-vs-Darwin differences must be registered here
 * BEFORE pb_wire_imports() runs; everything else resolves straight to the
 * host symbol (AAPCS64 and Apple arm64 agree for non-variadic calls).
 */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "pb.h"

/* ------------------------------------------------------------------ log */
void pb_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------- missing-import stubs
 * Any import we cannot resolve gets a tiny ARM64 trampoline that names the
 * symbol and aborts, so a mis-wire is diagnosed in one run instead of a
 * pc=0 mystery.  The arena is a single anonymous executable page.
 */
static void pb_missing_import(uint64_t idx);

static struct { char name[96]; } g_missing_names[512];
static int  g_missing_n;
static uint32_t *g_stub_arena;
static int g_stub_arena_rx;

/* macOS refuses RWX anonymous mmap; write in RW, then flip to RX. */
static void stub_arena_prot(int rx)
{
    if (!g_stub_arena) return;
    if (rx == g_stub_arena_rx) return;
    if (mprotect(g_stub_arena, 0x4000, rx ? PROT_READ | PROT_EXEC : PROT_READ | PROT_WRITE) != 0)
        fprintf(stderr, "[missing] mprotect arena %d failed\n", rx);
    g_stub_arena_rx = rx;
}

static void *emit_missing_stub(int idx)
{
    if (!g_stub_arena) {
        g_stub_arena = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
        if (g_stub_arena == MAP_FAILED) {
            fprintf(stderr, "[missing] cannot allocate stub arena\n");
            abort();
        }
    }
    stub_arena_prot(0);
    uint32_t *p = g_stub_arena + (size_t)idx * 8;      /* 32-byte stride */
    uint64_t h = (uint64_t)(uintptr_t)pb_missing_import;
    p[0] = 0xD2800010u | ((uint32_t)idx << 5);                       /* movz x16, #idx */
    p[1] = 0xD2800011u | ((uint32_t)((h >>  0) & 0xFFFF) << 5);      /* movz x17       */
    p[2] = 0xF2A00011u | ((uint32_t)((h >> 16) & 0xFFFF) << 5);      /* movk x17, lsl16*/
    p[3] = 0xF2C00011u | ((uint32_t)((h >> 32) & 0xFFFF) << 5);      /* movk x17, lsl32*/
    p[4] = 0xF2E00011u | ((uint32_t)((h >> 48) & 0xFFFF) << 5);      /* movk x17, lsl48*/
    p[5] = 0xAA1003E0u;                                              /* mov x0, x16    */
    p[6] = 0xD61F0220u;                                              /* br x17         */
    p[7] = 0xD503201Fu;                                              /* nop            */
    __builtin___clear_cache((char *)p, (char *)(p + 8));
    stub_arena_prot(1);
    return p;
}

static void pb_missing_import(uint64_t idx)
{
    if (idx < (uint64_t)g_missing_n)
        fprintf(stderr, "\n[missing] guest called unresolved import: %s\n", g_missing_names[idx].name);
    else
        fprintf(stderr, "\n[missing] guest called unresolved import #%llu\n", (unsigned long long)idx);
    fflush(stderr);
    abort();
}

/* ------------------------------------------------------------- registry */
typedef struct { const char *name; void *addr; bool is_data; } entry;
static entry reg[PB_MAX_IMPORTS];
static int  reg_n;
static char missing_seen[256][96];
static int  missing_n;

void pb_shim_register(const char *name, void *addr, bool is_data)
{
    if (reg_n >= PB_MAX_IMPORTS) { fprintf(stderr, "[shim] registry full at %s\n", name); return; }
    reg[reg_n].name = name;
    reg[reg_n].addr = addr;
    reg[reg_n].is_data = is_data;
    reg_n++;
}

static void *find_override(const char *name, bool *is_data)
{
    for (int i = 0; i < reg_n; i++)
        if (!strcmp(reg[i].name, name)) { *is_data = reg[i].is_data; return reg[i].addr; }
    return NULL;
}

void *pb_shim_lookup(const char *name, bool *is_data)
{
    /* Never dlsym these into the guest: ABI or semantics differ. */
    static const char *poison[] = {
        "printf", "fprintf", "sprintf", "snprintf", "vsnprintf", "vfprintf",
        "vasprintf", "fscanf", "sscanf", "swprintf", "syslog",
        "__android_log_print", "__android_log_assert",
        "pthread_create", "pthread_join", "pthread_detach", "pthread_self",
        "pthread_attr_init", "pthread_attr_setdetachstate", "pthread_getschedparam",
        "pthread_mutex_init", "pthread_mutex_lock", "pthread_mutex_trylock",
        "pthread_mutex_unlock", "pthread_mutex_destroy", "pthread_mutexattr_init",
        "pthread_mutexattr_settype", "pthread_mutexattr_destroy",
        "pthread_cond_init", "pthread_cond_wait", "pthread_cond_timedwait",
        "pthread_cond_signal", "pthread_cond_broadcast", "pthread_cond_destroy",
        "pthread_rwlock_rdlock", "pthread_rwlock_wrlock", "pthread_rwlock_unlock",
        "pthread_once", "pthread_key_create", "pthread_key_delete",
        "pthread_setspecific", "pthread_getspecific",
        "fcntl", "open", "socket", "connect", "setsockopt", "getaddrinfo",
        "freeaddrinfo", "sysconf", "getauxval", "syscall", "__errno",
        "android_set_abort_message", "strerror_r",
        "__memcpy_aarch64_simd", "__memset_aarch64", "__memcmp_aarch64",
        "__cxa_thread_atexit_impl", "__register_atfork", "__stack_chk_fail",
        "dl_iterate_phdr", "__sF", "slCreateEngine", "ExecuteProgram", "nice",
    };
    void *res = find_override(name, is_data);
    if (!res) {
        for (size_t i = 0; i < sizeof(poison) / sizeof(poison[0]); i++)
            if (!strcmp(poison[i], name)) { res = NULL; goto miss; }
        res = dlsym(RTLD_DEFAULT, name);
        if (res) { *is_data = false; return res; }
    } else {
        return res;
    }
miss:
    /* unresolved: remember the name, hand out a named abort trampoline */
    for (int k = 0; k < g_missing_n; k++)
        if (!strcmp(g_missing_names[k].name, name)) return emit_missing_stub(k);
    if (g_missing_n < 512) {
        snprintf(g_missing_names[g_missing_n].name, 96, "%s", name);
        if (missing_n < 256) snprintf(missing_seen[missing_n++], 96, "%s", name);
        void *stub = emit_missing_stub(g_missing_n);
        if (getenv("PB_TRACE_SHIM"))
            fprintf(stderr, "[shim] %s -> missing-stub %p\n", name, stub);
        g_missing_n++;
        return stub;
    }
    return NULL;
}

/* --------------------------------------------------- libc boundary bits */

/* (__errno is implemented in pthread_shim.c: Bionic TCB slot 2.) */

static void shim___stack_chk_fail(void)
{
    fprintf(stderr, "[guest] *** stack smashing detected ***\n");
    abort();
}

static int shim___cxa_thread_atexit_impl(void (*dtor)(void *), void *arg, void *dso)
{
    (void)dtor; (void)arg; (void)dso;
    return 0;
}

static int shim___register_atfork(void *prep, void *parent, void *child, void *dso)
{
    (void)prep; (void)parent; (void)child; (void)dso;
    return 0;
}

static int shim_dl_iterate_phdr(void *cb, void *data)
{
    (void)cb; (void)data;
    return 0;   /* "no objects" — limits libc++abi unwind search, returns cleanly */
}

static unsigned long shim_getauxval(unsigned long type)
{
    (void)type;
    return 0;
}

/* Linux vs Darwin constant translation for sysconf */
static long shim_sysconf(int name)
{
    /* Linux numbers (bionic): _SC_PAGESIZE=39, _SC_NPROCESSORS_CONF=96,
       _SC_NPROCESSORS_ONLN=97, _SC_PHYS_PAGES=98, _SC_OPEN_MAX=4? (bionic=4) */
    switch (name) {
    case 39: return 16384;      /* PAGESIZE */
    case 96: case 97: return 8; /* CPU counts */
    case 98: return (1 << 20);  /* PHYS_PAGES */
    case 4:  return 1024;       /* OPEN_MAX */
    default: return -1;
    }
}

static long shim_syscall(long nr, ...)
{
    /* bionic SYS_gettid = 178 */
    if (nr == 178) {
        uint64_t tid = 0;
        pthread_threadid_np(NULL, &tid);
        return (long)tid;
    }
    errno = ENOSYS;
    return -1;
}

static void shim_android_set_abort_message(const char *msg) { (void)msg; }

/* Allocation tracing (set PB_TRACE_ALLOC=1): logs the guest call site of
 * every malloc-family call so heap trouble can be pinned to engine code. */
static inline uint64_t sh_lr(void)
{
    uint64_t v;
    __asm__ volatile("mov %0, x30" : "=r"(v));
    return v;
}

static void sh_trace_alloc(const char *what, size_t n, const void *p)
{
    if (!getenv("PB_TRACE_ALLOC")) return;
    uint64_t lr = sh_lr();
    fprintf(stderr, "[alloc] %-7s(%9zu) = %p  from img+%#llx\n",
            what, n, p, (unsigned long long)(lr - (uint64_t)(uintptr_t)PB.base));
}

static void *sh_malloc(size_t n)
{
    void *p = malloc(n);
    sh_trace_alloc("malloc", n, p);
    return p;
}

static void *sh_calloc(size_t a, size_t b)
{
    void *p = calloc(a, b);
    sh_trace_alloc("calloc", a * b, p);
    return p;
}

static void *sh_realloc(void *q, size_t n)
{
    void *p = realloc(q, n);
    sh_trace_alloc("realloc", n, p);
    return p;
}

static void sh_free(void *p)
{
    if (getenv("PB_TRACE_ALLOC")) {
        uint64_t lr = sh_lr();
        fprintf(stderr, "[alloc] free   (%p)  from img+%#llx\n",
                p, (unsigned long long)(lr - (uint64_t)(uintptr_t)PB.base));
    }
    free(p);
}

/* Android system properties.  The engine reads "ro.arch" during ctors and
 * strtol()s it; the harness's empty answer took a proven-safe path, so that
 * is the default.  A few standard names get plausible values for later code. */
static int shim_system_property_get(const char *name, char *value)
{
    static const struct { const char *n, *v; } props[] = {
        { "ro.arch",                    "" },
        { "ro.product.cpu.abi",         "arm64-v8a" },
        { "ro.product.cpu.abilist",     "arm64-v8a,armeabi-v7a,armeabi" },
        { "ro.product.model",           "Mac" },
        { "ro.product.manufacturer",    "Apple" },
        { "ro.product.brand",           "apple" },
        { "ro.product.name",            "mac" },
        { "ro.product.device",          "mac" },
        { "ro.build.version.sdk",       "34" },
        { "ro.build.version.release",   "14" },
        { "ro.build.version.codename",  "REL" },
        { "ro.hardware",                "apple" },
        { "ro.board.platform",          "apple" },
        { "ro.debuggable",              "0" },
        { "ro.secure",                  "1" },
        { "ro.serialno",                "PBNATIVE0001" },
        { "ro.build.fingerprint",       "apple/mac/mac:14/UP1A/1:user/release-keys" },
        { "ro.build.type",              "user" },
        { "ro.build.tags",              "release-keys" },
        { "gsm.version.baseband",       "" },
        { "net.dns1",                   "" },
    };
    if (value) value[0] = 0;
    if (!name) return 0;
    for (size_t i = 0; i < sizeof(props) / sizeof(props[0]); i++) {
        if (!strcmp(props[i].n, name)) {
            size_t n = strlen(props[i].v);
            if (n > 91) n = 91;
            memcpy(value, props[i].v, n);
            value[n] = 0;
            return (int)n;
        }
    }
    return 0;
}

static int shim_strerror_r(int err, char *buf, size_t n)
{
    /* GNU variant: engine expects the RETURN VALUE to be a char* */
    const char *s = strerror(err);
    if (n) { snprintf(buf, n, "%s", s); return (int)(intptr_t)buf; }
    return (int)(intptr_t)s;
}

static int shim_nice(int inc) { (void)inc; return 0; }

/* macOS names these __sincos/__sincosf with an out-parameter shape */
static void shim_sincos(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }
static void shim_sincosf(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }

/* string routines Bionic exports with arch-specific names */
static void *shim_memcpy_aarch64_simd(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static void *shim_memset_aarch64(void *d, int c, size_t n) { return memset(d, c, n); }
static int   shim_memcmp_aarch64(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }

/* fd flag translation: Linux -> Darwin (and back for F_GETFL results) */
#define L_O_CREAT    0x40
#define L_O_TRUNC    0x200
#define L_O_APPEND   0x400
#define L_O_NONBLOCK 0x800
#define L_O_EXCL     0x80
#define L_O_CLOEXEC  0x80000

static int l2d_flags(int lf)
{
    int df = lf & 3;
    if (lf & L_O_CREAT)    df |= O_CREAT;
    if (lf & L_O_TRUNC)    df |= O_TRUNC;
    if (lf & L_O_APPEND)   df |= O_APPEND;
    if (lf & L_O_NONBLOCK) df |= O_NONBLOCK;
    if (lf & L_O_EXCL)     df |= O_EXCL;
    if (lf & L_O_CLOEXEC)  df |= O_CLOEXEC;
    return df;
}

static int d2l_flags(int df)
{
    int lf = df & 3;
    if (df & O_CREAT)    lf |= L_O_CREAT;
    if (df & O_TRUNC)    lf |= L_O_TRUNC;
    if (df & O_APPEND)   lf |= L_O_APPEND;
    if (df & O_NONBLOCK) lf |= L_O_NONBLOCK;
    if (df & O_EXCL)     lf |= L_O_EXCL;
    if (df & O_CLOEXEC)  lf |= L_O_CLOEXEC;
    return lf;
}

static int shim_open(const char *path, int flags, int mode)
{
    return open(path, l2d_flags(flags), mode);
}

static int shim_pipe(int fds[2])
{
    return pipe(fds);
}

static int shim_fcntl(int fd, int cmd, void *arg)
{
    /* F_GETFL/F_SETFL only need flag translation; other cmds pass through */
    if (cmd == 3) { int r = fcntl(fd, F_GETFL, arg); return r < 0 ? r : d2l_flags(r); } /* F_GETFL */
    if (cmd == 4) { return fcntl(fd, F_SETFL, l2d_flags((int)(intptr_t)arg)); }        /* F_SETFL */
    return fcntl(fd, cmd, arg);
}

/* sockets: AF_INET6 10 (linux) -> 30 (darwin); SOCK_NONBLOCK in type */
static int shim_socket(int domain, int type, int protocol)
{
    int nonblock = 0;
    if (type & 0x800) { nonblock = 1; type &= ~0x800; }   /* SOCK_NONBLOCK */
    int d = domain == 10 ? 30 : domain;
    int fd = socket(d, type, protocol);
    if (fd >= 0 && nonblock) fcntl(fd, F_SETFL, O_NONBLOCK);
    return fd;
}

/* sockaddr Linux <-> BSD (sa_len at [0], family at [1]) */
static void l2d_sockaddr(void *dst, const void *src, unsigned len)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    uint16_t fam = (uint16_t)(s[0] | (s[1] << 8));
    if (fam == 10) fam = 30;
    memset(d, 0, len);
    d[0] = (uint8_t)len;
    d[1] = (uint8_t)fam;
    d[2] = s[2]; d[3] = s[3];          /* port, same layout */
    memcpy(d + 4, s + 4, len >= 28 ? 24 : 12);
}

static int shim_connect(int fd, const void *addr, unsigned len)
{
    uint8_t buf[128];
    if (len > sizeof(buf)) { errno = EINVAL; return -1; }
    l2d_sockaddr(buf, addr, len);
    return connect(fd, (const struct sockaddr *)buf, len);
}

static int shim_getaddrinfo(const char *node, const char *service, const void *hints,
                            void **res)
{
    (void)node; (void)service; (void)hints;
    if (res) *res = NULL;
    return -2;  /* EAI_NONAME — networking arrives in a later phase */
}

static void shim_freeaddrinfo(void *res) { (void)res; }

static int shim_setsockopt(int fd, int level, int opt, const void *val, unsigned len)
{
    return setsockopt(fd, level, opt, val, len);
}

void pb_shim_install_libc(void)
{
    pb_shim_register("__stack_chk_fail", (void *)shim___stack_chk_fail, false);
    pb_shim_register("__cxa_thread_atexit_impl", (void *)shim___cxa_thread_atexit_impl, false);
    pb_shim_register("__register_atfork", (void *)shim___register_atfork, false);
    pb_shim_register("dl_iterate_phdr", (void *)shim_dl_iterate_phdr, false);
    pb_shim_register("getauxval", (void *)shim_getauxval, false);
    pb_shim_register("sysconf", (void *)shim_sysconf, false);
    pb_shim_register("syscall", (void *)shim_syscall, false);
    pb_shim_register("android_set_abort_message", (void *)shim_android_set_abort_message, false);
    pb_shim_register("strerror_r", (void *)shim_strerror_r, false);
    pb_shim_register("nice", (void *)shim_nice, false);
    pb_shim_register("__memcpy_aarch64_simd", (void *)shim_memcpy_aarch64_simd, false);
    pb_shim_register("__memset_aarch64", (void *)shim_memset_aarch64, false);
    pb_shim_register("__memcmp_aarch64", (void *)shim_memcmp_aarch64, false);
    pb_shim_register("open", (void *)shim_open, false);
    pb_shim_register("pipe", (void *)shim_pipe, false);
    pb_shim_register("fcntl", (void *)shim_fcntl, false);
    pb_shim_register("socket", (void *)shim_socket, false);
    pb_shim_register("connect", (void *)shim_connect, false);
    pb_shim_register("getaddrinfo", (void *)shim_getaddrinfo, false);
    pb_shim_register("freeaddrinfo", (void *)shim_freeaddrinfo, false);
    pb_shim_register("setsockopt", (void *)shim_setsockopt, false);
    pb_shim_register("nice", (void *)shim_nice, false);
    pb_shim_register("__system_property_get", (void *)shim_system_property_get, false);
    pb_shim_register("malloc", (void *)sh_malloc, false);
    pb_shim_register("calloc", (void *)sh_calloc, false);
    pb_shim_register("realloc", (void *)sh_realloc, false);
    pb_shim_register("free", (void *)sh_free, false);
    pb_shim_register("sincos", (void *)shim_sincos, false);
    pb_shim_register("sincosf", (void *)shim_sincosf, false);
}

/* PairIP VM entry — must never execute (plaintext .text). */
static uint64_t execute_program(void) { fprintf(stderr, "[stub] ExecuteProgram called!\n"); return 0; }

/* OpenSL ES object model lives in opensles_shim.c (pb_shim_install_opensl). */

void pb_shim_install_stubs(void)
{
    pb_shim_register("ExecuteProgram", (void *)execute_program, false);
}

void pb_shim_report_missing(void)
{
    if (missing_n == 0) { pb_log("[shim] all imports resolved\n"); return; }
    pb_log("[shim] %d UNRESOLVED imports:\n", missing_n);
    for (int i = 0; i < missing_n; i++) pb_log("    %s\n", missing_seen[i]);
}
