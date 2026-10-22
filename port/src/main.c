/* main.c — Payback 2 native port boot driver.
 *
 * Milestone 1: map + relocate the patched ELF, resolve imports to shims,
 * run constructors, call JNI_OnLoad.  Milestone 2 (arg "engine"): seed the
 * PairIP globals and call initializeNativeCode + the lifecycle natives.
 */
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/ucontext.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/thread_act.h>
#include <mach/arm/thread_status.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

#include "pb.h"

/* GameActivity hands the engine the three data directories as jstrings:
 *   initializeNativeCode(getFilesDir(), getObbDir(), getExternalFilesDir(null),
 *                        assets, savedState, configuration)
 * On Android the engine writes "Backup.sav" / "Payback.ini" straight into the
 * files dir, so mirror that layout under a macOS data root. */
static char pb_files_dir[1024];
static char pb_obb_dir[1024];
static char pb_external_dir[1024];

static void pb_setup_data_dir(void)
{
    const char *env = getenv("PB_DATA_DIR");
    char root[900];
    if (env && *env) {
        snprintf(root, sizeof root, "%s", env);
    } else {
        const char *home = getenv("HOME");
        snprintf(root, sizeof root, "%s/Library/Application Support/Payback2",
                 home && *home ? home : ".");
    }
    snprintf(pb_files_dir, sizeof pb_files_dir, "%s/files", root);
    snprintf(pb_obb_dir, sizeof pb_obb_dir, "%s/obb", root);
    snprintf(pb_external_dir, sizeof pb_external_dir, "%s/external", root);
    const char *dirs[4] = { root, pb_files_dir, pb_obb_dir, pb_external_dir };
    for (int i = 0; i < 4; i++) {
        if (mkdir(dirs[i], 0755) != 0 && errno != EEXIST)
            pb_log("[data] mkdir(%s) failed: %s\n", dirs[i], strerror(errno));
    }
    pb_log("[data] files dir %s\n", pb_files_dir);
}

/* ---- input -----------------------------------------------------------------
 * GameActivity registers its lifecycle natives through RegisterNatives; the
 * Java side would call onTouchEventNative(handle, MotionEvent) for every
 * touch.  The port has no Java, so it calls the registered function directly
 * with a fake MotionEvent backed by the state in jni_env.c. */
static uint64_t g_game_handle;
static void *g_touch_native, *g_key_down_native, *g_key_up_native;
static uint64_t g_motion_obj;
static long long g_down_ms;

static long long pb_mono_ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void pb_input_attach(uint64_t game)
{
    g_game_handle = game;
    /* The engine statically links GameActivity but never calls
     * RegisterNatives at runtime (the JNINativeMethod table at image
     * 0x388828 is what a Java VM would have consumed).  The three input
     * wrappers are fixed offsets in the image; their entries call
     * GameActivityMotionEvent_fromJava/GameActivityKeyEvent_fromJava and
     * then tail-call game->onTouchEvent / onKeyDown / onKeyUp. */
    g_touch_native = PB.base + 0x31D400;
    g_key_down_native = PB.base + 0x31D458;
    g_key_up_native = PB.base + 0x31D4B0;
    void *reg_touch = pb_jni_native("onTouchEventNative");
    if (reg_touch) g_touch_native = reg_touch;
    g_motion_obj = pb_jni_new_object("android/view/MotionEvent");
    const uint64_t *cb = (const uint64_t *)(uintptr_t)(game + 0xb8);
    pb_log("[input] game=%#llx onTouchEvent=%p onKeyDown=%p onKeyUp=%p\n",
           (unsigned long long)game, (void *)(uintptr_t)cb[0],
           (void *)(uintptr_t)cb[1], (void *)(uintptr_t)cb[2]);
    uint64_t app = *(const uint64_t *)(uintptr_t)(game + 0x38);
    uint64_t on_input = 0;
    if (app) on_input = *(const uint64_t *)(uintptr_t)(app + 0x10);
    pb_log("[input] app=%#llx onInputEvent=%#llx\n",
           (unsigned long long)app, (unsigned long long)on_input);
    pb_log("[input] touch=%p keydown=%p keyup=%p obj=%#llx (registered touch=%p)\n",
           g_touch_native, g_key_down_native, g_key_up_native,
           (unsigned long long)g_motion_obj, reg_touch);
}

/* Android MotionEvent actions: DOWN 0, UP 1, MOVE 2, CANCEL 3, HOVER 7 */
void pb_input_motion(int action, float x, float y)
{
    if (!g_touch_native || !g_game_handle) return;
    long long now = pb_mono_ms_now();
    if (action == 0 || g_down_ms == 0) g_down_ms = now;
    pb_jni_set_motion(action, x, y, now, g_down_ms);
    int r = ((int (*)(void *, void *, uint64_t, uint64_t))g_touch_native)(
                pb_jni_env(), (void *)(uintptr_t)0x60200010,
                g_game_handle, g_motion_obj);
    if (getenv("PB_INPUT_LOG"))
        pb_log("[input] motion action=%d x=%.1f y=%.1f -> %d\n", action, x, y, r);
}

/* Android KeyEvent actions: DOWN 0, UP 1 */
void pb_input_key(int action, int code)
{
    void *fn = action == 0 ? g_key_down_native : g_key_up_native;
    if (!fn || !g_game_handle) return;
    long long now = pb_mono_ms_now();
    if (action == 0) g_down_ms = now;
    pb_jni_set_key(code, 0, 0, 0);
    int r = ((int (*)(void *, void *, uint64_t, uint64_t))fn)(
                pb_jni_env(), (void *)(uintptr_t)0x60200010,
                g_game_handle, g_motion_obj);
    if (getenv("PB_INPUT_LOG"))
        pb_log("[input] key action=%d code=%d -> %d\n", action, code, r);
}

