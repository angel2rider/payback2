/* android_shim.c — the Android NDK surface over host primitives.
 *
 * ALooper is real: per-thread loopers over host poll(2) with a wake pipe, so
 * the engine's command pipe (app+0x118/0x11c) behaves exactly like on Android
 * now that the game loop runs on a real thread.
 * AConfiguration / ANativeWindow / ASensor mirror what the harness served
 * (en-GB locale, 1920x1080 window, no sensors).
 * EGL/GL return zero for now: the harness boot proved the engine tolerates a
 * dead GL stack (its GL init bails early and the loop still runs).  Real
 * EGL/GLES via ANGLE arrives in the windowed milestone.
 */
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pb.h"

/* ------------------------------------------------------------- ALooper */

#define ALOOPER_POLL_WAKE     (-1)
#define ALOOPER_POLL_CALLBACK (-2)
#define ALOOPER_POLL_TIMEOUT  (-3)
#define ALOOPER_POLL_ERROR    (-4)

#define ALOOPER_EVENT_INPUT   1
#define ALOOPER_EVENT_OUTPUT  2
#define ALOOPER_EVENT_ERROR   4
#define ALOOPER_EVENT_HANGUP  8
#define ALOOPER_EVENT_INVALID 16

#define LOOPER_MAX_FDS 32

typedef struct looper_fd {
    int fd, ident, events;
    void *cb, *data;
    struct looper_fd *next;
} looper_fd;

typedef struct {
    looper_fd *fds;
    int wake_r, wake_w;
} looper_impl;

static pthread_key_t g_looper_key;
static pthread_once_t g_looper_once = PTHREAD_ONCE_INIT;

static void looper_dtor(void *p)
{
    looper_impl *L = p;
    if (!L) return;
    while (L->fds) { looper_fd *d = L->fds; L->fds = d->next; free(d); }
    if (L->wake_r >= 0) close(L->wake_r);
    if (L->wake_w >= 0) close(L->wake_w);
    free(L);
}

static void looper_make_key(void) { pthread_key_create(&g_looper_key, looper_dtor); }

static looper_impl *looper_cur(int create)
{
    pthread_once(&g_looper_once, looper_make_key);
    looper_impl *L = pthread_getspecific(g_looper_key);
    if (L || !create) return L;
    L = calloc(1, sizeof *L);
    L->wake_r = L->wake_w = -1;
    int p[2];
    if (pipe(p) != 0) { free(L); return NULL; }
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    fcntl(p[1], F_SETFL, O_NONBLOCK);
    L->wake_r = p[0]; L->wake_w = p[1];
    pthread_setspecific(g_looper_key, L);
    return L;
}

static void *sh_ALooper_prepare(int opts) { (void)opts; return looper_cur(1); }
static void *sh_ALooper_forThread(void)   { return looper_cur(1); }
static void  sh_noop1(void *a)            { (void)a; }

static int sh_ALooper_addFd(void *lp, int fd, int ident, int events, void *cb, void *data)
{
    looper_impl *L = lp ? lp : looper_cur(1);
    if (!L || fd < 0) return -1;
    looper_fd *r = calloc(1, sizeof *r);
    r->fd = fd; r->ident = ident; r->events = events; r->cb = cb; r->data = data;
    r->next = L->fds;
    L->fds = r;
    return 1;
}

static int sh_ALooper_removeFd(void *lp, int fd)
{
    looper_impl *L = lp ? lp : looper_cur(0);
    if (!L) return -1;
    looper_fd **pp = &L->fds;
    while (*pp) {
        if ((*pp)->fd == fd) { looper_fd *d = *pp; *pp = d->next; free(d); return 1; }
        pp = &(*pp)->next;
    }
    return -1;
}

static void sh_ALooper_wake(void *lp)
{
    looper_impl *L = lp ? lp : looper_cur(1);
    if (!L) return;
    char b = 1;
    ssize_t n = write(L->wake_w, &b, 1);
    (void)n;
}

