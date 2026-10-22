/* android_shim_wasm.c — Payback 2 Web Port: Android NDK Shims.
 *
 * Implements AAssetManager, ALooper, AConfiguration, and logging
 * for the WebAssembly runtime.
 */
#include "pb_wasm.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ------------------------------------------------------------- AAsset */
typedef struct {
    int handle;
    size_t len;
    size_t pos;
    char path[256];
} wasm_asset;

static int g_asset_token;

static void *sh_AAssetManager_fromJava(void *env, void *mgr)
{
    (void)env;
    return mgr ? mgr : &g_asset_token;
}

static void *sh_AAssetManager_open(void *mgr, const char *path, int mode)
{
    (void)mgr; (void)mode;
    if (!path) return NULL;

#ifdef __EMSCRIPTEN__
    int h = js_asset_open(path);
    if (!h) return NULL;

    wasm_asset *a = (wasm_asset *)calloc(1, sizeof(wasm_asset));
    a->handle = h;
    a->len = js_asset_length(h);
    a->pos = 0;
    strncpy(a->path, path, sizeof(a->path) - 1);
    return a;
#else
    return NULL;
#endif
}

static long long sh_AAsset_getLength(void *h)
{
    wasm_asset *a = (wasm_asset *)h;
    return a ? (long long)a->len : 0;
}

static long long sh_AAsset_getRemainingLength(void *h)
{
    wasm_asset *a = (wasm_asset *)h;
    return a ? (long long)(a->len - a->pos) : 0;
}

static int sh_AAsset_read(void *h, void *buf, size_t n)
{
    wasm_asset *a = (wasm_asset *)h;
    if (!a || !buf || !n) return 0;

#ifdef __EMSCRIPTEN__
    int got = js_asset_read(a->handle, buf, (int)n);
    if (got > 0) a->pos += got;
    return got;
#else
    return 0;
#endif
}

static long long sh_AAsset_seek(void *h, long long off, int whence)
{
    wasm_asset *a = (wasm_asset *)h;
    if (!a) return -1;

    size_t npos = a->pos;
    if (whence == 0) npos = (size_t)off;                  /* SEEK_SET */
    else if (whence == 1) npos = (size_t)(a->pos + off); /* SEEK_CUR */
    else if (whence == 2) npos = (size_t)(a->len + off); /* SEEK_END */

    if (npos > a->len) npos = a->len;
    a->pos = npos;
    return (long long)a->pos;
}

static void sh_AAsset_close(void *h)
{
    wasm_asset *a = (wasm_asset *)h;
    if (!a) return;
#ifdef __EMSCRIPTEN__
    js_asset_close(a->handle);
#endif
    free(a);
}

/* ------------------------------------------------------------- Logging */
static int sh_log_print(int prio, const char *tag, const char *fmt, ...)
{
    (void)prio;
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("[%s] %s\n", tag ? tag : "PaybackNative", buf);
    return 0;
}

static void sh_log_assert(const char *cond, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[ASSERT][%s] %s (cond: %s)\n", tag ? tag : "Payback", buf, cond ? cond : "?");
}

/* ------------------------------------------------------------- ALooper */
static void *sh_ALooper_forThread(void) { return (void *)(uintptr_t)0x60300010; }
static void *sh_ALooper_prepare(int opts) { (void)opts; return (void *)(uintptr_t)0x60300010; }
static void  sh_ALooper_acquire(void *looper) { (void)looper; }
static void  sh_ALooper_release(void *looper) { (void)looper; }
static int   sh_ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData)
{
    (void)timeoutMillis; (void)outFd; (void)outEvents; (void)outData;
    return -1; /* ALOOPER_POLL_WAKE: triggers game input pump */
}
static void  sh_ALooper_wake(void *looper) { (void)looper; }

void pb_shim_install_android(void)
{
    pb_shim_register("AAssetManager_fromJava",   (void *)sh_AAssetManager_fromJava, false);
    pb_shim_register("AAssetManager_open",       (void *)sh_AAssetManager_open, false);
    pb_shim_register("AAsset_getLength",         (void *)sh_AAsset_getLength, false);
    pb_shim_register("AAsset_getLength64",       (void *)sh_AAsset_getLength, false);
    pb_shim_register("AAsset_getRemainingLength",(void *)sh_AAsset_getRemainingLength, false);
    pb_shim_register("AAsset_read",              (void *)sh_AAsset_read, false);
    pb_shim_register("AAsset_seek",              (void *)sh_AAsset_seek, false);
    pb_shim_register("AAsset_seek64",            (void *)sh_AAsset_seek, false);
    pb_shim_register("AAsset_close",             (void *)sh_AAsset_close, false);

    pb_shim_register("__android_log_print",      (void *)sh_log_print, false);
    pb_shim_register("__android_log_assert",     (void *)sh_log_assert, false);

    pb_shim_register("ALooper_forThread",        (void *)sh_ALooper_forThread, false);
    pb_shim_register("ALooper_prepare",          (void *)sh_ALooper_prepare, false);
    pb_shim_register("ALooper_acquire",          (void *)sh_ALooper_acquire, false);
    pb_shim_register("ALooper_release",          (void *)sh_ALooper_release, false);
    pb_shim_register("ALooper_pollOnce",         (void *)sh_ALooper_pollOnce, false);
    pb_shim_register("ALooper_wake",             (void *)sh_ALooper_wake, false);
}