/* PB_TAP=<sec>:<x>:<y>[,<sec>:<x>:<y>...] — synthetic taps for testing. */
static void *pb_tap_thread(void *arg)
{
    char *spec = strdup((const char *)arg);
    char *save = NULL;
    long long t0 = pb_mono_ms_now();
    for (char *tok = strtok_r(spec, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        double at; float x, y; int hold = 80;
        int nf = sscanf(tok, "%lf:%f:%f:%d", &at, &x, &y, &hold);
        if (nf < 3) continue;
        if (hold < 20) hold = 20;
        long long due = t0 + (long long)(at * 1000.0);
        long long now = pb_mono_ms_now();
        if (due > now) usleep((useconds_t)(due - now) * 1000);
        if (getenv("PB_TAP_MOVE")) pb_input_motion(2, x, y);  /* optional mouse-like pre-move */
        pb_input_motion(0, x, y);      /* down */
        int jit = getenv("PB_TAP_JITTER") ? atoi(getenv("PB_TAP_JITTER")) : 0;
        if (jit) pb_input_motion(2, x + (float)jit, y);   /* nudge (scroll-view click) */
        usleep((useconds_t)hold * 1000);
        pb_input_motion(1, x, y);      /* up */
        pb_log("[input] tap %.2fs at %.0f,%.0f hold=%dms\n", at, x, y, hold);
    }
    free(spec);
    return NULL;
}

/* PB_DRAG=<sec>:<x1>:<y1>:<x2>:<y2>[:<hold_ms>][,...] — press, drag, release. */
static void *pb_drag_thread(void *arg)
{
    char *spec = strdup((const char *)arg);
    char *save = NULL;
    long long t0 = pb_mono_ms_now();
    for (char *tok = strtok_r(spec, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        double at, x1, y1, x2, y2; int hold = 300;
        int nf = sscanf(tok, "%lf:%lf:%lf:%lf:%lf:%d", &at, &x1, &y1, &x2, &y2, &hold);
        if (nf < 5) continue;
        long long due = t0 + (long long)(at * 1000.0);
        long long now = pb_mono_ms_now();
        if (due > now) usleep((useconds_t)(due - now) * 1000);
        pb_input_motion(0, (float)x1, (float)y1);            /* down on source */
        usleep(40000);
        pb_input_motion(2, (float)x1, (float)y1);            /* move (small hold) */
        usleep((useconds_t)hold * 1000);
        for (int i = 1; i <= 6; i++) {                       /* glide */
            float fx = (float)(x1 + (x2 - x1) * i / 6.0);
            float fy = (float)(y1 + (y2 - y1) * i / 6.0);
            pb_input_motion(2, fx, fy);
            usleep(30000);
        }
        usleep(200000);
        pb_input_motion(1, (float)x2, (float)y2);            /* up on target */
        pb_log("[input] drag %.2fs (%.0f,%.0f) -> (%.0f,%.0f) hold=%dms\n",
               at, x1, y1, x2, y2, hold);
    }
    free(spec);
    return NULL;
}

/* PB_MOVE=<sec>:<x>:<y>[,...] — pointer moves only (hover test). */
static void *pb_move_thread(void *arg)
{
    char *spec = strdup((const char *)arg);
    char *save = NULL;
    long long t0 = pb_mono_ms_now();
    for (char *tok = strtok_r(spec, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        double at; float x, y;
        if (sscanf(tok, "%lf:%f:%f", &at, &x, &y) != 3) continue;
        long long due = t0 + (long long)(at * 1000.0);
        long long now = pb_mono_ms_now();
        if (due > now) usleep((useconds_t)(due - now) * 1000);
        pb_input_motion(2, x, y);
        pb_log("[input] move %.2fs -> %.0f,%.0f\n", at, x, y);
    }
    free(spec);
    return NULL;
}


/* --- runtime memory scanner (PB_SCAN) ----------------------------------
 * PB_SCAN=<text>[,<text>...]  PB_MSCAN_AT=<sec>  PB_MSCAN_MAX=<n>
 * PB_MSCAN_PTR=<hexaddr>[,<hexaddr>...]   scan for 8-byte pointers
 * Scans readable, writable regions of the process (guest heap lives here).
 */
static unsigned long long pb_scan_one(const char *needle, int maxhits)
{
    unsigned long long first = 0;
    size_t nlen = strlen(needle);
    if (!nlen) return 0;
    mach_vm_address_t addr = 0;
    int hits = 0;
    while (hits < maxhits) {
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        kern_return_t kr = mach_vm_region(mach_task_self(), &addr, &size,
                                          VM_REGION_BASIC_INFO_64,
                                          (vm_region_info_t)&info, &count, &obj);
        if (kr != KERN_SUCCESS) break;
        if ((info.protection & VM_PROT_READ) && (info.protection & VM_PROT_WRITE) &&
            size <= (mach_vm_size_t)2 * 1024 * 1024 * 1024) {
            const size_t chunk = 1 << 20;
            unsigned char *buf = malloc(chunk + 4096);
            if (buf) {
                for (mach_vm_size_t off = 0; off < size && hits < maxhits; off += chunk) {
                    mach_vm_size_t want = (size - off < chunk) ? (size - off) : chunk;
                    mach_vm_size_t got = 0;
                    if (mach_vm_read_overwrite(mach_task_self(), addr + off, want,
                                               (mach_vm_address_t)buf, &got) != KERN_SUCCESS)
                        continue;
                    if (got) {
                        unsigned char *p = buf, *end = buf + got;
                        while (p < end) {
                            unsigned char *q = memmem(p, (size_t)(end - p), needle, nlen);
                            if (!q) break;
                            if (!first) first = (unsigned long long)(addr + off + (mach_vm_size_t)(q - buf));
                            fprintf(stderr, "[scan] \"%s\" at %#llx (region %#llx prot=%d)\n",
                                    needle, (unsigned long long)(addr + off + (mach_vm_size_t)(q - buf)),
                                    (unsigned long long)addr, (int)info.protection);
                            hits++;
                            p = q + 1;
                            if (hits >= maxhits) break;
                        }
                    }
                }
                free(buf);
            }
        }
        addr += size;
    }
    if (!hits) fprintf(stderr, "[scan] \"%s\" not found\n", needle);
    return first;
}

static void pb_scan_ptrs(const char *spec, int maxhits)
{
    char *copy = strdup(spec);
    char *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        unsigned long long target = strtoull(tok, NULL, 0);
        if (!target) continue;
        mach_vm_address_t addr = 0;
        int hits = 0;
        while (hits < maxhits) {
            mach_vm_size_t size = 0;
            vm_region_basic_info_data_64_t info;
            mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t obj = MACH_PORT_NULL;
            kern_return_t kr = mach_vm_region(mach_task_self(), &addr, &size,
                                              VM_REGION_BASIC_INFO_64,
                                              (vm_region_info_t)&info, &count, &obj);
            if (kr != KERN_SUCCESS) break;
            if ((info.protection & VM_PROT_READ) && (info.protection & VM_PROT_WRITE) &&
                size <= (mach_vm_size_t)2 * 1024 * 1024 * 1024) {
                const size_t chunk = 1 << 20;
                unsigned char *buf = malloc(chunk + 4096);
                if (buf) {
                    for (mach_vm_size_t off = 0; off < size && hits < maxhits; off += chunk) {
                        mach_vm_size_t want = (size - off < chunk) ? (size - off) : chunk;
                        mach_vm_size_t got = 0;
                        if (mach_vm_read_overwrite(mach_task_self(), addr + off, want,
                                                   (mach_vm_address_t)buf, &got) != KERN_SUCCESS)
                            continue;
                        for (size_t i = 0; i + 8 <= (size_t)got; i += 8) {
                            unsigned long long v;
                            memcpy(&v, buf + i, 8);
                            if (v == target) {
                                fprintf(stderr, "[scan] ptr %#llx at %#llx (region %#llx)\n",
                                        target, (unsigned long long)(addr + off + i),
                                        (unsigned long long)addr);
                                if (++hits >= maxhits) break;
                            }
                        }
                    }
                    free(buf);
                }
            }
            addr += size;
        }
        if (!hits) fprintf(stderr, "[scan] ptr %#llx not found\n", target);
    }
    free(copy);
}

static void *pb_scan_thread(void *arg)
{
    (void)arg;
    const char *at = getenv("PB_MSCAN_AT");
    int delay = at ? atoi(at) : 8;
    if (delay < 0) delay = 0;
    sleep((unsigned)delay);
    int maxhits = getenv("PB_MSCAN_MAX") ? atoi(getenv("PB_MSCAN_MAX")) : 16;
    const char *spec = getenv("PB_MSCAN");
    if (spec && *spec) {
        char *copy = strdup(spec);
        char *save = NULL;
        for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long long hit = pb_scan_one(tok, maxhits);
            if (hit && getenv("PB_MSCAN_FOLLOW")) {
                char pbuf[32];
                snprintf(pbuf, sizeof pbuf, "%#llx", hit);
                pb_scan_ptrs(pbuf, maxhits);
            }
        }
        free(copy);
    }
    const char *pspec = getenv("PB_MSCAN_PTR");
    if (pspec && *pspec) pb_scan_ptrs(pspec, maxhits);
    return NULL;
}

static void *pb_watch_thread(void *arg)
{
    (void)arg;
    const char *spec = getenv("PB_WATCH");
    if (!spec || !*spec) return NULL;
    for (int t = 1;; t++) {
        sleep(1);
        char buf[512], out[1024];
        snprintf(buf, sizeof buf, "%s", spec);
        int o = 0;
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok;
             tok = strtok_r(NULL, ",", &save)) {
            if (*tok == '@') {
                /* @ADDR[+OFF]: read pointer at PB.base+ADDR, then u32 at ptr+OFF */
                unsigned long o2 = 0;
                char *plus = strchr(tok, '+');
                if (plus) { *plus = 0; o2 = strtoul(plus + 1, NULL, 0); }
                unsigned long a2 = strtoul(tok + 1, NULL, 0);
                unsigned long long pp = 0, v = 0;
                memcpy(&pp, (char *)PB.base + a2, 8);
                if (pp > 0x10000 && pp < 0x0001000000000000ULL) memcpy(&v, (char *)pp + o2, 4);
                o += snprintf(out + o, sizeof out - (size_t)o, " @%#lx+%#lx=%#llx",
                              a2, o2, v);
            } else {
                unsigned long off = strtoul(tok, NULL, 0);
                unsigned long long v = 0;
                memcpy(&v, (char *)PB.base + off, 8);
                o += snprintf(out + o, sizeof out - (size_t)o, " %#lx=%#llx", off, v);
            }
            if (o > (int)sizeof out - 64) break;
        }
        fprintf(stderr, "[watch] t=%d%s\n", t, out);
    }
    return NULL;
}