static int looper_poll(void *lp, int timeout, int *out_fd, int *out_events, void **out_data)
{
    looper_impl *L = lp ? lp : looper_cur(1);
    if (!L) return ALOOPER_POLL_ERROR;

    struct pollfd pfd[1 + LOOPER_MAX_FDS];
    looper_fd *map[1 + LOOPER_MAX_FDS];
    int n = 0;

    pfd[n].fd = L->wake_r; pfd[n].events = POLLIN; pfd[n].revents = 0; map[n] = NULL; n++;

    int count = 0;
    for (looper_fd *r = L->fds; r && count < LOOPER_MAX_FDS; r = r->next, count++) {
        pfd[n].fd = r->fd;
        pfd[n].events = 0;
        if (r->events & ALOOPER_EVENT_INPUT)  pfd[n].events |= POLLIN;
        if (r->events & ALOOPER_EVENT_OUTPUT) pfd[n].events |= POLLOUT;
        pfd[n].revents = 0;
        map[n] = r;
        n++;
    }

    int rc = poll(pfd, n, timeout);
    if (rc < 0) return ALOOPER_POLL_ERROR;
    if (rc == 0) return ALOOPER_POLL_TIMEOUT;

    if (pfd[0].revents & POLLIN) {                 /* wake pipe */
        char buf[64];
        while (read(L->wake_r, buf, sizeof buf) > 0) {}
        return ALOOPER_POLL_WAKE;
    }

    for (int i = 1; i < n; i++) {
        if (!pfd[i].revents) continue;
        looper_fd *r = map[i];
        int ev = 0;
        if (pfd[i].revents & POLLIN)  ev |= ALOOPER_EVENT_INPUT;
        if (pfd[i].revents & POLLOUT) ev |= ALOOPER_EVENT_OUTPUT;
        if (pfd[i].revents & POLLHUP) ev |= ALOOPER_EVENT_HANGUP;
        if (pfd[i].revents & (POLLERR | POLLNVAL)) ev |= ALOOPER_EVENT_ERROR;
        if (r->cb) {
            int (*cb)(int, int, void *) = (int (*)(int, int, void *))r->cb;
            return cb(r->fd, ev, r->data);
        }
        if (out_fd) *out_fd = r->ident;
        if (out_events) *out_events = ev;
        if (out_data) *out_data = r->data;
        return r->ident;
    }
    return ALOOPER_POLL_TIMEOUT;
}

static int sh_ALooper_pollOnce(int timeout_ms, int *out_fd, int *out_events, void **out_data)
{
    return looper_poll(looper_cur(1), timeout_ms, out_fd, out_events, out_data);
}

static int sh_ALooper_pollAll(int timeout_ms, int *out_fd, int *out_events, void **out_data)
{
    for (;;) {
        int r = looper_poll(looper_cur(1), timeout_ms, out_fd, out_events, out_data);
        if (r != ALOOPER_POLL_CALLBACK) return r;
    }
}

/* ------------------------------------------------------------- AAsset */

static int g_asset_token;

static void *sh_AAssetManager_fromJava(void *env, void *mgr)
{
    (void)env;
    return mgr ? mgr : &g_asset_token;
}

typedef struct {
    FILE *f;
    uint8_t *buf;
    size_t len, pos;
    unsigned long long nread;
    char path[256];
} asset_impl;

static const char *assets_root(void) { return getenv("PB_ASSETS"); }

static void *sh_AAssetManager_open(void *mgr, const char *path, int mode)
{
    (void)mgr; (void)mode;
    const char *root = assets_root();
    if (!root || !path) return NULL;
    char full[4096];
    snprintf(full, sizeof full, "%s/%s", root, path);
    FILE *f = fopen(full, "rb");
    if (getenv("PB_TRACE_ASSET"))
        fprintf(stderr, "[asset] t=0x%llx open %s -> %s ret=%p %p\n",
                (unsigned long long)pb_cur_guest_thread(), path, f ? "ok" : "NULL",
                __builtin_return_address(0), __builtin_return_address(1));
    if (strstr(path, "Maps/") && getenv("PB_TRACE_ASSET") && !strcmp(getenv("PB_TRACE_ASSET"), "2")) {
        static int n_bt;
        if (n_bt < 3) { n_bt++; pb_backtrace(path); }
    }
    if (!f) return NULL;
    asset_impl *a = calloc(1, sizeof *a);
    a->f = f;
    if (path) { strncpy(a->path, path, sizeof a->path - 1); }
    fseek(f, 0, SEEK_END);
    a->len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    return a;
}

static long long sh_AAsset_getLength(void *h)
{ asset_impl *a = h; return a ? (long long)a->len : 0; }

static long long sh_AAsset_getRemainingLength(void *h)
{ asset_impl *a = h; return a ? (long long)(a->len - a->pos) : 0; }

static int sh_AAsset_read(void *h, void *buf, size_t n)
{
    asset_impl *a = h;
    if (!a || !a->f) return -1;
    size_t want = n;
    if (n > a->len - a->pos) n = a->len - a->pos;
    size_t r = fread(buf, 1, n, a->f);
    a->pos += r;
    a->nread += r;
    if (getenv("PB_TRACE_ASSET") && !strcmp(getenv("PB_TRACE_ASSET"), "2"))
        fprintf(stderr, "[asset] read %s want=%zu got=%zu pos=%zu/%zu\n",
                a->path, want, r, a->pos, a->len);
    return (int)r;
}

static long long sh_AAsset_seek(void *h, long long off, int whence)
{
    asset_impl *a = h;
    if (!a || !a->f) return -1;
    if (fseek(a->f, (long)off, whence) != 0) return -1;
    long p = ftell(a->f);
    if (p >= 0) a->pos = (size_t)p;
    return p;
}

static void sh_AAsset_close(void *h)
{
    asset_impl *a = h;
    if (!a) return;
    if (getenv("PB_TRACE_ASSET") && !strcmp(getenv("PB_TRACE_ASSET"), "2"))
        fprintf(stderr, "[asset] close %s read=%llu/%zu\n", a->path, a->nread, a->len);
    if (a->f) fclose(a->f);
    free(a->buf);
    free(a);
}

static const void *sh_AAsset_getBuffer(void *h)
{
    asset_impl *a = h;
    if (!a) return NULL;
    if (!a->buf) {
        a->buf = malloc(a->len ? a->len : 1);
        if (a->buf) {
            long save = ftell(a->f);
            fseek(a->f, 0, SEEK_SET);
            fread(a->buf, 1, a->len, a->f);
            fseek(a->f, save, SEEK_SET);
        }
    }
    return a->buf;
}

static int sh_AAsset_openFileDescriptor(void *h, long long *start, long long *len)
{
    asset_impl *a = h;
    if (!a || !a->f) return -1;
    if (start) *start = 0;
    if (len) *len = (long long)a->len;
    return dup(fileno(a->f));
}

/* ------------------------------------------------------ AConfiguration */

typedef struct { char lang[4], country[4]; } aconfig_impl;

static void *sh_AConfiguration_new(void)
{
    aconfig_impl *c = calloc(1, sizeof *c);
    strcpy(c->lang, "en");
    strcpy(c->country, "GB");
    return c;
}

static void sh_AConfiguration_delete(void *c) { free(c); }
static void sh_AConfiguration_fromAssetManager(void *c, void *am) { (void)c; (void)am; }

static void sh_AConfiguration_getLanguage(void *c, char *out)
{
    (void)c;
    if (out) { out[0] = 'e'; out[1] = 'n'; out[2] = 0; }
}
static void sh_AConfiguration_getCountry(void *c, char *out)
{
    (void)c;
    if (out) { out[0] = 'G'; out[1] = 'B'; out[2] = 0; }
}
static void sh_AConfiguration_setToDefaults(void *c)
{
    aconfig_impl *a = c;
    if (a) { strcpy(a->lang, "en"); strcpy(a->country, "GB"); }
}
static void sh_AConfiguration_getDensity(void *c, int *out) { (void)c; if (out) *out = 160; }
static void sh_AConfiguration_getScreenWidthDp(void *c, int *out) { (void)c; if (out) *out = 960; }
static void sh_AConfiguration_getScreenHeightDp(void *c, int *out) { (void)c; if (out) *out = 540; }
static void sh_AConfiguration_getSdkVersion(void *c, int *out) { (void)c; if (out) *out = 34; }
static void sh_AConfiguration_getLayoutDirection(void *c, int *out) { (void)c; if (out) *out = 0; }

/* ------------------------------------------------------- ANativeWindow */

static int g_win_w = 1920, g_win_h = 1080;
static int g_window_token;