/* PB_SAMPLE=<ms>:0xADDR,0xADDR,... : high-rate u32 sampling of guest cells,
 * one "[sample] t=<secs> ..." line per tick.  Used to compare dynamic
 * behaviour against the live Android process on the emulator. */
static void *pb_sample_thread(void *arg)
{
    char spec[1024];
    snprintf(spec, sizeof spec, "%s", (const char *)arg);
    char *colon = strchr(spec, ':');
    if (!colon) return NULL;
    *colon = 0;
    int ms = atoi(spec);
    if (ms <= 0) ms = 20;
    unsigned long offs[32];
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(colon + 1, ",", &save); tok && n < 32;
         tok = strtok_r(NULL, ",", &save))
        offs[n++] = strtoul(tok, NULL, 0);
    if (!n) return NULL;
    struct timespec t0, tn;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    char out[1024];
    for (;;) {
        int o = 0;
        for (int i = 0; i < n; i++) {
            unsigned v = 0;
            memcpy(&v, (const char *)PB.base + offs[i], 4);
            o += snprintf(out + o, sizeof out - (size_t)o, " %#lx=%u", offs[i], v);
        }
        clock_gettime(CLOCK_MONOTONIC, &tn);
        double dt = (double)(tn.tv_sec - t0.tv_sec)
                  + (double)(tn.tv_nsec - t0.tv_nsec) / 1e9;
        fprintf(stderr, "[sample] t=%.3f%s\n", dt, out);
        usleep((useconds_t)ms * 1000);
    }
    return NULL;
}

extern void pb_shim_install_libc(void);
extern void pb_shim_install_stubs(void);
extern void pb_stdio_register(void);
uint64_t pb_call8(void *fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                  uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);

/* PB_DUMP_AT=<sec>:0xADDR:len:/path[,...] : dump a guest range after <sec>
 * of runtime (the boot-time PB_DUMPM is too early for state comparisons). */
static void *pb_dump_at_thread(void *arg)
{
    char *spec = strdup((const char *)arg);
    int sec = 0;
    char *c0 = strchr(spec, ':');
    if (!c0) { free(spec); return NULL; }
    *c0 = 0;
    sec = atoi(spec);
    char *p = c0 + 1;
    struct timespec ts = { .tv_sec = sec };
    nanosleep(&ts, NULL);
    while (p && *p) {
        char *c1 = strchr(p, ':');
        if (!c1) break;
        *c1 = 0;
        char *c2 = strchr(c1 + 1, ':');
        if (!c2) break;
        *c2 = 0;
        unsigned long off = strtoul(p, NULL, 0);
        unsigned long len = strtoul(c1 + 1, NULL, 0);
        FILE *f = fopen(c2 + 1, "wb");
        if (f) {
            fwrite((const char *)PB.base + off, 1, len, f);
            fclose(f);
            pb_log("[dump_at] %#lx..%#lx -> %s\n", off, off + len, c2 + 1);
        }
        p = strchr(c2 + 1, ',');
        if (p) p++;
    }
    free(spec);
    return NULL;
}

static void pb_dump_at_init(const char *spec)
{
    if (!spec || !*spec || !PB.base) return;
    pthread_t t;
    pthread_create(&t, NULL, pb_dump_at_thread, (void *)spec);
    pthread_detach(t);
}

/* PB_DUMPM=0xADDR:len:/path[,0xADDR:len:/path...] : at boot, write guest memory
 * ranges to host files (used to diff the relocated image against the Android
 * oracle's live mapping). */