static void *sh_ANativeWindow_fromSurface(void *env, void *surface)
{
    (void)env; (void)surface;
    return &g_window_token;
}
static int  sh_ANativeWindow_getWidth(void *w)  { (void)w; return g_win_w; }
static int  sh_ANativeWindow_getHeight(void *w) { (void)w; return g_win_h; }
static void sh_no_w1(void *w) { (void)w; }
static int  sh_ret0_ni(void *w, int a) { (void)w; (void)a; return 0; }

/* ------------------------------------------------------------ ASensor */

static int g_sensor_token;

/* -------------------------------------------------------------- EGL/GL */

static uint64_t sh_ret0(void) { return 0; }

/* ------------------------------------------------------- registration */

void pb_shim_install_android(void)
{
    /* ALooper — real loopers over host poll() */
    pb_shim_register("ALooper_prepare",     (void *)sh_ALooper_prepare, false);
    pb_shim_register("ALooper_forThread",   (void *)sh_ALooper_forThread, false);
    pb_shim_register("ALooper_acquire",     (void *)sh_noop1, false);
    pb_shim_register("ALooper_release",     (void *)sh_noop1, false);
    pb_shim_register("ALooper_addFd",       (void *)sh_ALooper_addFd, false);
    pb_shim_register("ALooper_removeFd",    (void *)sh_ALooper_removeFd, false);
    pb_shim_register("ALooper_pollOnce",    (void *)sh_ALooper_pollOnce, false);
    pb_shim_register("ALooper_pollAll",     (void *)sh_ALooper_pollAll, false);
    pb_shim_register("ALooper_wake",        (void *)sh_ALooper_wake, false);

    /* AAsset */
    pb_shim_register("AAssetManager_fromJava",   (void *)sh_AAssetManager_fromJava, false);
    pb_shim_register("AAssetManager_open",       (void *)sh_AAssetManager_open, false);
    pb_shim_register("AAsset_getLength",         (void *)sh_AAsset_getLength, false);
    pb_shim_register("AAsset_getLength64",       (void *)sh_AAsset_getLength, false);
    pb_shim_register("AAsset_getRemainingLength",(void *)sh_AAsset_getRemainingLength, false);
    pb_shim_register("AAsset_read",              (void *)sh_AAsset_read, false);
    pb_shim_register("AAsset_seek",              (void *)sh_AAsset_seek, false);
    pb_shim_register("AAsset_seek64",            (void *)sh_AAsset_seek, false);
    pb_shim_register("AAsset_close",             (void *)sh_AAsset_close, false);
    pb_shim_register("AAsset_getBuffer",         (void *)sh_AAsset_getBuffer, false);
    pb_shim_register("AAsset_openFileDescriptor",(void *)sh_AAsset_openFileDescriptor, false);

    /* AConfiguration */
    pb_shim_register("AConfiguration_new",         (void *)sh_AConfiguration_new, false);
    pb_shim_register("AConfiguration_delete",      (void *)sh_AConfiguration_delete, false);
    pb_shim_register("AConfiguration_fromAssetManager", (void *)sh_AConfiguration_fromAssetManager, false);
    pb_shim_register("AConfiguration_getLanguage", (void *)sh_AConfiguration_getLanguage, false);
    pb_shim_register("AConfiguration_getCountry",  (void *)sh_AConfiguration_getCountry, false);
    pb_shim_register("AConfiguration_setToDefaults", (void *)sh_AConfiguration_setToDefaults, false);
    pb_shim_register("AConfiguration_getDensity",  (void *)sh_AConfiguration_getDensity, false);
    pb_shim_register("AConfiguration_getScreenWidthDp", (void *)sh_AConfiguration_getScreenWidthDp, false);
    pb_shim_register("AConfiguration_getScreenHeightDp", (void *)sh_AConfiguration_getScreenHeightDp, false);
    pb_shim_register("AConfiguration_getSdkVersion", (void *)sh_AConfiguration_getSdkVersion, false);
    pb_shim_register("AConfiguration_getLayoutDirection", (void *)sh_AConfiguration_getLayoutDirection, false);

    /* ANativeWindow */
    pb_shim_register("ANativeWindow_fromSurface",    (void *)sh_ANativeWindow_fromSurface, false);
    pb_shim_register("ANativeWindow_getWidth",       (void *)sh_ANativeWindow_getWidth, false);
    pb_shim_register("ANativeWindow_getHeight",      (void *)sh_ANativeWindow_getHeight, false);
    pb_shim_register("ANativeWindow_acquire",        (void *)sh_no_w1, false);
    pb_shim_register("ANativeWindow_release",        (void *)sh_no_w1, false);
    pb_shim_register("ANativeWindow_setBuffersGeometry", (void *)sh_ret0_ni, false);

    /* ASensor — no sensors on this machine */
    pb_shim_register("ASensorManager_getInstance",      (void *)sh_ret0, false);
    pb_shim_register("ASensorManager_getDefaultSensor", (void *)sh_ret0, false);
    pb_shim_register("ASensorManager_createEventQueue", (void *)sh_ret0, false);
    pb_shim_register("ASensorEventQueue_disableSensor", (void *)sh_ret0, false);
    pb_shim_register("ASensorEventQueue_enableSensor",  (void *)sh_ret0, false);
    pb_shim_register("ASensorEventQueue_setEventRate",  (void *)sh_ret0, false);
    pb_shim_register("ASensorEventQueue_getEvents",     (void *)sh_ret0, false);
    (void)g_sensor_token;

    /* EGL / GLES2 — zero stubs for the headless milestone (harness-proven) */
    static const char *zeroes[] = {
        "eglGetDisplay", "eglInitialize", "eglChooseConfig", "eglGetConfigAttrib",
        "eglCreateWindowSurface", "eglCreateContext", "eglMakeCurrent", "eglSwapBuffers",
        "eglQuerySurface", "eglDestroySurface", "eglDestroyContext", "eglTerminate",
        "eglGetError", "eglGetCurrentContext", "eglGetCurrentSurface", "eglSwapInterval",
        "eglBindAPI", "eglSurfaceAttrib",
        "glActiveTexture", "glAttachShader", "glBindAttribLocation", "glBindBuffer",
        "glBindFramebuffer", "glBindRenderbuffer", "glBindTexture", "glBlendFunc",
        "glBufferData", "glBufferSubData", "glCheckFramebufferStatus", "glClear",
        "glClearColor", "glColorMask", "glCompileShader", "glCompressedTexImage2D",
        "glCreateProgram", "glCreateShader", "glCullFace", "glDeleteBuffers",
        "glDeleteFramebuffers", "glDeleteProgram", "glDeleteRenderbuffers",
        "glDeleteShader", "glDeleteTextures", "glDepthFunc", "glDepthMask", "glDisable",
        "glDisableVertexAttribArray", "glDrawArrays", "glDrawElements", "glEnable",
        "glEnableVertexAttribArray", "glFinish", "glFlush", "glFramebufferRenderbuffer",
        "glFramebufferTexture2D", "glFrontFace", "glGenerateMipmap", "glGenBuffers",
        "glGenFramebuffers", "glGenRenderbuffers", "glGenTextures", "glGetAttribLocation",
        "glGetError", "glGetIntegerv", "glGetProgramInfoLog", "glGetProgramiv",
        "glGetShaderInfoLog", "glGetShaderiv", "glGetString", "glGetUniformLocation",
        "glLinkProgram", "glPixelStorei", "glReadPixels", "glRenderbufferStorage",
        "glScissor", "glShaderSource", "glStencilFunc", "glStencilOp", "glTexImage2D",
        "glTexParameterf", "glTexParameteri", "glTexSubImage2D", "glUniform1f",
        "glUniform1i", "glUniform2f", "glUniform3f", "glUniform4f", "glUniform4fv",
        "glUniformMatrix4fv", "glUseProgram", "glVertexAttribPointer", "glViewport",
        "glClearDepthf", "glDepthRangef", "glLineWidth", "glPolygonOffset",
        "glValidateProgram", "glBlendFuncSeparate", "glBindVertexArray",
        "glDeleteVertexArrays", "glGenVertexArrays",
    };
    for (size_t i = 0; i < sizeof(zeroes) / sizeof(zeroes[0]); i++)
        pb_shim_register(zeroes[i], (void *)sh_ret0, false);
}