static void pb_dump_ranges(void)
{
    const char *spec = getenv("PB_DUMPM");
    char buf[1024];
    if (spec && *spec) {
    snprintf(buf, sizeof buf, "%s", spec);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        char *c1 = strchr(tok, ':');
        if (!c1) continue;
        *c1 = 0;
        char *c2 = strchr(c1 + 1, ':');
        if (!c2) continue;
        *c2 = 0;
        unsigned long off = strtoul(tok, NULL, 0);
        unsigned long len = strtoul(c1 + 1, NULL, 0);
        FILE *f = fopen(c2 + 1, "wb");
        if (!f) { pb_log("[dumpm] cannot open %s\n", c2 + 1); continue; }
        size_t n = fwrite((const char *)PB.base + off, 1, len, f);
        fclose(f);
        pb_log("[dumpm] %#lx..%#lx -> %s (%zu bytes)\n", off, off + len, c2 + 1, n);
    }
    }
    /* PB_DUMPCHAIN=0xADDR+0x10+0x20:len:/path[,...] : dereference a pointer
     * chain starting at image-relative ADDR, then dump len bytes at the end.
     * Host pointers in the chain are followed as-is. */
    spec = getenv("PB_DUMPCHAIN");
    if (!spec || !*spec) return;
    snprintf(buf, sizeof buf, "%s", spec);
    char *save2 = NULL;
    for (char *tok = strtok_r(buf, ",", &save2); tok;
         tok = strtok_r(NULL, ",", &save2)) {
        char *c1 = strchr(tok, ':');
        if (!c1) continue;
        *c1 = 0;
        char *c2 = strchr(c1 + 1, ':');
        if (!c2) continue;
        *c2 = 0;
        unsigned long len = strtoul(c1 + 1, NULL, 0);
        uint64_t p = strtoull(tok, NULL, 0);
        if (p < PB.span) p += (uint64_t)(uintptr_t)PB.base;   /* image-relative start */
        char *q = tok;
        while ((q = strchr(q, '+')) != NULL) {
            q++;
            unsigned long add = strtoul(q, NULL, 0);
            uint64_t v = 0;
            memcpy(&v, (void *)(uintptr_t)p, 8);
            pb_log("[dumpc] %#llx -> %#llx (+%#lx)\n",
                   (unsigned long long)p, (unsigned long long)v, add);
            p = v + add;
        }
        FILE *f = fopen(c2 + 1, "wb");
        if (!f) { pb_log("[dumpc] cannot open %s\n", c2 + 1); continue; }
        size_t n = fwrite((void *)(uintptr_t)p, 1, len, f);
        fclose(f);
        pb_log("[dumpc] %#llx (%zu bytes) -> %s\n", (unsigned long long)p, n, c2 + 1);
    }
}

/* Crash reporter: registers, guest offsets, and a frame-pointer walk with
 * host symbolization - enough to place any fault without lldb. */
static const char *pb_where(uint64_t a)
{
    static char buf[160];
    uint64_t b = (uint64_t)(uintptr_t)PB.base;
    if (a >= b && a < b + PB.span + 0x10000) {
        snprintf(buf, sizeof buf, "img+%#llx", a - b);
        return buf;
    }
    Dl_info di;
    if (dladdr((void *)a, &di) && di.dli_sname)
        snprintf(buf, sizeof buf, "%s", di.dli_sname);
    else
        snprintf(buf, sizeof buf, "?%#llx", (unsigned long long)a);
    return buf;
}

static uintptr_t g_prot_lo, g_prot_hi;
void pb_prot_refresh(void);
static int g_prot_count;

static void *pb_prot_rearm_thread(void *arg)
{
    int us = (int)(intptr_t)arg;
    for (;;) {
        pb_prot_refresh();
        usleep((useconds_t)us);
    }
    return NULL;
}

static void pb_prot_init(void)
{
    const char *e = getenv("PB_PROT_RANGE");   /* "off_lo-off_hi", image-relative */
    if (!e) return;
    unsigned long lo = 0, hi = 0;
    if (sscanf(e, "%lx-%lx", &lo, &hi) != 2 || !PB.base) return;
    g_prot_lo = ((uintptr_t)PB.base + lo) & ~(uintptr_t)0x3fff;
    g_prot_hi = (((uintptr_t)PB.base + hi) + 0x3fff) & ~(uintptr_t)0x3fff;
    fprintf(stderr, "[prot] watching %#lx-%#lx\n",
            (unsigned long)g_prot_lo, (unsigned long)g_prot_hi);
    if (getenv("PB_PROT_START")) pb_prot_refresh();
    /* PB_PROT_FAST=<us>: keep re-arming from a background thread so nearly
     * every write to the watched pages is reported, not just the first one
     * per frame.  Slow, but it names the exact writer. */
    if (getenv("PB_PROT_FAST")) {
        int us = atoi(getenv("PB_PROT_FAST"));
        if (us <= 0) us = 100;
        pthread_t t;
        pthread_create(&t, NULL, pb_prot_rearm_thread, (void *)(intptr_t)us);
        pthread_detach(t);
    }
}

void pb_prot_refresh(void)
{
    if (!g_prot_lo) return;
    if (g_prot_count > 400 && !getenv("PB_PROT_FAST")) return;
    int rc = mprotect((void *)g_prot_lo, g_prot_hi - g_prot_lo, PROT_READ);
    {
        static int n_dbg;
        if (n_dbg < 3) {
            n_dbg++;
            fprintf(stderr, "[prot] arm rc=%d errno=%d va=%#lx len=%#lx cur=%#llx\n",
                    rc, errno, (unsigned long)g_prot_lo,
                    (unsigned long)(g_prot_hi - g_prot_lo),
                    (unsigned long long)*(volatile uint64_t *)g_prot_lo);
        }
    }
}

/* pb_backtrace: walk the frame-pointer chain (guest frames use the standard
 * AAPCS64 frame record) from the caller's frame and print guest-relative LRs.
 * Used with PB_TRACE_ASSET to see who drives per-frame asset loads. */
void pb_backtrace(const char *tag)
{
    uintptr_t hi = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
    size_t sz = pthread_get_stacksize_np(pthread_self());
    uintptr_t lo = hi - sz;
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
    uintptr_t gbase = (uintptr_t)PB.base, gend = gbase + PB.span;
    int o = 0;
    char out[1024];
    o += snprintf(out + o, sizeof out - (size_t)o, "[bt] %s:", tag);
    for (int i = 0; i < 40; i++) {
        if (fp < lo || fp + 16 > hi || (fp & 7)) break;
        uintptr_t lr = *(uintptr_t *)(fp + 8);
        if (lr >= gbase && lr < gend)
            o += snprintf(out + o, sizeof out - (size_t)o, " g+%#lx", (unsigned long)(lr - gbase));
        else if (lr < 0x100000)
            o += snprintf(out + o, sizeof out - (size_t)o, " ?%#lx", (unsigned long)lr);
        else
            o += snprintf(out + o, sizeof out - (size_t)o, " h%#lx", (unsigned long)lr);
        if (o > (int)sizeof out - 32) break;
        uintptr_t nfp = *(uintptr_t *)fp;
        if (nfp <= fp) break;
        fp = nfp;
    }
    fprintf(stderr, "%s\n", out);
    fflush(stderr);
}

static void pb_segv(int sig, siginfo_t *si, void *uctx)
{
    ucontext_t *uc = uctx;
    struct __darwin_arm_thread_state64 *ssp = &uc->uc_mcontext->__ss;
    struct __darwin_arm_neon_state64 *nsp = &uc->uc_mcontext->__ns;
#define ss (*ssp)
    uint64_t pc = (uint64_t)ss.__pc;
    uint64_t lr = (uint64_t)ss.__lr;
    uint64_t sp = (uint64_t)ss.__sp;
    uint64_t fp = (uint64_t)ss.__fp;

    if (g_prot_lo && (uintptr_t)si->si_addr >= g_prot_lo && (uintptr_t)si->si_addr < g_prot_hi) {
        g_prot_count++;
        fprintf(stderr, "[prot] hit#%d addr=%#llx pc=%#llx (%s) lr=%#llx x0=%#llx x1=%#llx x2=%#llx x3=%#llx x19=%#llx x20=%#llx\n",
                g_prot_count, (unsigned long long)(uintptr_t)si->si_addr,
                (unsigned long long)pc, pb_where(pc), (unsigned long long)lr,
                (unsigned long long)ss.__x[0], (unsigned long long)ss.__x[1],
                (unsigned long long)ss.__x[2], (unsigned long long)ss.__x[3],
                (unsigned long long)ss.__x[19], (unsigned long long)ss.__x[20]);
        {
            const float *fs = (const float *)&nsp->__v[0];
            fprintf(stderr, "        s0..s7 = %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g\n",
                    fs[0], fs[1], fs[2], fs[3], fs[4], fs[5], fs[6], fs[7]);
            uint64_t e = ss.__x[19];
            if (e > 0x100000000ULL && e < 0x800000000000ULL) {
                uint32_t *w = (uint32_t *)e;
                fprintf(stderr, "        elem[%#llx]: %08x %08x %08x %08x %08x %08x %08x\n",
                        (unsigned long long)e, w[0], w[1], w[2], w[3], w[4], w[5], w[6]);
            }
        }
        fflush(stderr);
        mprotect((void *)g_prot_lo, g_prot_hi - g_prot_lo, PROT_READ | PROT_WRITE);
        return;
    }

    fprintf(stderr, "\n[fault] signal %d addr=%p\n", sig, si->si_addr);
    fprintf(stderr, "        pc=%#llx (%s)\n", (unsigned long long)pc, pb_where(pc));
    fprintf(stderr, "        lr=%#llx (%s)  sp=%#llx fp=%#llx\n",
            (unsigned long long)lr, pb_where(lr),
            (unsigned long long)sp, (unsigned long long)fp);
    fprintf(stderr, "        x0=%#llx x1=%#llx x2=%#llx x3=%#llx\n",
            (unsigned long long)ss.__x[0], (unsigned long long)ss.__x[1],
            (unsigned long long)ss.__x[2], (unsigned long long)ss.__x[3]);
    fprintf(stderr, "        x4=%#llx x5=%#llx x6=%#llx x7=%#llx\n",
            (unsigned long long)ss.__x[4], (unsigned long long)ss.__x[5],
            (unsigned long long)ss.__x[6], (unsigned long long)ss.__x[7]);
    for (int base = 8; base < 16; base += 4) {
        fprintf(stderr, "        ");
        for (int i = base; i < base + 4; i++)
            fprintf(stderr, "x%d=%#llx ", i, (unsigned long long)ss.__x[i]);
        fprintf(stderr, "\n");
    }
    for (int base = 16; base < 32; base += 4) {
        fprintf(stderr, "        ");
        for (int i = base; i < base + 4; i++)
            fprintf(stderr, "x%d=%#llx ", i, (unsigned long long)ss.__x[i]);
        fprintf(stderr, "\n");
    }

    /* frame-pointer chain */
    uint64_t f = fp;
    for (int i = 0; i < 24 && f >= 0x100000000ULL && f < 0x800000000000ULL && !(f & 0xf); i++) {
        uint64_t prev = *(uint64_t *)f;
        uint64_t ret = *(uint64_t *)(f + 8);
        if (ret) fprintf(stderr, "        #%d  ret=%#llx (%s)\n", i,
                         (unsigned long long)ret, pb_where(ret));
        if (prev <= f) break;
        f = prev;
    }

    /* last resort: raw stack */
    uint64_t *st = (uint64_t *)sp;
    fprintf(stderr, "        stack:");
    for (int i = 0; i < 10; i++) {
        uint64_t v = 0;
        if (sp >= 0x100000000ULL) v = st[i];
        fprintf(stderr, " %#llx", (unsigned long long)v);
    }
    fprintf(stderr, "\n");

    /* extra guest offsets to print, e.g. PB_FAULT_EXTRA=0x396458,0x1549ad8 */
    const char *extra = getenv("PB_FAULT_EXTRA");
    if (extra && PB.base) {
        char buf[256];
        snprintf(buf, sizeof buf, "%s", extra);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ", ", &save); tok;
             tok = strtok_r(NULL, ", ", &save)) {
            unsigned long off = strtoul(tok, NULL, 0);
            uint64_t *p = (uint64_t *)(PB.base + off);
            fprintf(stderr, "        extra[%#lx] = %#llx\n",
                    off, (unsigned long long)*p);
        }
    }
    _exit(77);
}

static const char *default_so_paths[] = {
    "libpayback2.patched.so",
    "native/lib/arm64-v8a/libpayback2.patched.so",
    "../native/lib/arm64-v8a/libpayback2.patched.so",
    "/var/folders/gf/1tjhg30n08nbbz5g7sz9gsyw0000gn/T/opencode/payback/native/lib/arm64-v8a/libpayback2.patched.so",
    NULL,
};

static const char *find_so(void)
{
    for (int i = 0; default_so_paths[i]; i++)
        if (access(default_so_paths[i], R_OK) == 0)
            return default_so_paths[i];
    return NULL;
}

/* ---- PB_BRKS: guest breakpoints -----------------------------------------
 * PB_BRKS=0xOFF[,0xOFF...] patches `brk #0` at image-relative offsets.  On
 * SIGTRAP the handler prints LR (the exact caller), registers and a frame
 * chain, then restores the original word (one shot; PB_BRKS_REPEAT keeps it
 * armed).  This is the tool for climbing call paths in the rehoused engine. */
static struct { uint64_t off; uint32_t orig; int hits; } g_brks[16];
static int g_n_brks;

static void brk_mprotect(uint64_t addr, int writable)
{
    uintptr_t page = addr & ~(uintptr_t)0x3fff;
    mprotect((void *)page, 0x4000,
             writable ? (PROT_READ | PROT_WRITE) : (PROT_READ | PROT_EXEC));
}

static void pb_brks_arm(const char *e)
{
    if (!e || !*e || !PB.base) return;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", e);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ", ", &save); tok && g_n_brks < 16;
         tok = strtok_r(NULL, ", ", &save)) {
        uint64_t off = strtoull(tok, NULL, 0);
        uint32_t *p = (uint32_t *)(PB.base + off);
        g_brks[g_n_brks].off = off;
        g_brks[g_n_brks].orig = *p;
        g_brks[g_n_brks].hits = 0;
        brk_mprotect((uint64_t)p, 1);
        *p = 0xD4200000u;                        /* brk #0 */
        __builtin___clear_cache((char *)p, (char *)p + 4);
        brk_mprotect((uint64_t)p, 0);
        fprintf(stderr, "[brk] armed off=%#llx (was %#x)\n",
                (unsigned long long)off, g_brks[g_n_brks].orig);
        g_n_brks++;
    }
    fflush(stderr);
}

/* PB_BRKS_AT=<sec>:<offsets>: arm the breakpoints only after <sec> seconds.
 * Needed for state that only diverges late (e.g. a poll whose value flips
 * after the front-end has been up for a while). */
static void *pb_brks_at_thread(void *arg)
{
    char *spec = strdup((const char *)arg);
    char *c = strchr(spec, ':');
    if (!c) { free(spec); return NULL; }
    *c = 0;
    sleep((unsigned)atoi(spec));
    pb_brks_arm(c + 1);
    free(spec);
    return NULL;
}

static void pb_brks_init(void)
{
    pb_brks_arm(getenv("PB_BRKS"));
    const char *e = getenv("PB_BRKS_AT");
    if (e && *e) {
        pthread_t t;
        pthread_create(&t, NULL, pb_brks_at_thread, (void *)e);
        pthread_detach(t);
    }
}

/* ---- PB_POKE: raw word patches into the loaded image -------------------
 * PB_POKE=0xOFF=0xWORD[,0xOFF=0xWORD...] applied once, after relocation
 * and before the ctors run.  Used to neutralise single branches while
 * chasing behaviour differences (e.g. 0x18f76c cbnz -> nop). */
static void pb_pokes_init(void)
{
    const char *e = getenv("PB_POKE");
    if (!e || !*e || !PB.base) return;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", e);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ", ", &save); tok;
         tok = strtok_r(NULL, ", ", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        uint64_t off = strtoull(tok, NULL, 0);
        uint32_t val = (uint32_t)strtoul(eq + 1, NULL, 0);
        uint32_t *p = (uint32_t *)(PB.base + off);
        fprintf(stderr, "[poke] %#llx: %#x -> %#x\n", (unsigned long long)off, *p, val);
        brk_mprotect((uint64_t)p, 1);
        *p = val;
        __builtin___clear_cache((char *)p, (char *)p + 4);
        brk_mprotect((uint64_t)p, 0);
    }
    fflush(stderr);
}

static void pb_brks_restore(int i)
{
    uint32_t *p = (uint32_t *)(PB.base + g_brks[i].off);
    brk_mprotect((uint64_t)p, 1);
    *p = g_brks[i].orig;
    __builtin___clear_cache((char *)p, (char *)p + 4);
    brk_mprotect((uint64_t)p, 0);
}

static void pb_trap(int sig, siginfo_t *si, void *uctx)
{
    (void)sig; (void)si;
    ucontext_t *uc = uctx;
    struct __darwin_arm_thread_state64 *ssp = &uc->uc_mcontext->__ss;
    uint64_t pc = (uint64_t)ssp->__pc;
    uint64_t gb = (uint64_t)PB.base, ge = gb + PB.span;
    for (int i = 0; i < g_n_brks; i++) {
        if (pc != (uint64_t)(PB.base + g_brks[i].off)) continue;
        g_brks[i].hits++;
        uint64_t lr = (uint64_t)ssp->__lr;
        fprintf(stderr, "[brk] #%d off=%#llx hit lr=%#llx",
                g_brks[i].hits, (unsigned long long)g_brks[i].off,
                (unsigned long long)lr);
        if (lr >= gb && lr < ge)
            fprintf(stderr, " (ret into g+%#lx)", (unsigned long)(lr - gb));
        fprintf(stderr, "\n      lr host symbol: %s", pb_where(lr));
        fprintf(stderr, " sp=%#llx x0=%#llx x1=%#llx x2=%#llx x3=%#llx x8=%#llx x9=%#llx x19=%#llx x20=%#llx\n",
                (unsigned long long)ssp->__sp,
                (unsigned long long)ssp->__x[0], (unsigned long long)ssp->__x[1],
                (unsigned long long)ssp->__x[2], (unsigned long long)ssp->__x[3],
                (unsigned long long)ssp->__x[8], (unsigned long long)ssp->__x[9],
                (unsigned long long)ssp->__x[19], (unsigned long long)ssp->__x[20]);
        {
            uint64_t f = (uint64_t)ssp->__fp;
            char out[512];
            int o = snprintf(out, sizeof out, "      chain:");
            for (int k = 0; k < 20 && f >= 0x100000000ULL && f < 0x800000000000ULL && !(f & 7); k++) {
                uint64_t ret = 0;
                memcpy(&ret, (void *)(f + 8), 8);
                if (ret >= gb && ret < ge)
                    o += snprintf(out + o, sizeof out - (size_t)o, " g+%#lx", (unsigned long)(ret - gb));
                else if (ret)
                    o += snprintf(out + o, sizeof out - (size_t)o, " %#lx", (unsigned long)ret);
                if (o > (int)sizeof out - 32) break;
                uint64_t prev = 0;
                memcpy(&prev, (void *)f, 8);
                if (prev <= f) break;
                f = prev;
            }
            fprintf(stderr, "%s\n", out);
        }
        fflush(stderr);
        /* If the original instruction was restored we must re-execute it;
         * otherwise (repeat mode) the word is still `brk #0` and we skip. */
        if (g_brks[i].hits >= (getenv("PB_BRKS_REPEAT") ? 100000000 : 1)) {
            pb_brks_restore(i);
        } else {
            ssp->__pc = pc + 4;                  /* skip the brk */
        }
        return;
    }
    fprintf(stderr, "[brk] unhandled trap pc=%#llx (%s)\n",
            (unsigned long long)pc, pb_where(pc));
    fflush(stderr);
    _exit(78);
}
/* ---- PB_PCSAMPLE: sample every thread's PC once a second ---------------
 * Uses Mach thread_get_state to see where each OS thread is executing.
 * Guest (image) PCs are printed image-relative; host PCs get a symbol. */
static void *pb_sampler(void *arg)
{
    (void)arg;
    uintptr_t gb = (uintptr_t)PB.base, ge = gb + PB.span;
    int max_t = 90;
    const char *e = getenv("PB_PCSAMPLE");
    if (e && *e && atoi(e) > 0) max_t = atoi(e);
    for (int t = 1; t <= max_t; t++) {
        usleep(1000000);
        thread_act_array_t ths = NULL;
        mach_msg_type_number_t n = 0;
        if (task_threads(mach_task_self(), &ths, &n) != KERN_SUCCESS) continue;
        struct { uint64_t pc, lr; int cnt; } agg[80];
        int nagg = 0;
        for (mach_msg_type_number_t i = 0; i < n; i++) {
            struct __darwin_arm_thread_state64 st;
            mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
            if (thread_get_state(ths[i], ARM_THREAD_STATE64,
                                 (thread_state_t)&st, &cnt) != KERN_SUCCESS) continue;
            uint64_t pc = (uint64_t)st.__pc;
            int found = 0;
            for (int k = 0; k < nagg; k++)
                if (agg[k].pc == pc) { agg[k].cnt++; found = 1; break; }
            if (!found && nagg < 80) {
                agg[nagg].pc = pc;
                agg[nagg].lr = (uint64_t)st.__lr;
                agg[nagg].cnt = 1;
                nagg++;
            }
        }
        fprintf(stderr, "[sample] t=%d nthreads=%u distinct=%d\n", t, (unsigned)n, nagg);
        for (int k = 0; k < nagg; k++) {
            uint64_t pc = agg[k].pc, lr = agg[k].lr;
            if (pc >= gb && pc < ge)
                fprintf(stderr, "    x%-3d pc=g+%#lx lr=g+%#lx\n", agg[k].cnt,
                        (unsigned long)(pc - gb), (unsigned long)(lr - gb));
            else
                fprintf(stderr, "    x%-3d pc=%s (lr %s)\n", agg[k].cnt,
                        pb_where(pc), pb_where(lr));
        }
        fflush(stderr);
        for (mach_msg_type_number_t i = 0; i < n; i++)
            mach_port_deallocate(mach_task_self(), ths[i]);
        vm_deallocate(mach_task_self(), (vm_address_t)ths, n * sizeof(thread_act_t));
    }
    return NULL;
}

/* ---- PB_PHRASE_DUMP: dump the engine's own text corpus ----------------
 * The engine pushes every phrase to Java by index via ANDROID_StorePhrase;
 * its getter at img+0x198370(index) returns the UTF-8 text.  Call it from
 * an idle host thread after boot and write /tmp/pb_phrases.txt. */
static void *pb_phrase_dump_thread(void *arg)
{
    (void)arg;
    int delay = 14, maxi = 4000;
    const char *d = getenv("PB_PHRASE_DELAY");
    if (d && *d) delay = atoi(d);
    const char *m = getenv("PB_PHRASE_MAX");
    if (m && *m) maxi = atoi(m);
    sleep((unsigned)delay);
    uint64_t fn = (uint64_t)PB.base + 0x198370;
    /* The engine's own phrase tables: blob pointer at globals 0x1549ac8/0x1549ad0,
     * index table bank n at 0x15569e0 + n*10800 (6 blocks of 450 u32 offsets). */
    uint64_t blob = 0;
    uint32_t bsize = 0, blk_off = 0;
    memcpy(&blob, (char *)PB.base + 0x1549ac8, 8);
    memcpy(&bsize, (char *)PB.base + 0x1549ad0, 4);
    memcpy(&blk_off, (char *)PB.base + 0x396458, 4);
    fprintf(stderr, "[phrase-dump] blob=%#llx size=%u bank=%u lang=%d var=%d\n",
            (unsigned long long)blob, bsize, blk_off,
            (int)*(int32_t *)((char *)PB.base + 0x3afe90),
            (int)*(int32_t *)((char *)PB.base + 0x3afe94));
    if (blob) {
        FILE *rb = fopen("/tmp/pb_phrase_blob.bin", "wb");
        if (rb && bsize && bsize < (64u << 20)) { fwrite((void *)(uintptr_t)blob, 1, bsize, rb); }
        if (rb) fclose(rb);
        FILE *tb = fopen("/tmp/pb_texttab.bin", "wb");
        if (tb) { fwrite((void *)((char *)PB.base + 0x15569e0), 1, 0x20000, tb); fclose(tb); }
        FILE *tf = fopen("/tmp/pb_texttabs.txt", "w");
        for (int b = 0; b < 6; b++) {
            for (int blk = 0; blk < 6; blk++) {
                uint32_t *e = (uint32_t *)((char *)PB.base + 0x15569e0 + b * 10800 + blk * 1800);
                int good = 0;
                char first[96] = "";
                for (int i = 0; i < 450; i++) {
                    uint32_t o = e[i];
                    if (o < bsize) {
                        const char *sp = (const char *)(uintptr_t)(blob + o);
                        if (sp[0] && (unsigned char)sp[0] >= 0x20) {
                            if (!good) snprintf(first, sizeof first, "%s", sp);
                            good++;
                        }
                    }
                }
                if (tf) fprintf(tf, "bank=%d block=%d (lang=%d var=%d) good=%d first=%s\n",
                                b, blk, blk >> 1, blk & 1, good, first);
                fprintf(stderr, "[phrase-tab] bank=%d block=%d lang=%d var=%d good=%d first=%s\n",
                        b, blk, blk >> 1, blk & 1, good, first);
            }
        }
        if (tf) fclose(tf);
    }
    FILE *f = fopen("/tmp/pb_phrases.txt", "w");
    int n = 0;
    for (int i = 0; i <= maxi; i++) {
        const char *t = (const char *)(uintptr_t)pb_call((void *)fn, (uint64_t)i, 0, 0, 0);
        if (!t || !*t) continue;
        n++;
        if (f) fprintf(f, "%d\t%s\n", i, t);
    }
    if (f) fclose(f);
    fprintf(stderr, "[phrase-dump] %d non-empty phrases -> /tmp/pb_phrases.txt\n", n);
    fflush(stderr);
    return NULL;
}

int main(int argc, char **argv)
{
    setvbuf(stderr, NULL, _IONBF, 0);

    struct sigaction sa = { 0 };
    sa.sa_sigaction = pb_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    {
        struct sigaction st = { 0 };
        st.sa_sigaction = pb_trap;
        st.sa_flags = SA_SIGINFO;
        sigaction(SIGTRAP, &st, NULL);
    }

    const char *so = argc > 1 ? argv[1] : find_so();
    if (!so) { fprintf(stderr, "usage: pbnative <libpayback2.patched.so> [engine]\n"); return 2; }
    pb_setup_data_dir();

    /* register every shim BEFORE wiring imports */
    pb_gvar_register();
    pb_stdio_init();
    pb_stdio_register();
    pb_shim_install_libc();
    pb_shim_install_stubs();
    pb_shim_install_egl();      /* real EGL/GLES2 first: registry is first-wins */
    pb_shim_install_opensl();   /* minimal OpenSL ES (silent audio)             */
    pb_shim_install_android();
    pb_pthread_register();

    pb_tls_check("before load");
    if (pb_load(so) != 0) return 1;
    pb_prot_init();
    pb_tls_check("after load");

    pb_jni_init();
    pb_tls_check("after jni_init");

    if (getenv("PB_DUMP_STAGES")) {
        bool d = false;
        void *t = pb_shim_lookup("SL_IID_ENGINE", &d);
        pb_log("[stage] after load: slot38dc68=%#llx lookup(ENGINE)=%p data=%d\n",
               (unsigned long long)*(uint64_t *)(PB.base + 0x38dc68), t, d);
    }

    pb_relocate();
    pb_tls_check("after relocate");
    if (getenv("PB_DUMP_STAGES"))
        pb_log("[stage] after relocate: slot38dc68=%#llx\n",
               (unsigned long long)*(uint64_t *)(PB.base + 0x38dc68));
    pb_wire_imports();
    pb_opensl_fixup_slots();
    pb_jni_lang_fixup();
    if (getenv("PB_DUMP_STAGES"))
        pb_log("[stage] after wire: slot38dc68=%#llx\n",
               (unsigned long long)*(uint64_t *)(PB.base + 0x38dc68));
    pb_shim_report_missing();
    pb_tls_check("after wire");

    if (getenv("PB_DUMP_SLOT")) {
        unsigned long off = strtoul(getenv("PB_DUMP_SLOT"), NULL, 0);
        void *slot = PB.base + off;
        pb_log("[dump] base=%p slot[%#lx]=%#llx\n",
               PB.base, off, *(unsigned long long *)slot);
    }

    pb_finish();   /* mprotect + run ctors */
    pb_tls_check("after ctors");
    pb_pokes_init();
    pb_brks_init();
    pb_dump_at_init(getenv("PB_DUMP_AT"));
    pb_dump_at_init(getenv("PB_DUMP_AT2"));
    pb_dump_at_init(getenv("PB_DUMP_AT3"));
    if (getenv("PB_PCSAMPLE")) {
        pthread_t st;
        pthread_create(&st, NULL, pb_sampler, NULL);
    }

    /* Screen brightness (0..255) consumed by the CRT post-process at 0x123344:
     * with F1/F2 set, cb4 = brightness*170/256 and the composite vertex
     * colours are built from it.  Zero => fully black output. */
    {
        const char *br = getenv("PB_BRIGHT");
        if (br && PB.base) {
            uint32_t v = (uint32_t)strtoul(br, NULL, 0);
            *(uint32_t *)((char *)PB.base + 0x19e8cf8) = v;
            pb_log("[boot] PB_BRIGHT=%u -> [0x19e8cf8]=%u (readback %u)\n", v, v,
                   *(uint32_t *)((char *)PB.base + 0x19e8cf8));
        }
    }

    if (getenv("PB_DUMP_SLOT")) {
        unsigned long off = strtoul(getenv("PB_DUMP_SLOT"), NULL, 0);
        void *slot = PB.base + off;
        pb_log("[dump] after ctors: slot[%#lx]=%#llx\n", off, *(unsigned long long *)slot);
    }

    void *env = pb_jni_env();
    void *vm = pb_jni_vm();

    if (PB.jni_onload) {
        pb_log("[boot] calling JNI_OnLoad(%p, NULL)\n", vm);
        uint64_t r = pb_call(PB.jni_onload, (uint64_t)(uintptr_t)vm, 0, 0, 0);
        pb_log("[boot] JNI_OnLoad -> %#llx (want 0x10006)\n", (unsigned long long)r);
    } else {
        pb_log("[boot] no JNI_OnLoad exported!\n");
    }

    /* PairIP normally plants these; the harness had to seed them too. */
    *(uint64_t *)pb_global(0x3b2120) = (uint64_t)(uintptr_t)env;
    *(uint64_t *)pb_global(0x3b2208) = pb_jni_mint_id();
    pb_log("[boot] seeded [0x3b2120]=env %p, [0x3b2208]=%#llx\n",
           env, (unsigned long long)pb_jni_mint_id());

    if (argc > 2 && !strcmp(argv[2], "engine")) {
        int headless = getenv("PB_HEADLESS") != NULL;
        if (!headless) pb_window_create(1920, 1080, "Payback 2");
        /* 0x31c908 = internal initializeNativeCode(JNIEnv*, jobject, jstring,
         * jstring, jstring, jobject, jbyteArray, jobject) -> jlong Game*.
         * The exported Java_..._initializeNativeCode at 0x31c894 is only a
         * JNI thunk that tail-calls it (plus a Game SDK pre-call); the harness
         * drove 0x31c908 directly and so does the port. */
        void *init = PB.base + 0x31C908;
        pb_log("[boot] initializeNativeCode = %p\n", init);
        if (init) {
            void *activity = (void *)(uintptr_t)0x60200010;   /* handle-ish jobject */
            void *asset_mgr = (void *)(uintptr_t)0x60200020;
            uint64_t j_files = pb_jni_handle_string(pb_files_dir);
            uint64_t j_obb = pb_jni_handle_string(pb_obb_dir);
            uint64_t j_ext = pb_jni_handle_string(pb_external_dir);
            uint64_t j_cfg = pb_jni_new_object("android/content/res/Configuration");
            uint64_t game = pb_call8(init, (uint64_t)(uintptr_t)env, (uint64_t)(uintptr_t)activity,
                                     j_files, j_obb, j_ext, (uint64_t)(uintptr_t)asset_mgr, 0, j_cfg);
            pb_log("[boot] initializeNativeCode -> Game = %#llx\n", (unsigned long long)game);
            pb_input_attach(game);
            if (getenv("PB_TAP")) {
                pthread_t tt;
                pthread_create(&tt, NULL, pb_tap_thread, (void *)getenv("PB_TAP"));
                pthread_detach(tt);
            }
            if (getenv("PB_MOVE")) {
                pthread_t tm;
                pthread_create(&tm, NULL, pb_move_thread, (void *)getenv("PB_MOVE"));
                pthread_detach(tm);
            }
            if (getenv("PB_DRAG")) {
                pthread_t td;
                pthread_create(&td, NULL, pb_drag_thread, (void *)getenv("PB_DRAG"));
                pthread_detach(td);
            }

            /* PaybackActivity's callback order (mirrors the harness drive) */
            uint64_t SURFACE = 0x60200040;
            struct { const char *nm; uint64_t off; uint64_t a0, a1, a2, a3; } steps[] = {
                { "onStartNative",              0x31D0A8, 0, 0, 0, 0 },
                { "onResumeNative",             0x31D0C0, 0, 0, 0, 0 },
                { "onWindowFocusChangedNative", 0x31D1DC, 1, 0, 0, 0 },
                { "onSurfaceCreatedNative",     0x31D1FC, SURFACE, 0, 0, 0 },
                { "onSurfaceChangedNative",     0x31D264, SURFACE, 1920, 1080, 1 },
            };
            for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
                uint64_t r = pb_call8(PB.base + steps[i].off,
                                      (uint64_t)(uintptr_t)env, 0, game,
                                      steps[i].a0, steps[i].a1, steps[i].a2, steps[i].a3, 0);
                pb_log("[engine] %-28s -> %#llx\n", steps[i].nm, (unsigned long long)r);
                usleep(200000);   /* give the game thread time to consume */
            }
            const char *hold = getenv("PB_HOLD");
            if (getenv("PB_PHRASE_DUMP")) {
                pthread_t pt;
                pthread_create(&pt, NULL, pb_phrase_dump_thread, NULL);
                pthread_detach(pt);
            }
            if (getenv("PB_MSCAN")) {
                pthread_t sc;
                pthread_create(&sc, NULL, pb_scan_thread, NULL);
                pthread_detach(sc);
            }
            if (getenv("PB_WATCH")) {
                pthread_t wt;
                pthread_create(&wt, NULL, pb_watch_thread, NULL);
                pthread_detach(wt);
            }
            if (getenv("PB_SAMPLE")) {
                pthread_t st;
                pthread_create(&st, NULL, pb_sample_thread, (void *)getenv("PB_SAMPLE"));
                pthread_detach(st);
            }
            pb_dump_ranges();
            if (!headless) {
                pb_log("[engine] entering window loop%s\n",
                       hold ? " (PB_HOLD timer armed)" : " (close the window to quit)");
                pb_window_run();
            } else {
                int hold_s = hold ? atoi(hold) : 2;
                pb_log("[engine] drive complete; holding %ds to watch the game thread\n", hold_s);
                usleep((useconds_t)hold_s * 1000000);
            }
        }
    }

    pb_log("[boot] done (milestone 1)\n");
    return 0;
}
