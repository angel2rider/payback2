#include <math.h>
/* egl_shim.c — real EGL/GLES2 for the port, on ANGLE (Metal backend).
 *
 * Registration order (main.c): pb_shim_install_egl() runs BEFORE
 * pb_shim_install_android(), and the shim registry (shims.c) is first-wins,
 * so every symbol registered here shadows the EGL/GL zero-stub fallbacks.
 * PB_HEADLESS=1 or a missing ANGLE => nothing registered => zeros as before.
 *
 * ANGLE is dlopen'ed from one of the well-known dirs on this machine
 * (PB_ANGLE_DIR overrides).  libGLESv2 is preloaded with RTLD_GLOBAL before
 * libEGL so libEGL's lazy leaf-name load resolves against the loaded image.
 *
 * Surfaces: our "window" surface is really a plain ANGLE pbuffer sized to
 * the port's engine buffer (1920x1080 by default).  eglSwapBuffers reads the
 * frame back (glReadPixels, bottom-up -> top-down row flip) and publishes it
 * to window.m, which blits it into the NSWindow's CALayer.  This ANGLE build
 * (2.1.27678, Metal) advertises EGL_ANGLE_iosurface_client_buffer but rejects
 * every IOSurface pbuffer/image variant (EGL_BAD_ATTRIBUTE / EGL_BAD_PARAMETER
 * were all observed in port/tools/angletest.c), so readback is the honest v1
 * path; retry zero-copy IOSurface with a newer ANGLE.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pb.h"
#include <pthread.h>

typedef void *EGLDisplay, *EGLSurface, *EGLContext, *EGLConfig, *EGLClientBuffer;
typedef int32_t EGLint;
typedef uint32_t EGLBoolean, EGLenum;

#define EGL_DEFAULT_DISPLAY ((void *)0)
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_FALSE 0
#define EGL_TRUE 1

#define EGL_NONE 0x3038
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056

#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489

#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401

/* ------------------------------------------------------------- real ANGLE */
static void *G_gles, *G_egl;
static const char *G_dir;
static EGLDisplay G_dpy;

static EGLDisplay (*r_GetPlatformDisplayEXT)(EGLenum, void *, const EGLint *);
static EGLDisplay (*r_GetDisplay)(void *);
static EGLBoolean (*r_Initialize)(EGLDisplay, EGLint *, EGLint *);
static EGLBoolean (*r_Terminate)(EGLDisplay);
static const char *(*r_QueryString)(EGLDisplay, EGLint);
static EGLBoolean (*r_ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
static EGLBoolean (*r_GetConfigs)(EGLDisplay, EGLConfig *, EGLint, EGLint *);
static EGLBoolean (*r_GetConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);
static EGLSurface (*r_CreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
static EGLBoolean (*r_DestroySurface)(EGLDisplay, EGLSurface);
static EGLBoolean (*r_QuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint *);
static EGLContext (*r_CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
static EGLBoolean (*r_DestroyContext)(EGLDisplay, EGLContext);
static EGLBoolean (*r_MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLContext (*r_GetCurrentContext)(void);
static EGLSurface (*r_GetCurrentSurface)(EGLint);
static EGLDisplay (*r_GetCurrentDisplay)(void);
static EGLBoolean (*r_SwapBuffers)(EGLDisplay, EGLSurface);
static EGLint (*r_GetError)(void);
static void *(*r_GetProcAddress)(const char *);

static void *G_glReadPixels;
static void *G_glGetIntegerv, *G_glBindFramebuffer;

/* ------------------------------------------- draw accounting + wrappers */
static unsigned long G_n_draws, G_n_elems, G_n_clears;
static unsigned long G_n_vaptr, G_n_bufdata, G_n_bufsub, G_n_gul, G_n_um4;
static int G_last_draw_fbo = -1;
static int G_frame_fbo = -1;   /* last non-zero draw target this frame */
static void (*r_DrawArrays)(unsigned, int, int);
static void (*r_DrawElements)(unsigned, int, unsigned, const void *);
static void (*r_Clear)(unsigned);

static int cur_fbo(void)
{
    if (!G_glGetIntegerv)
        G_glGetIntegerv = r_GetProcAddress ? r_GetProcAddress("glGetIntegerv")
                                           : (G_gles ? dlsym(G_gles, "glGetIntegerv") : NULL);
    int fbo = -1;
    if (G_glGetIntegerv) ((void (*)(unsigned, int *))G_glGetIntegerv)(0x8CA6, &fbo);
    return fbo;
}


static unsigned (*r_GetErrorFn)(void);
static unsigned cur_gl_error(void)
{
    if (!r_GetErrorFn)
        r_GetErrorFn = r_GetProcAddress ? r_GetProcAddress("glGetError")
                                        : (G_gles ? dlsym(G_gles, "glGetError") : NULL);
    return r_GetErrorFn ? r_GetErrorFn() : 0;
}

static int cur_program(void)
{
    int p = -1;
    if (G_glGetIntegerv) ((void (*)(unsigned, int *))G_glGetIntegerv)(0x8B8D /*GL_CURRENT_PROGRAM*/, &p);
    return p;
}

/* ------------------------------------------------ diagnostic wrappers --- */
static void pb_gldebug_cb(unsigned src, unsigned type, unsigned id, unsigned sev,
                          long len, const char *msg, const void *ud)
{
    (void)src; (void)type; (void)id; (void)ud;
    static unsigned n;
    if (n++ > 200) return;
    fprintf(stderr, "[gldbg] sev=%#x: %.*s\n", sev, len > 0 && len < 400 ? (int)len : 0, msg ? msg : "");
    fflush(stderr);
}
static unsigned long G_n_tex_up, G_n_errs;
static const char *tidtag(char *b, size_t n)
{
    static __thread char tb[24];
    if (!tb[0]) snprintf(tb, sizeof tb, "%lx", (unsigned long)pthread_self() & 0xffffff);
    snprintf(b, n, "t=%s", tb);
    return b;
}

static void gl_ck(const char *what, long a, long b, long c)
{
    unsigned e = cur_gl_error();
    if (e && G_n_errs < 80) {
        char tb[32];
        G_n_errs++;
        fprintf(stderr, "[gl] ERR %s(%ld,%ld,%ld) -> %#x %s\n", what, a, b, c, e, tidtag(tb, sizeof tb));
        fflush(stderr);
    }
}

static int G_tex_bind[8];
static unsigned G_act_tex = 0x84C0;
static void (*r_TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static struct { unsigned id; int w, h; unsigned ifmt; } G_texinfo[64];
static void texinfo_set(unsigned id, int w, int h, unsigned ifmt)
{
    if (!id) return;
    for (int i = 0; i < 64; i++) {
        if (G_texinfo[i].id == id) { G_texinfo[i].w = w; G_texinfo[i].h = h; G_texinfo[i].ifmt = ifmt; return; }
    }
    for (int i = 0; i < 64; i++) {
        if (!G_texinfo[i].id) { G_texinfo[i].id = id; G_texinfo[i].w = w; G_texinfo[i].h = h; G_texinfo[i].ifmt = ifmt; return; }
    }
}
static int texinfo_get(unsigned id, int *w, int *h, unsigned *ifmt)
{
    for (int i = 0; i < 64; i++)
        if (G_texinfo[i].id == id) { *w = G_texinfo[i].w; *h = G_texinfo[i].h; if (ifmt) *ifmt = G_texinfo[i].ifmt; return 1; }
    return 0;
}
static void w_glTexImage2D(unsigned t, int lvl, int ifmt, int w, int h, int bd, unsigned fmt, unsigned ty, const void *px)
{
    if (lvl == 0) {
        int u = (int)(G_act_tex - 0x84C0);
        if (u >= 0 && u < 8) texinfo_set((unsigned)G_tex_bind[u], w, h, (unsigned)ifmt);
    }
    if (G_n_tex_up < 50) {
        G_n_tex_up++;
        char tb[32];
        fprintf(stderr, "[gl] TexImage2D tgt=%#x lvl=%d ifmt=%#x %dx%d fmt=%#x type=%#x %s\n", t, lvl, ifmt, w, h, fmt, ty, tidtag(tb, sizeof tb));
        fflush(stderr);
    }
    if (r_TexImage2D) r_TexImage2D(t, lvl, ifmt, w, h, bd, fmt, ty, px);
    gl_ck("TexImage2D", ifmt, w, h);
}

static void (*r_CompTexImage2D)(unsigned, int, unsigned, int, int, int, int, const void *);
static void w_glCompressedTexImage2D(unsigned t, int lvl, unsigned ifmt, int w, int h, int bd, int sz, const void *px)
{
    if (lvl == 0) {
        int u = (int)(G_act_tex - 0x84C0);
        if (u >= 0 && u < 8) texinfo_set((unsigned)G_tex_bind[u], w, h, ifmt);
    }
    if (G_n_tex_up < 50) {
        G_n_tex_up++;
        fprintf(stderr, "[gl] CompTexImage2D tgt=%#x lvl=%d ifmt=%#x %dx%d size=%d\n", t, lvl, ifmt, w, h, sz);
        fflush(stderr);
    }
    if (r_CompTexImage2D) r_CompTexImage2D(t, lvl, ifmt, w, h, bd, sz, px);
    gl_ck("CompTexImage2D", ifmt, w, h);
}

static void (*r_TexSubImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static void w_glTexSubImage2D(unsigned t, int lvl, int xo, int yo, int w, int h, unsigned fmt, unsigned ty, const void *px)
{
    if (r_TexSubImage2D) r_TexSubImage2D(t, lvl, xo, yo, w, h, fmt, ty, px);
    gl_ck("TexSubImage2D", fmt, w, h);
}

static int G_fbo_tex[32];
static unsigned G_n_bindtex, G_n_u1i;

static void (*r_ActiveTexture)(unsigned);
static void w_glActiveTexture(unsigned t)
{
    G_act_tex = t;
    if (r_ActiveTexture) r_ActiveTexture(t);
}

static void (*r_BindTexture)(unsigned, unsigned);
static void w_glBindTexture(unsigned tgt, unsigned tex)
{
    int u = (int)(G_act_tex - 0x84C0);
    if (u >= 0 && u < 8) G_tex_bind[u] = (int)tex;
    if (G_n_bindtex < 140) {
        G_n_bindtex++;
        fprintf(stderr, "[gl] BindTex u=%d tex=%u tgt=%#x\n", u, tex, tgt);
        fflush(stderr);
    }
    if (r_BindTexture) r_BindTexture(tgt, tex);
}

static void (*r_FramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
static void w_glFramebufferTexture2D(unsigned tgt, unsigned att, unsigned tt, unsigned tex, int lvl)
{
    int f = cur_fbo();
    if (att == 0x8CE0 && f >= 0 && f < 32) G_fbo_tex[f] = (int)tex;
    fprintf(stderr, "[gl] FBOtex fbo=%d att=%#x tex=%u lvl=%d\n", f, att, tex, lvl);
    fflush(stderr);
    if (r_FramebufferTexture2D) r_FramebufferTexture2D(tgt, att, tt, tex, lvl);
}

static void (*r_FramebufferRenderbuffer)(unsigned, unsigned, unsigned, unsigned);
static void w_glFramebufferRenderbuffer(unsigned tgt, unsigned att, unsigned rbt, unsigned rb)
{
    int f = cur_fbo();
    if (att == 0x8CE0 && f >= 0 && f < 32) G_fbo_tex[f] = -1;
    fprintf(stderr, "[gl] FBOrb fbo=%d att=%#x rb=%u\n", f, att, rb);
    fflush(stderr);
    if (r_FramebufferRenderbuffer) r_FramebufferRenderbuffer(tgt, att, rbt, rb);
}

static void (*r_U1i)(int, int);
static void w_glUniform1i(int loc, int v)
{
    if (G_n_u1i < 140) {
        G_n_u1i++;
        fprintf(stderr, "[gl] Uniform1i prog=%u loc=%d v=%d\n", cur_program(), loc, v);
        fflush(stderr);
    }
    if (r_U1i) r_U1i(loc, v);
}

static void (*r_TexParamI)(unsigned, unsigned, int);
static void w_glTexParameteri(unsigned t, unsigned p, int v)
{
    static unsigned n;
    if (n < 80) {
        n++;
        fprintf(stderr, "[gl] TexParam tex=%d p=%#x v=%#x\n", G_tex_bind[0], p, v);
        fflush(stderr);
    }
    if (r_TexParamI) r_TexParamI(t, p, v);
}

static void (*r_VertAttribPtr)(unsigned, int, unsigned, unsigned char, int, const void *);
static void w_glVertexAttribPointer(unsigned idx, int size, unsigned type, unsigned char norm, int stride, const void *ptr)
{
    static unsigned n24;
    if (stride == 24 && n24 < 60 && getenv("PB_VAPTR")) {
        n24++;
        fprintf(stderr, "[gl] VAP idx=%u size=%d type=%#x norm=%d stride=%d off=%ld\n",
                idx, size, type, norm, stride, (long)ptr);
        fflush(stderr);
    }
    if (G_n_vaptr < 40) {
        G_n_vaptr++;
        fprintf(stderr, "[gl] VertexAttribPointer idx=%u size=%d type=%#x norm=%d stride=%d ptr=%p\n", idx, size, type, norm, stride, ptr);
        fflush(stderr);
    }
    if (r_VertAttribPtr) r_VertAttribPtr(idx, size, type, norm, stride, ptr);
    gl_ck("VertexAttribPointer", idx, size, stride);
}

static void (*r_UseProgram)(unsigned);
static void w_glUseProgram(unsigned p)
{
    if (r_UseProgram) r_UseProgram(p);
    {
        static unsigned n_use;
        if (n_use < 60) {
            n_use++;
            char tb[32];
            fprintf(stderr, "[gl] UseProgram(%u) %s\n", p, tidtag(tb, sizeof tb));
            fflush(stderr);
        }
    }
    gl_ck("UseProgram", p, 0, 0);
}


/* ---------------------------------------------- vertex buffer shadow state --- */
typedef struct { unsigned id; unsigned n_up; long size; unsigned char head[2048]; } vbo_shadow;
static vbo_shadow G_vbo[512];
static unsigned G_bound_array, G_bound_elem;

static vbo_shadow *vbo_slot(unsigned id, int create)
{
    int i, free_i = -1;
    if (!id) return NULL;
    for (i = 0; i < 512; i++) {
        if (G_vbo[i].id == id) return &G_vbo[i];
        if (free_i < 0 && !G_vbo[i].id) free_i = i;
    }
    if (create && free_i >= 0) { G_vbo[free_i].id = id; return &G_vbo[free_i]; }
    return NULL;
}

static void (*r_BindBuffer)(unsigned, unsigned);
static void w_glBindBuffer(unsigned t, unsigned b)
{
    if (t == 0x8892) G_bound_array = b;
    else if (t == 0x8893) G_bound_elem = b;
    if (r_BindBuffer) r_BindBuffer(t, b);
    {
        static unsigned n_bind;
        if (b && n_bind < 60) {
            n_bind++;
            fprintf(stderr, "[gl] BindBuffer(tgt=%#x id=%u)\n", t, b);
            fflush(stderr);
        }
    }
    gl_ck("BindBuffer", t, b, 0);
}

static void (*r_BufferData)(unsigned, long, const void *, unsigned);
static void w_glBufferData(unsigned t, long sz, const void *d, unsigned u)
{
    if (G_n_bufdata < 60) {
        G_n_bufdata++;
        const float *fd = d;
        fprintf(stderr, "[gl] BufferData tgt=%#x id=%u size=%ld usage=%#x f0..7=%.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f\n",
                t, t == 0x8892 ? G_bound_array : G_bound_elem, sz, u,
                fd?fd[0]:0.f, fd?fd[1]:0.f, fd?fd[2]:0.f, fd?fd[3]:0.f,
                fd?fd[4]:0.f, fd?fd[5]:0.f, fd?fd[6]:0.f, fd?fd[7]:0.f);
        fflush(stderr);
    }
    if (sz == 27648 && d && getenv("PB_DUMP_COMP")) {
        static int done;
        if (!done) {
            done = 1;
            FILE *f = fopen("/tmp/comp_verts.bin", "wb");
            if (f) { fwrite(d, 1, (size_t)sz, f); fclose(f); }
            fprintf(stderr, "[gl] dumped comp verts\n");
        }
    }
    if (sz == 9600 || sz == 5120 || sz == 288 || sz == 27648) {        static unsigned n_x[4];
        int k = sz == 9600 ? 0 : sz == 5120 ? 1 : sz == 288 ? 2 : 3;
        if (n_x[k] < 2) {
            n_x[k]++;
            fprintf(stderr, "[gl] UPLOAD sz=%ld ra=%p data=%p t=0x%llx\n",
                    (long)sz, __builtin_return_address(0), d,
                    (unsigned long long)pb_cur_guest_thread());
            if (d && sz >= 64) {
                const unsigned char *p = d;
                for (int r = 0; r < 4; r++) {
                    fprintf(stderr, "   +%03x:", r * 32);
                    for (int c = 0; c < 32; c += 4) {
                        unsigned w;
                        memcpy(&w, p + r * 32 + c, 4);
                        fprintf(stderr, " %08x", w);
                    }
                    fprintf(stderr, "\n");
                }
                const float *ff = (const float *)d;
                fprintf(stderr, "   f:");
                for (int i = 0; i < 8; i++) fprintf(stderr, " %.4g", ff[i]);
                fprintf(stderr, "\n");
            }
            fflush(stderr);
        }
    }
    if (t == 0x8892 || t == 0x8893) {
        vbo_shadow *vs = vbo_slot(t == 0x8892 ? G_bound_array : G_bound_elem, 1);
        if (vs) {
            vs->size = sz;
            vs->n_up++;
            memset(vs->head, 0, sizeof vs->head);
            if (d && sz > 0) memcpy(vs->head, d, sz < (long)sizeof vs->head ? sz : (long)sizeof vs->head);
        }
    }
    if (r_BufferData) r_BufferData(t, sz, d, u);
    gl_ck("BufferData", t, sz, u);
}

static unsigned (*r_CreateProgram)(void);
static unsigned w_glCreateProgram(void)
{
    unsigned p = r_CreateProgram ? r_CreateProgram() : 0;
    char tb[32];
    fprintf(stderr, "[gl] CreateProgram -> %u %s\n", p, tidtag(tb, sizeof tb));
    fflush(stderr);
    return p;
}

static void (*r_BufferSubData)(unsigned, long, long, const void *);
static void w_glBufferSubData(unsigned t, long off, long sz, const void *d)
{
    if (G_n_bufsub < 20 && d && sz >= 16) {
        const float *f = d;
        G_n_bufsub++;
        char tb[32];
        fprintf(stderr, "[gl] BufferSubData off=%ld size=%ld f0..3=%.2f %.2f %.2f %.2f %s\n",
                off, sz, f[0], f[1], f[2], f[3], tidtag(tb, sizeof tb));
    }
    if (t == 0x8892 || t == 0x8893) {
        vbo_shadow *vs = vbo_slot(t == 0x8892 ? G_bound_array : G_bound_elem, 1);
        if (vs) vs->n_up++;
        if (vs && d && off >= 0 && off < (long)sizeof vs->head) {
            long n = sz;
            if (off + n > (long)sizeof vs->head) n = (long)sizeof vs->head - off;
            memcpy(vs->head + off, d, n);
        }
    }
    if (r_BufferSubData) r_BufferSubData(t, off, sz, d);
    gl_ck("BufferSubData", off, sz, 0);
}

static int (*r_GUL)(unsigned, const char *);
static int w_glGetUniformLocation(unsigned pr, const char *nm)
{
    int loc = r_GUL ? r_GUL(pr, nm) : -1;
    if (G_n_gul < 80) {
        G_n_gul++;
        fprintf(stderr, "[gl] GetUniformLocation(prog=%u, \"%s\") -> %d\n", pr, nm ? nm : "(null)", loc);
        fflush(stderr);
    }
    return loc;
}

static void (*r_UM4)(int, int, unsigned char, const float *);
static float G_mtx63[32][16];
static unsigned char G_mtx63_set[32];
static void w_glUniformMatrix4fv(int loc, int cnt, unsigned char tr, const float *v)
{
    if (cur_program() == 63 && v && cnt == 1 && loc >= 0 && loc < 32) {
        memcpy(G_mtx63[loc], v, 16 * sizeof(float));
        G_mtx63_set[loc] = 1;
    }
    if (G_n_um4 < 220) {
        G_n_um4++;
        fprintf(stderr, "[gl] UniformMatrix4fv prog=%u loc=%d cnt=%d tr=%d m0=%.3g %.3g %.3g %.3g m1=%.3g %.3g %.3g %.3g m3=%.3g %.3g %.3g %.3g ptr=%p\n",
                cur_program(), loc, cnt, tr,
                v?v[0]:0.f, v?v[1]:0.f, v?v[2]:0.f, v?v[3]:0.f,
                v?v[4]:0.f, v?v[5]:0.f, v?v[6]:0.f, v?v[7]:0.f,
                v?v[12]:0.f, v?v[13]:0.f, v?v[14]:0.f, v?v[15]:0.f, (const void *)v);
        fflush(stderr);
    } else if (v && (!isfinite(v[0]) || !isfinite(v[5]))) {
        static unsigned n_bad;
        if (n_bad < 40) {
            n_bad++;
            fprintf(stderr, "[gl] UM4-BAD prog=%u loc=%d tr=%d m0=%.3g %.3g %.3g %.3g m1=%.3g %.3g %.3g %.3g ptr=%p\n",
                    cur_program(), loc, tr,
                    v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], (const void *)v);
            fflush(stderr);
        }
    }
    if (r_UM4) r_UM4(loc, cnt, tr, v);
    gl_ck("UniformMatrix4fv", loc, cnt, 0);
}

static void (*r_BindAttrib)(unsigned, unsigned, const char *);
static void w_glBindAttribLocation(unsigned p, unsigned idx, const char *nm)
{
    static unsigned n;
    if (n < 80) {
        n++;
        fprintf(stderr, "[gl] BindAttribLocation prog=%u idx=%u name=%s\n", p, idx, nm ? nm : "(null)");
        fflush(stderr);
    }
    if (r_BindAttrib) r_BindAttrib(p, idx, nm);
}

static int (*r_GetAttrib)(unsigned, const char *);
static int w_glGetAttribLocation(unsigned p, const char *nm)
{
    int loc = r_GetAttrib ? r_GetAttrib(p, nm) : -1;
    static unsigned n;
    if (n < 80) {
        n++;
        fprintf(stderr, "[gl] GetAttribLocation prog=%u name=%s -> %d\n", p, nm ? nm : "(null)", loc);
        fflush(stderr);
    }
    return loc;
}

static void (*r_ShaderSource)(unsigned, int, const char *const *, const int *);
static void w_glShaderSource(unsigned sh, int cnt, const char *const *str, const int *len)
{
    static unsigned n_ss;
    /* PB_UI_RED / PB_COMP_BLUE: replace fragment output with a flat colour to
       separate "geometry lands" from "texture/vertex colour is black". */
    const char *ui = getenv("PB_UI_RED");
    const char *cb = getenv("PB_COMP_BLUE");
    const char *cs = getenv("PB_COMP_SRC");
    const char *cc = getenv("PB_COMP_COL");
    const char *cd = getenv("PB_COMP_DIRT");
    const char *cu = getenv("PB_COMP_UV");
    int is_comp = str && cnt == 1 && str[0] && strstr(str[0], "PS_SourceColour.rgb * (sourceCol");
    int is_ui = str && cnt == 1 && str[0] && strstr(str[0], "ColourOut * texture2D");
    if (is_ui && ui) {
        const char *one = "varying lowp vec4 ColourOut;\nvarying lowp vec2 TexCoordOut;\n"
                          "uniform sampler2D Texture;\nvoid main(void){ gl_FragColor = vec4(1.0,0.0,0.0,1.0); }\n";
        int l = (int)strlen(one);
        fprintf(stderr, "[gl] SHADER-PATCH ui-red sh=%u\n", sh);
        if (r_ShaderSource) r_ShaderSource(sh, 1, &one, &l);
        return;
    }
    if (is_comp && (cb || cs || cc || cd || cu)) {
        const char *one;
        int l;
        if (cb) {
            one = "uniform sampler2D SourceTex;\nuniform sampler2D DirtTex;\nuniform sampler2D CRTTex;\n"
                  "varying lowp vec2 PS_SourceTexCoord;\nvarying lowp vec2 PS_DirtTexCoord;\n"
                  "varying mediump vec2 PS_CRTTexCoord;\nvarying lowp vec4 PS_SourceColour;\n"
                  "varying lowp vec3 PS_DirtColour;\nvoid main(void){ gl_FragColor = vec4(0.2,0.4,1.0,1.0); }\n";
            fprintf(stderr, "[gl] SHADER-PATCH comp-blue sh=%u\n", sh);
        } else if (cs) {
            one = "uniform sampler2D SourceTex;\nuniform sampler2D DirtTex;\nuniform sampler2D CRTTex;\n"
                  "varying lowp vec2 PS_SourceTexCoord;\nvarying lowp vec2 PS_DirtTexCoord;\n"
                  "varying mediump vec2 PS_CRTTexCoord;\nvarying lowp vec4 PS_SourceColour;\n"
                  "varying lowp vec3 PS_DirtColour;\n"
                  "void main(void){ gl_FragColor = vec4(texture2D(SourceTex, PS_SourceTexCoord).rgb, 1.0); }\n";
            fprintf(stderr, "[gl] SHADER-PATCH comp-src sh=%u\n", sh);
        } else if (cc) {
            one = "uniform sampler2D SourceTex;\nuniform sampler2D DirtTex;\nuniform sampler2D CRTTex;\n"
                  "varying lowp vec2 PS_SourceTexCoord;\nvarying lowp vec2 PS_DirtTexCoord;\n"
                  "varying mediump vec2 PS_CRTTexCoord;\nvarying lowp vec4 PS_SourceColour;\n"
                  "varying lowp vec3 PS_DirtColour;\n"
                  "void main(void){ gl_FragColor = vec4(PS_SourceColour.rgb, 1.0); }\n";
            fprintf(stderr, "[gl] SHADER-PATCH comp-col sh=%u\n", sh);
        } else if (cd) {
            one = "uniform sampler2D SourceTex;\nuniform sampler2D DirtTex;\nuniform sampler2D CRTTex;\n"
                  "varying lowp vec2 PS_SourceTexCoord;\nvarying lowp vec2 PS_DirtTexCoord;\n"
                  "varying mediump vec2 PS_CRTTexCoord;\nvarying lowp vec4 PS_SourceColour;\n"
                  "varying lowp vec3 PS_DirtColour;\n"
                  "void main(void){ gl_FragColor = vec4(texture2D(DirtTex, PS_DirtTexCoord).rgb, 1.0); }\n";
            fprintf(stderr, "[gl] SHADER-PATCH comp-dirt sh=%u\n", sh);
        } else if (getenv("PB_COMP_SRC0")) {
            one = "uniform sampler2D SourceTex;\nuniform sampler2D DirtTex;\nuniform sampler2D CRTTex;\n"
                  "varying lowp vec2 PS_SourceTexCoord;\nvarying lowp vec2 PS_DirtTexCoord;\n"
                  "varying mediump vec2 PS_CRTTexCoord;\nvarying lowp vec4 PS_SourceColour;\n"
                  "varying lowp vec3 PS_DirtColour;\n"
                  "void main(void){ gl_FragColor = vec4(texture2D(SourceTex, vec2(0.1,0.1)).rgb, 1.0); }\n";
            fprintf(stderr, "[gl] SHADER-PATCH comp-src0 sh=%u\n", sh);
        } else {
            one = "uniform sampler2D SourceTex;\nuniform sampler2D DirtTex;\nuniform sampler2D CRTTex;\n"
                  "varying lowp vec2 PS_SourceTexCoord;\nvarying lowp vec2 PS_DirtTexCoord;\n"
                  "varying mediump vec2 PS_CRTTexCoord;\nvarying lowp vec4 PS_SourceColour;\n"
                  "varying lowp vec3 PS_DirtColour;\n"
                  "void main(void){ gl_FragColor = vec4(PS_SourceTexCoord, 0.0, 1.0); }\n";
            fprintf(stderr, "[gl] SHADER-PATCH comp-uv sh=%u\n", sh);
        }
        l = (int)strlen(one);
        if (r_ShaderSource) r_ShaderSource(sh, 1, &one, &l);
        return;
    }
    if (n_ss < 14 && str && cnt > 0) {
        n_ss++;
        fprintf(stderr, "[gl] ShaderSource sh=%u cnt=%d:\n", sh, cnt);
        for (int i = 0; i < cnt && i < 4; i++) {
            int n = len && len[i] > 0 ? len[i] : (str[i] ? (int)strlen(str[i]) : 0);
            if (n > 3000) n = 3000;
            if (str[i] && n > 0) fprintf(stderr, "%.*s\n[/src]\n", n, str[i]);
        }
        fflush(stderr);
    }
    if (r_ShaderSource) r_ShaderSource(sh, cnt, str, len);
}

static void (*r_CompileShader)(unsigned);
static void (*r_GetShaderiv)(unsigned, unsigned, int *);
static void (*r_GetShaderInfoLog)(unsigned, int, int *, char *);
static void w_glCompileShader(unsigned sh)
{
    if (r_CompileShader) r_CompileShader(sh);
    int ok = -1;
    if (r_GetShaderiv) r_GetShaderiv(sh, 0x8B81, &ok);
    if (ok == 0) {
        char lg[2048] = {0};
        if (r_GetShaderInfoLog) r_GetShaderInfoLog(sh, sizeof lg - 1, NULL, lg);
        fprintf(stderr, "[gl] SHADER %u COMPILE FAILED: %.1500s\n", sh, lg);
        fflush(stderr);
    }
    gl_ck("CompileShader", sh, ok, 0);
}

static void (*r_LinkProgram)(unsigned);
static void (*r_GetProgramiv)(unsigned, unsigned, int *);
static void (*r_GetActiveUniform)(unsigned, int, int, int *, int *, unsigned *, char *);
static void (*r_GetUniformfv)(unsigned, int, float *);
static void (*r_GetProgramInfoLog)(unsigned, int, int *, char *);
static void w_glLinkProgram(unsigned pr)
{
    if (r_LinkProgram) r_LinkProgram(pr);
    int ok = -1;
    if (r_GetProgramiv) r_GetProgramiv(pr, 0x8B82, &ok);
    {
        char tb[32];
        fprintf(stderr, "[gl] LinkProgram %u ok=%d %s\n", pr, ok, tidtag(tb, sizeof tb));
        fflush(stderr);
    }
    if (ok == 0) {
        char lg[2048] = {0};
        if (r_GetProgramInfoLog) r_GetProgramInfoLog(pr, sizeof lg - 1, NULL, lg);
        fprintf(stderr, "[gl] PROGRAM %u LINK FAILED: %.1500s\n", pr, lg);
        fflush(stderr);
    }
    gl_ck("LinkProgram", pr, ok, 0);
}


static void (*r_DepthMaskFn)(unsigned char);
static void (*r_DisableFn)(unsigned);

static int force_nodepth(void)
{
    static int v = -1;
    if (v < 0) v = getenv("PB_FORCE_NODEPTH") ? 1 : 0;
    return v;
}

static void force_nodepth_apply(void)
{
    if (!force_nodepth()) return;
    if (!r_DisableFn)   r_DisableFn   = r_GetProcAddress ? r_GetProcAddress("glDisable")   : dlsym(G_gles, "glDisable");
    if (!r_DepthMaskFn) r_DepthMaskFn = r_GetProcAddress ? r_GetProcAddress("glDepthMask") : dlsym(G_gles, "glDepthMask");
    if (r_DisableFn)   r_DisableFn(0x0B71);
    if (r_DepthMaskFn) r_DepthMaskFn(1);
}

static void (*r_DepthMaskW)(unsigned char);
static void w_glDepthMask(unsigned char m)
{
    {
        static unsigned n_dm;
        if (n_dm < 40) {
            n_dm++;
            fprintf(stderr, "[gl] DepthMask(%d)\n", m);
            fflush(stderr);
        }
    }
    if (r_DepthMaskW) r_DepthMaskW(m);
    gl_ck("DepthMask", m, 0, 0);
}

static int (*r_GetVertAttribiv)(unsigned, unsigned, int *);
static unsigned char (*r_IsEnabled)(unsigned);

static void dump_uniforms(unsigned prog)
{
    if (!r_GetProgramiv || !r_GetActiveUniform || !r_GetUniformfv || !r_GUL) return;
    int n = 0;
    r_GetProgramiv(prog, 0x8B86 /*ACTIVE_UNIFORMS*/, &n);
    fprintf(stderr, "[gl]   uniforms prog=%u n=%d\n", prog, n);
    for (int i = 0; i < n && i < 48; i++) {
        char nm[160]; int len = 0, size = 0; unsigned type = 0;
        float f[16] = {0};
        r_GetActiveUniform(prog, i, (int)sizeof nm, &len, &size, &type, nm);
        int loc = r_GUL(prog, nm);
        if (loc >= 0) r_GetUniformfv(prog, loc, f);
        if (type == 0x8B5C)
            fprintf(stderr, "[gl]     %s mat4 [%.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g]\n",
                    nm, f[0],f[1],f[2],f[3], f[4],f[5],f[6],f[7], f[8],f[9],f[10],f[11], f[12],f[13],f[14],f[15]);
        else if (type == 0x1406) fprintf(stderr, "[gl]     %s float %.5g\n", nm, f[0]);
        else if (type == 0x8B50) fprintf(stderr, "[gl]     %s vec2 %.5g %.5g\n", nm, f[0], f[1]);
        else if (type == 0x8B51) fprintf(stderr, "[gl]     %s vec3 %.5g %.5g %.5g\n", nm, f[0], f[1], f[2]);
        else if (type == 0x8B52) fprintf(stderr, "[gl]     %s vec4 %.5g %.5g %.5g %.5g\n", nm, f[0], f[1], f[2], f[3]);
        else fprintf(stderr, "[gl]     %s type=%#x loc=%d\n", nm, type, loc);
    }
    fflush(stderr);
}

static void dump_draw_state(unsigned long n)
{
    unsigned abuf = 0, ebuf = 0;
    int prog = -1;
    if (G_glGetIntegerv) {
        ((void (*)(unsigned, int *))G_glGetIntegerv)(0x8894 /*ARRAY_BUFFER_BINDING*/, (int *)&abuf);
        ((void (*)(unsigned, int *))G_glGetIntegerv)(0x8895 /*ELEMENT_ARRAY_BUFFER_BINDING*/, (int *)&ebuf);
        ((void (*)(unsigned, int *))G_glGetIntegerv)(0x8B8D /*CURRENT_PROGRAM*/, &prog);
    }
    fprintf(stderr, "[gl] DRAWSTATE n=%lu prog=%d fbo=%d tex=%d,%d,%d,%d", n, prog, cur_fbo(),
            G_tex_bind[0], G_tex_bind[1], G_tex_bind[2], G_tex_bind[3]);
    {
        vbo_shadow *vs = vbo_slot(abuf, 0);
        if (vs) {
            const float *f = (const float *)vs->head;
            fprintf(stderr, " vbo=%u sz=%ld up=%u f0..5=%.2f %.2f %.2f %.2f %.2f %.2f",
                    abuf, vs->size, vs->n_up, f[0], f[1], f[2], f[3], f[4], f[5]);
        } else {
            fprintf(stderr, " vbo=%u (nosha)", abuf);
        }
        vbo_shadow *es = vbo_slot(ebuf, 0);
        if (es) {
            const unsigned short *ui = (const unsigned short *)es->head;
            fprintf(stderr, " elembuf=%u sz=%ld i0..7=%u %u %u %u %u %u %u %u",
                    ebuf, es->size, ui[0], ui[1], ui[2], ui[3], ui[4], ui[5], ui[6], ui[7]);
        } else if (ebuf) {
            fprintf(stderr, " elembuf=%u (nosha)", ebuf);
        }
    }
    fprintf(stderr, "\n");
    if (r_GetVertAttribiv) {
        int i;
        for (i = 0; i < 8; i++) {
            int en = 0, sz = -1, ty = 0, st = 0, bb = 0, ptr = 0;
            r_GetVertAttribiv(i, 0x8622, &en);
            r_GetVertAttribiv(i, 0x8623, &sz);
            r_GetVertAttribiv(i, 0x8625, &ty);
            r_GetVertAttribiv(i, 0x8624, &st);
            r_GetVertAttribiv(i, 0x889F, &bb);
            r_GetVertAttribiv(i, 0x8645, &ptr);
            if (en)
                fprintf(stderr, "[gl]   attrib%d size=%d type=%#x stride=%d buf=%d off=%d\n", i, sz, ty, st, bb, ptr);        }
    }
    {
        int dfn = 0, dmask = -1, cm[4] = {-1, -1, -1, -1};
        if (G_glGetIntegerv) {
            ((void (*)(unsigned, int *))G_glGetIntegerv)(0x0B74 /*DEPTH_FUNC*/, &dfn);
            ((void (*)(unsigned, int *))G_glGetIntegerv)(0x0B72 /*DEPTH_WRITEMASK*/, &dmask);
            ((void (*)(unsigned, int *))G_glGetIntegerv)(0x0C23 /*COLOR_WRITEMASK*/, cm);
        }
        {
            int asz = 0, aty = 0, astr = 0;
            if (r_GetVertAttribiv) {
                r_GetVertAttribiv(0, 0x8623, &asz);
                r_GetVertAttribiv(0, 0x8625, &aty);
                r_GetVertAttribiv(0, 0x8624, &astr);
            }
            vbo_shadow *vs2 = vbo_slot(abuf, 0);
            if (vs2 && aty == 0x1406 && asz >= 2 && astr > 0) {
                static const int idxs[6] = {0, 1, 2, 10, 40, 80};
                const float *fb = (const float *)vs2->head;
                fprintf(stderr, "[gl]   pos:");
                for (int k = 0; k < 6; k++) {
                    long off = (long)idxs[k] * astr;
                    if (off + 8 <= (long)sizeof vs2->head)
                        fprintf(stderr, " v%d=(%.1f,%.1f)", idxs[k], fb[off / 4], fb[off / 4 + 1]);
                }
                fprintf(stderr, "\n");
            }
        }
        int bsrc = 0, bdst = 0, beq = 0, vp[4] = {0,0,0,0}, sb[4] = {0,0,0,0};
        if (G_glGetIntegerv) {
            ((void (*)(unsigned, int *))G_glGetIntegerv)(0x80C9 /*BLEND_SRC_RGB*/, &bsrc);
            ((void (*)(unsigned, int *))G_glGetIntegerv)(0x80C8 /*BLEND_DST_RGB*/, &bdst);
            ((void (*)(unsigned, int *))G_glGetIntegerv)(0x8009 /*BLEND_EQUATION_RGB*/, &beq);
            ((void (*)(unsigned, int *))G_glGetIntegerv)(0x0BA2 /*VIEWPORT*/, vp);
            ((void (*)(unsigned, int *))G_glGetIntegerv)(0x0C10 /*SCISSOR_BOX*/, sb);
        }
        fprintf(stderr, "[gl]   state blend=%d depth=%d cull=%d scissor=%d stencil=%d dither=%d dfn=%#x dmask=%d cm=%d,%d,%d,%d\n",
                r_IsEnabled ? r_IsEnabled(0x0BE2) : -1,
                r_IsEnabled ? r_IsEnabled(0x0B71) : -1,
                r_IsEnabled ? r_IsEnabled(0x0B44) : -1,
                r_IsEnabled ? r_IsEnabled(0x0C11) : -1,
                r_IsEnabled ? r_IsEnabled(0x0B90) : -1,
                r_IsEnabled ? r_IsEnabled(0x0BD0) : -1,
                dfn, dmask, cm[0], cm[1], cm[2], cm[3]);
        fprintf(stderr, "[gl]   extra bsrc=%#x bdst=%#x beq=%#x vp=%d,%d %dx%d sb=%d,%d %dx%d\n",
                bsrc, bdst, beq, vp[0], vp[1], vp[2], vp[3], sb[0], sb[1], sb[2], sb[3]);
    }
    fflush(stderr);
}

static unsigned G_n_probe;

/* PB_TEXDUMP: attach a guest texture to a scratch FBO and histogram it. */
static void (*r_GenFB)(int, unsigned *);
static void (*r_DelFB)(int, const unsigned *);
static unsigned (*r_CheckFB)(unsigned);
static void tex_probe(const char *tag, unsigned tex, int w, int h)
{
    if (!getenv("PB_TEXDUMP") || !tex || !r_GenFB || !G_glBindFramebuffer ||
        !r_FramebufferTexture2D || !G_glReadPixels) return;
    static unsigned n;
    if (n >= 6) return;
    n++;
    unsigned fb = 0, prev = (unsigned)cur_fbo();
    r_GenFB(1, &fb);
    ((void (*)(unsigned, unsigned))G_glBindFramebuffer)(0x8D40, fb);
    r_FramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, tex, 0);
    unsigned st = r_CheckFB ? r_CheckFB(0x8D40) : 0;
    unsigned char *px = malloc((size_t)w * h * 4);
    unsigned nz = 0, mn[4] = {255,255,255,255}, mx[4] = {0,0,0,0};
    unsigned long long sum[4] = {0,0,0,0};
    if (px) {
        memset(px, 0, (size_t)w * h * 4);
        ((void (*)(int,int,int,int,unsigned,unsigned,void *))G_glReadPixels)(0, 0, w, h, 0x1908, 0x1401, px);
        for (long i = 0; i < (long)w * h; i++) {
            int any = 0;
            for (int c = 0; c < 4; c++) {
                unsigned v = px[i * 4 + c];
                if (v < mn[c]) mn[c] = v;
                if (v > mx[c]) mx[c] = v;
                sum[c] += v;
                if (v) any = 1;
            }
            if (any) nz++;
        }
        fprintf(stderr, "[tex] %s tex=%u fbo=%u st=%#x %dx%d nz=%u/%ld ch0=%u..%u avg=%llu ch1=%u..%u ch2=%u..%u ch3=%u..%u\n",
                tag, tex, fb, st, w, h, nz, (long)w * h,
                mn[0], mx[0], sum[0] / (unsigned long long)((long)w * h),
                mn[1], mx[1], mn[2], mx[2], mn[3], mx[3]);
        fflush(stderr);
        /* PB_TEXDUMP_RAW: keep the pixels so the scene can be inspected. */
        if (getenv("PB_TEXDUMP_RAW")) {
            char path[128];
            snprintf(path, sizeof path, "/tmp/texdump_%s_%u.raw", tag, n);
            FILE *f = fopen(path, "wb");
            if (f) {
                fprintf(f, "%d %d %s\n", w, h, tag);
                for (int y = h - 1; y >= 0; y--)          /* top-down */
                    fwrite(px + (size_t)y * w * 4, 1, (size_t)w * 4, f);
                fclose(f);
                fprintf(stderr, "[tex]   wrote %s\n", path);
            }
        }
        free(px);
    }
    ((void (*)(unsigned, unsigned))G_glBindFramebuffer)(0x8D40, prev);
    if (r_DelFB) r_DelFB(1, &fb);
}

static void crt_dbg(void)
{
    const char *e = getenv("PB_CRTDBG");
    if (!e || !PB.base) return;
    static int done;
    static double last_ms;
    if (!strcmp(e, "1")) {
        if (done) return;
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
        if (now - last_ms < 1000.0) return;
        last_ms = now;
    }
    done = 1;
    uint8_t *b = PB.base;
    uint64_t p1 = 0, p2 = 0, p3 = 0, p4 = 0;
    memcpy(&p1, b + 0x38ded0, 8);
    memcpy(&p2, b + 0x38def8, 8);
    memcpy(&p4, b + 0x38e040, 8);
    (void)p3;
    uint32_t v1 = 0, v2 = 0, v4 = 0;
    if (p1 > 0x10000 && p1 < 0x8000000000ull) memcpy(&v1, (void *)(uintptr_t)p1, 4);
    if (p2 > 0x10000 && p2 < 0x8000000000ull) memcpy(&v2, (void *)(uintptr_t)p2, 4);
    if (p4 > 0x10000 && p4 < 0x8000000000ull) memcpy(&v4, (void *)(uintptr_t)p4, 4);
    uint32_t cb4 = 0, cb8 = 0, cbc = 0;
    memcpy(&cb4, b + 0x13f6cb4, 4);
    memcpy(&cb8, b + 0x13f6cb8, 4);
    memcpy(&cbc, b + 0x13f6cbc, 4);
    fprintf(stderr, "[crt] p1=%#llx v1=%u p2=%#llx v2=%u p4=%#llx v4=%u cb4=%u cb8=%u cbc=%u tid=%s\n",
            (unsigned long long)p1, v1, (unsigned long long)p2, v2,
            (unsigned long long)p4, v4, cb4, cb8, cbc, tidtag((char[32]){0}, 32));
    fflush(stderr);
}
static void probe_fb0(const char *tag, int count)
{
    if (!getenv("PB_PIX_AFTER") || G_n_probe >= 80) return;
    if (cur_fbo() != 0 || !G_glReadPixels) return;
    if (count != 1152 && count != 600 && count != 132) return;
    if (count == 1152 && G_n_probe < 40 && getenv("PB_TEXQ")) {        /* is the sampled render texture complete / what does FBO1 hold? */
        static void *gtp, *gtl;
        if (!gtp) gtp = r_GetProcAddress ? r_GetProcAddress("glGetTexParameteriv") : NULL;
        if (!gtl) gtl = r_GetProcAddress ? r_GetProcAddress("glGetTexLevelParameteriv") : NULL;
        int b = 0, mn = 0, mg = 0, ws = 0, wt = 0, tw = 0, th = 0;
        if (G_glGetIntegerv) ((void (*)(unsigned, int *))G_glGetIntegerv)(0x8069, &b);
        if (gtp) {
            ((void (*)(unsigned, unsigned, int *))gtp)(0x0DE1, 0x2801, &mn);
            ((void (*)(unsigned, unsigned, int *))gtp)(0x0DE1, 0x2800, &mg);
            ((void (*)(unsigned, unsigned, int *))gtp)(0x0DE1, 0x2802, &ws);
            ((void (*)(unsigned, unsigned, int *))gtp)(0x0DE1, 0x2803, &wt);
        }
        if (gtl) {
            ((void (*)(unsigned, int, unsigned, int *))gtl)(0x0DE1, 0, 0x1000, &tw);
            ((void (*)(unsigned, int, unsigned, int *))gtl)(0x0DE1, 0, 0x1001, &th);
        }
        unsigned char q[4] = {0xAA,0xAA,0xAA,0xAA};
        if (G_glBindFramebuffer) {
            ((void (*)(unsigned, unsigned))G_glBindFramebuffer)(0x8D40, 1);
            ((void (*)(int,int,int,int,unsigned,unsigned,void *))G_glReadPixels)(960, 540, 1, 1, 0x1908, 0x1401, q);
            ((void (*)(unsigned, unsigned))G_glBindFramebuffer)(0x8D40, 0);
        }
        fprintf(stderr, "[texq] bind=%d min=%#x mag=%#x wrap=%#x/%#x size=%dx%d fbo1px=%02x%02x%02x%02x\n",
                b, mn, mg, ws, wt, tw, th, q[0], q[1], q[2], q[3]);
        fflush(stderr);
    }
    static const int pts[5][2] = {{960,540},{100,100},{1820,980},{480,540},{1440,540}};
    unsigned char px[20];
    for (int i = 0; i < 5; i++)
        ((void (*)(int,int,int,int,unsigned,unsigned,void *))G_glReadPixels)
            (pts[i][0], pts[i][1], 1, 1, 0x1908, 0x1401, px + i * 4);
    G_n_probe++;
    fprintf(stderr, "[fb0] %s n=%u prog=%d cnt=%d px=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
            tag, G_n_probe, cur_program(), count,
            px[0],px[1],px[2],px[3], px[4],px[5],px[6],px[7], px[8],px[9],px[10],px[11],
            px[12],px[13],px[14],px[15], px[16],px[17],px[18],px[19]);
    fflush(stderr);
}

static void scan_fb0(const char *tag, int count)
{
    if (!getenv("PB_SCAN") || !G_glReadPixels || !G_glGetIntegerv) return;
    static unsigned n;
    if (n >= 12) return;
    if (count >= 1000) return;
    if (G_n_draws + G_n_elems < 450) return;
    if (cur_fbo() != 0) return;
    n++;
    int vp[4] = {0, 0, 0, 0};
    ((void (*)(unsigned, int *))G_glGetIntegerv)(0x0BA2, vp);
    int w = vp[2], h = vp[3];
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
    unsigned char *p = malloc((size_t)w * h * 4);
    if (!p) return;
    ((void (*)(int,int,int,int,unsigned,unsigned,void *))G_glReadPixels)(0, 0, w, h, 0x1908, 0x1401, p);
    long red = 0, nonblue = 0, nonzero = 0;
    int minx = w, miny = h, maxx = -1, maxy = -1;
    for (long i = 0; i < (long)w * h; i++) {
        unsigned char r = p[i * 4], g = p[i * 4 + 1], b = p[i * 4 + 2];
        if (r || g || b) nonzero++;
        if (r > 200 && g < 70 && b < 70) {
            red++;
            int x = (int)(i % w), y = (int)(i / w);
            if (x < minx) minx = x; if (x > maxx) maxx = x;
            if (y < miny) miny = y; if (y > maxy) maxy = y;
        } else if (!(r == 51 && g == 102 && b == 255)) nonzero += 0;
    }
    fprintf(stderr, "[scan] %s n=%u cnt=%d vp=%dx%d red=%ld nonzero=%ld bbox=%d,%d..%d,%d\n",
            tag, n, count, w, h, red, nonzero, minx, miny, maxx, maxy);
    fflush(stderr);
    free(p);
}

static struct { int prog, count, fbo, elem; } G_ring[16];
static unsigned G_ring_n;
static struct { unsigned n; unsigned long v; } G_fsum[64];
static void ring_push(int prog, int count, int fbo, int elem)
{
    G_ring[G_ring_n & 15].prog = prog;
    G_ring[G_ring_n & 15].count = count;
    G_ring[G_ring_n & 15].fbo = fbo;
    G_ring[G_ring_n & 15].elem = elem;
    G_ring_n++;
    /* PB_FRAMESUM: per-program draw/vertex totals since the last swap */
    if (getenv("PB_FRAMESUM") && prog >= 0 && prog < 64) {
        G_fsum[prog].n++;
        G_fsum[prog].v += count;
    }
}
static void framesum_dump(void)
{
    if (!getenv("PB_FRAMESUM")) return;
    fprintf(stderr, "[fsum]");
    for (int i = 0; i < 64; i++)
        if (G_fsum[i].n)
            fprintf(stderr, " p%d:%ux/%luv", i, G_fsum[i].n, G_fsum[i].v);
    fprintf(stderr, "\n");
    memset(G_fsum, 0, sizeof G_fsum);
    fflush(stderr);
}
static void ring_dump(const char *where)
{
    if (!getenv("PB_RING")) return;
    fprintf(stderr, "[ring] %s n=%u:", where, G_ring_n);
    unsigned start = G_ring_n > 16 ? G_ring_n - 16 : 0;
    for (unsigned i = start; i < G_ring_n; i++)
        fprintf(stderr, " [%d p%d c%d f%d]", (int)(i - start), G_ring[i & 15].prog,
                G_ring[i & 15].count, G_ring[i & 15].fbo);
    fprintf(stderr, "\n");
    fflush(stderr);
}

static void probe_fbo1_scene(const char *tag, int count)
{
    if (!getenv("PB_SCAN1") || !G_glBindFramebuffer || !G_glReadPixels) return;
    static unsigned n;
    if (n >= 8 || count < 100) return;
    if (cur_fbo() != 1) return;
    n++;
    int prev = cur_fbo();
    ((void (*)(unsigned, unsigned))G_glBindFramebuffer)(0x8D40, 1);
    int vp[4] = {0,0,0,0};
    ((void (*)(unsigned, int *))G_glGetIntegerv)(0x0BA2, vp);
    int vw = vp[2] > 0 ? vp[2] : 192, vh = vp[3] > 0 ? vp[3] : 192;
    if (vw > 4096) vw = 4096;
    if (vh > 4096) vh = 4096;
    unsigned char *px = malloc((size_t)vw * vh * 4);
    if (!px) return;
    ((void (*)(int,int,int,int,unsigned,unsigned,void *))G_glReadPixels)(0, 0, vw, vh, 0x1908, 0x1401, px);
    enum { W = 0, H = 0 };
    (void)W; (void)H;
    {
        static int once;
        if (!once) {
            once = 1;
            FILE *f = fopen("/tmp/fbo1_view.raw", "wb");
            if (f) { fprintf(f, "%d %d\n", vw, vh); fwrite(px, 1, (size_t)vw * vh * 4, f); fclose(f); }
        }
    }
    int mn[3] = {255,255,255}, mx[3] = {0,0,0};
    long sum[3] = {0,0,0};
    for (long i = 0; i < (long)vw * vh; i++)
        for (int c = 0; c < 3; c++) {
            int v = px[i * 4 + c];
            if (v < mn[c]) mn[c] = v;
            if (v > mx[c]) mx[c] = v;
            sum[c] += v;
        }
    fprintf(stderr, "[fbo1] %s cnt=%d vp=%d,%d %dx%d min=%d,%d,%d max=%d,%d,%d avg=%ld,%ld,%ld\n",
            tag, count, vp[0], vp[1], vp[2], vp[3], mn[0], mn[1], mn[2], mx[0], mx[1], mx[2],
            sum[0] / ((long)vw*vh), sum[1] / ((long)vw*vh), sum[2] / ((long)vw*vh));
    fflush(stderr);
    ((void (*)(unsigned, unsigned))G_glBindFramebuffer)(0x8D40, (unsigned)prev);
}

static void w_glDrawArrays(unsigned mode, int first, int count)
{
    G_n_draws++;
    G_last_draw_fbo = cur_fbo();
    if (G_last_draw_fbo > 0) G_frame_fbo = G_last_draw_fbo;
    if (G_n_draws + G_n_elems <= 160) {
        unsigned pre = cur_gl_error();
        if (pre)
            fprintf(stderr, "[gl] PRE-ERR before DrawArrays n=%lu = %#x\n", G_n_draws + G_n_elems, pre);
        dump_draw_state(G_n_draws + G_n_elems);
    } else if (count == 600 && getenv("PB_LATE")) {
        static unsigned ln;
        if (ln++ < 2) dump_draw_state(G_n_draws + G_n_elems);
    }
    force_nodepth_apply();
    /* PB_TEXPROBE: at the composite draw, report sampler->unit->texture->size
     * and read back the texture the fragment shader will actually sample. */
    if (getenv("PB_TEXPROBE") && count == 1152 && cur_program() == 9) {
        static int n_tp;
        if (n_tp < 3 && G_glGetIntegerv) {
            n_tp++;
            int prog = cur_program();
            typedef int (*pGUL)(unsigned, const char *);
            typedef void (*pGUIV)(unsigned, int, int *);
            typedef void (*pAT)(unsigned);
            pGUL gul = (pGUL)dlsym(G_gles, "glGetUniformLocation");
            pGUIV guiv = (pGUIV)dlsym(G_gles, "glGetUniformiv");
            pAT at = (pAT)dlsym(G_gles, "glActiveTexture");
            const char *un[3] = {"SourceTex", "DirtTex", "CRTTex"};
            for (int i = 0; i < 3 && gul && guiv && at; i++) {
                int loc = gul((unsigned)prog, un[i]);
                if (loc < 0) { fprintf(stderr, "[texprobe] %s: not found\n", un[i]); continue; }
                int unit = -1;
                guiv((unsigned)prog, loc, &unit);
                at(0x84C0u + (unsigned)(unit < 0 ? 0 : unit));
                int tex = 0, w = 0, h = 0;
                unsigned ifmt = 0;
                ((void (*)(unsigned, int *))G_glGetIntegerv)(0x8069 /*TEXTURE_BINDING_2D*/, &tex);
                if (tex) texinfo_get((unsigned)tex, &w, &h, &ifmt);
                fprintf(stderr, "[texprobe] %s loc=%d unit=%d tex=%u size=%dx%d\n", un[i], loc, unit, tex, w, h);
                if (tex && w > 0 && h > 0 && w <= 4096 && h <= 4096) {
                    typedef void (*pGTP)(unsigned, unsigned, int *);
                    pGTP gtp = (pGTP)dlsym(G_gles, "glGetTexParameteriv");
                    if (gtp) {
                        int mn = 0, mg = 0, ws = 0, wt = 0;
                        gtp(0x0DE1, 0x2801, &mn);
                        gtp(0x0DE1, 0x2800, &mg);
                        gtp(0x0DE1, 0x2802, &ws);
                        gtp(0x0DE1, 0x2803, &wt);
                        fprintf(stderr, "[texprobe]   min=%#x mag=%#x wraps=%#x wrapt=%#x ifmt=%#x\n",
                                mn, mg, ws, wt, ifmt);
                    }
                    tex_probe(un[i], tex, w, h);
                }
            }
            if (at) at(0x84C0);
            fflush(stderr);
        }
    }
    if (getenv("PB_VBATTR") && count == 1152 && cur_program() == 9) {
        static int n_vbattr;
        if (n_vbattr < 2 && r_GetVertAttribiv) {
            n_vbattr++;
            unsigned abuf = 0;
            ((void (*)(unsigned, int *))G_glGetIntegerv)(0x8894 /*ARRAY_BUFFER_BINDING*/, (int *)&abuf);
            vbo_shadow *vs = vbo_slot(abuf, 0);
            fprintf(stderr, "[vbattr] prog=9 count=%d abuf=%u size=%ld shadow=%d viewport %s\n",
                    count, abuf, vs ? vs->size : -1L, vs != NULL,
                    getenv("PB_DUMP_FBO") ? "(fbo)" : "");
            for (int i = 0; i < 6; i++) {
                int en = 0, sz = 0, ty = 0, st = 0, bb = 0, off = 0;
                r_GetVertAttribiv(i, 0x8622, &en);
                r_GetVertAttribiv(i, 0x8623, &sz);
                r_GetVertAttribiv(i, 0x8625, &ty);
                r_GetVertAttribiv(i, 0x8624, &st);
                r_GetVertAttribiv(i, 0x889F, &bb);
                r_GetVertAttribiv(i, 0x8645, &off);
                if (!en) {
                    int gv[4] = {0,0,0,0};
                    if (r_GetVertAttribiv) {
                        r_GetVertAttribiv(i, 0x8626, gv);      /* CURRENT_VERTEX_ATTRIB */
                        fprintf(stderr, "[vbattr] attrib%d DISABLED current=0x%08x (%d %d %d %d)\n",
                                i, (unsigned)gv[0], gv[0], gv[1], gv[2], gv[3]);
                    }
                    continue;
                }
                fprintf(stderr, "[vbattr] attrib%d size=%d type=%#x stride=%d buf=%d off=%d",
                        i, sz, ty, st, bb, off);
                if (vs && st > 0) {
                    for (int v = 0; v < 2; v++) {
                        long o = (long)off + (long)v * st;
                        if (o >= 0 && o + 24 <= (long)sizeof vs->head) {
                            const unsigned char *b = vs->head + o;
                            fprintf(stderr, " | v%d:", v);
                            for (int k = 0; k < 24; k++) fprintf(stderr, " %02x", b[k]);
                        }
                    }
                }
                fprintf(stderr, "\n");
            }
            fflush(stderr);
        }
    }
    if (r_DrawArrays) r_DrawArrays(mode, first, count);
    probe_fb0("DA", count);
    if (count == 1152) crt_dbg();
    if (count == 600) tex_probe("ui", (unsigned)G_tex_bind[0], 512, 512);
    if (cur_program() == 3) scan_fb0("ui", count);
    if (cur_program() == 63) probe_fbo1_scene("scene", count);
    ring_push(cur_program(), count, G_last_draw_fbo, 0);
    if (G_n_draws + G_n_elems <= 400) {
        unsigned e = cur_gl_error();
        char tb[32];
        fprintf(stderr, "[gl] DrawArrays(mode=%u first=%d count=%d) fbo=%d prog=%d err=%#x n=%lu %s\n",
                mode, first, count, G_last_draw_fbo, cur_program(), e, G_n_draws + G_n_elems, tidtag(tb, sizeof tb));
        fflush(stderr);
    }
}

static void w_glDrawElements(unsigned mode, int count, unsigned type, const void *idx)
{
    G_n_elems++;
    G_last_draw_fbo = cur_fbo();
    if (G_last_draw_fbo > 0) G_frame_fbo = G_last_draw_fbo;
    if (G_n_draws + G_n_elems <= 160) {
        unsigned pre = cur_gl_error();
        if (pre)
            fprintf(stderr, "[gl] PRE-ERR before DrawElements n=%lu = %#x\n", G_n_draws + G_n_elems, pre);
        dump_draw_state(G_n_draws + G_n_elems);
    }
    force_nodepth_apply();
    if (r_DrawElements) r_DrawElements(mode, count, type, idx);
    probe_fb0("DE", count);
    if (cur_program() == 63 && getenv("PB_MATDBG")) {
        static unsigned dn;
        if (G_n_draws + G_n_elems > 300 && dn++ < 2) {
            dump_draw_state(G_n_draws + G_n_elems);
            dump_uniforms(63);
            for (int l = 0; l < 32; l++) if (G_mtx63_set[l]) {
                const float *m = G_mtx63[l];
                fprintf(stderr, "[gl]   mtx63 loc=%d [%.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g]\n",
                        l, m[0],m[1],m[2],m[3], m[4],m[5],m[6],m[7],
                        m[8],m[9],m[10],m[11], m[12],m[13],m[14],m[15]);
            }
            fflush(stderr);
        }
    }
    if (cur_program() == 63) probe_fbo1_scene("sceneE", count);
    ring_push(cur_program(), count, G_last_draw_fbo, 1);
    if (G_n_draws + G_n_elems <= 400) {
        unsigned e = cur_gl_error();
        char tb[32];
        fprintf(stderr, "[gl] DrawElements(mode=%u count=%d type=%#x) fbo=%d prog=%d err=%#x n=%lu %s\n",
                mode, count, type, G_last_draw_fbo, cur_program(), e, G_n_draws + G_n_elems, tidtag(tb, sizeof tb));
        fflush(stderr);
    }
}

static void (*r_ClearColorFn)(float, float, float, float);
static void w_glClearColor(float r, float g, float b, float a)
{
    {
        static unsigned n_cc;
        if (n_cc < 30) {
            n_cc++;
            fprintf(stderr, "[gl] ClearColor(%.2f,%.2f,%.2f,%.2f)\n", r, g, b, a);
            fflush(stderr);
        }
    }
    if (r_ClearColorFn) r_ClearColorFn(r, g, b, a);
}

/* PB_BRIGHT_INJ: the CRT builder (0x123344) calls glBindFramebuffer/glViewport
 * at its very start, then reads the fade/brightness at 0x1236a8.  Injecting the
 * value in glViewport therefore lands *before* the engine reads it. */
static void (*r_ViewportFn)(int, int, int, int);
static int g_bright_inj = -1;              /* -1 = unset, 0 = off */
static uint32_t g_bright_val = 0;
static void pb_bright_inject(void)
{
    if (g_bright_inj < 0) {
        const char *b = getenv("PB_BRIGHT");
        const char *inj = getenv("PB_BRIGHT_INJ");
        g_bright_inj = (b && inj) ? 1 : 0;
        g_bright_val = b ? (uint32_t)strtoul(b, NULL, 0) : 0;
    }
    if (g_bright_inj && PB.base)
        *(volatile uint32_t *)((char *)PB.base + 0x19e8cf8) = g_bright_val;
}
static void w_glViewport(int x, int y, int w, int h)
{
    pb_bright_inject();
    if (r_ViewportFn) r_ViewportFn(x, y, w, h);
}

static void w_glClear(unsigned mask)
{
    G_n_clears++;
    {
        static unsigned n_cl;
        if (n_cl < 40) {
            n_cl++;
            int fbo = -2;
            if (G_glGetIntegerv) ((void (*)(unsigned, int *))G_glGetIntegerv)(0x8CA6, &fbo);
            fprintf(stderr, "[gl] Clear(mask=%#x) fbo=%d\n", mask, fbo);
            fflush(stderr);
        }
    }
    if (r_Clear) r_Clear(mask);
}

/* ------------------------------------------------------- surface bookkeeping */
typedef struct { EGLSurface s; int w, h; } surf_rec;
static surf_rec G_surfs[32];
static int G_nsurfs;

static surf_rec *surf_find(EGLSurface s)
{
    for (int i = 0; i < G_nsurfs; i++)
        if (G_surfs[i].s == s) return &G_surfs[i];
    return NULL;
}

static void surf_track(EGLSurface s, int w, int h)
{
    if (!s || G_nsurfs >= (int)(sizeof G_surfs / sizeof G_surfs[0])) return;
    G_surfs[G_nsurfs].s = s;
    G_surfs[G_nsurfs].w = w;
    G_surfs[G_nsurfs].h = h;
    G_nsurfs++;
}

static void surf_untrack(EGLSurface s)
{
    for (int i = 0; i < G_nsurfs; i++) {
        if (G_surfs[i].s == s) {
            G_surfs[i] = G_surfs[G_nsurfs - 1];
            G_nsurfs--;
            return;
        }
    }
}

/* ---------------------------------------------------------------- loading */
static const char *angle_candidates[] = {
    "/Applications/Mac.apk.app/Contents/Resources",
    "/Applications/Visual Studio Code.app/Contents/Frameworks/Electron Framework.framework/Libraries",
    "/Applications/Slack.app/Contents/Frameworks/Electron Framework.framework/Libraries",
    "/Applications/Google Chrome.app/Contents/Frameworks/Google Chrome Framework.framework/Libraries",
};

static void *load_pair(const char *dir)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/libGLESv2.dylib", dir);
    void *g = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!g) return NULL;
    snprintf(path, sizeof path, "%s/libEGL.dylib", dir);
    void *e = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!e) { dlclose(g); return NULL; }
    G_gles = g;
    G_egl = e;
    return e;
}

static void resolve_egl(void)
{
    r_GetPlatformDisplayEXT = (void *)dlsym(G_egl, "eglGetPlatformDisplayEXT");
    r_GetDisplay            = (void *)dlsym(G_egl, "eglGetDisplay");
    r_Initialize            = (void *)dlsym(G_egl, "eglInitialize");
    r_Terminate             = (void *)dlsym(G_egl, "eglTerminate");
    r_QueryString           = (void *)dlsym(G_egl, "eglQueryString");
    r_ChooseConfig          = (void *)dlsym(G_egl, "eglChooseConfig");
    r_GetConfigs            = (void *)dlsym(G_egl, "eglGetConfigs");
    r_GetConfigAttrib       = (void *)dlsym(G_egl, "eglGetConfigAttrib");
    r_CreatePbufferSurface  = (void *)dlsym(G_egl, "eglCreatePbufferSurface");
    r_DestroySurface        = (void *)dlsym(G_egl, "eglDestroySurface");
    r_QuerySurface          = (void *)dlsym(G_egl, "eglQuerySurface");
    r_CreateContext         = (void *)dlsym(G_egl, "eglCreateContext");
    r_DestroyContext        = (void *)dlsym(G_egl, "eglDestroyContext");
    r_MakeCurrent           = (void *)dlsym(G_egl, "eglMakeCurrent");
    r_GetCurrentContext     = (void *)dlsym(G_egl, "eglGetCurrentContext");
    r_GetCurrentSurface     = (void *)dlsym(G_egl, "eglGetCurrentSurface");
    r_GetCurrentDisplay     = (void *)dlsym(G_egl, "eglGetCurrentDisplay");
    r_SwapBuffers           = (void *)dlsym(G_egl, "eglSwapBuffers");
    r_GetError              = (void *)dlsym(G_egl, "eglGetError");
    r_GetProcAddress        = (void *)dlsym(G_egl, "eglGetProcAddress");
}

static EGLDisplay egl_get_display(void)
{
    if (G_dpy) return G_dpy;
    const EGLint attrs[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
        EGL_NONE
    };
    if (r_GetPlatformDisplayEXT)
        G_dpy = r_GetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, attrs);
    if (!G_dpy && r_GetDisplay)
        G_dpy = r_GetDisplay(EGL_DEFAULT_DISPLAY);
    return G_dpy;
}

/* ---------------------------------------------------------- presentation */
static uint8_t *G_scratch;
static size_t   G_scratch_cap;
static double   G_last_present_ms;
static int      G_mc_fails;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void present_surface(EGLSurface s)
{
    if (!pb_window_ready()) return;
    surf_rec *r = surf_find(s);
    if (!r || r->w <= 0 || r->h <= 0) return;

    /* PB_PROT_REARM: re-arm the write trap (see main.c PB_PROT_RANGE) once per
     * present, so a writer that runs once per frame can be caught repeatedly. */
    if (getenv("PB_PROT_REARM")) {
        extern void pb_prot_refresh(void);
        pb_prot_refresh();
    }
    {
        static int inited = 0;
        static uint32_t bright = 0;
        if (!inited) {
            const char *b = getenv("PB_BRIGHT");
            bright = b ? (uint32_t)strtoul(b, NULL, 0) : 0xffffffffu;
            inited = 1;
        }
        if (PB.base && bright != 0xffffffffu) {
            static int logged = 0;
            *(volatile uint32_t *)((char *)PB.base + 0x19e8cf8) = bright;
            if (!logged) {
                logged = 1;
                fprintf(stderr, "[bright] wrote %u -> readback %u (base %p) tid=%s\n", bright,
                        *(volatile uint32_t *)((char *)PB.base + 0x19e8cf8), PB.base,
                        tidtag((char[32]){0}, 32));
                fflush(stderr);
            }
        }
    }

    if (getenv("PB_NOPRESENT")) return;              /* pacing probe      */
    double now = now_ms();
    if (now - G_last_present_ms < 8.0) return;       /* ~120 Hz cap       */
    G_last_present_ms = now;

    size_t stride = (size_t)r->w * 4;
    size_t need = stride * (size_t)r->h;
    if (G_scratch_cap < need) {
        uint8_t *nb = realloc(G_scratch, need);
        if (!nb) return;
        G_scratch = nb;
        G_scratch_cap = need;
    }
    if (!G_glReadPixels)
        G_glReadPixels = r_GetProcAddress ? r_GetProcAddress("glReadPixels")
                                          : dlsym(G_gles, "glReadPixels");
    if (!G_glReadPixels) return;
    if (!G_glGetIntegerv)
        G_glGetIntegerv = r_GetProcAddress ? r_GetProcAddress("glGetIntegerv")
                                           : dlsym(G_gles, "glGetIntegerv");
    if (!G_glBindFramebuffer)
        G_glBindFramebuffer = r_GetProcAddress ? r_GetProcAddress("glBindFramebuffer")
                                               : dlsym(G_gles, "glBindFramebuffer");

    /* read the default framebuffer, even if the guest left an FBO bound */
    int prev_fbo = -1;
    if (G_glGetIntegerv)
        ((void (*)(unsigned, int *))G_glGetIntegerv)(0x8CA6 /*GL_FRAMEBUFFER_BINDING*/, &prev_fbo);

    /* capture the FBO the engine last drew into (scene target) */
    uint8_t *fbo_buf = NULL;
    int cap_fbo = (prev_fbo > 0) ? prev_fbo : G_frame_fbo;
    if (cap_fbo <= 0 && getenv("PB_DUMP_FBO")) cap_fbo = 1;   /* scene FBO is stable */
    if (cap_fbo > 0 && G_glBindFramebuffer && G_glReadPixels) {
        fbo_buf = malloc(need);
        if (fbo_buf) {
            ((void (*)(unsigned, unsigned))G_glBindFramebuffer)(0x8D40, (unsigned)cap_fbo);
            ((void (*)(int, int, int, int, unsigned, unsigned, void *))G_glReadPixels)
                (0, 0, r->w, r->h, GL_RGBA, GL_UNSIGNED_BYTE, fbo_buf);
            ((void (*)(unsigned, unsigned))G_glBindFramebuffer)(0x8D40, 0);
        }
    }

    if (prev_fbo > 0 && G_glBindFramebuffer)
        ((void (*)(unsigned, unsigned))G_glBindFramebuffer)(0x8D40 /*GL_FRAMEBUFFER*/, 0);

    ((void (*)(int, int, int, int, unsigned, unsigned, void *))G_glReadPixels)
        (0, 0, r->w, r->h, GL_RGBA, GL_UNSIGNED_BYTE, G_scratch);

    if (prev_fbo > 0 && G_glBindFramebuffer)
        ((void (*)(unsigned, unsigned))G_glBindFramebuffer)(0x8D40, (unsigned)prev_fbo);

    /* PB_TEST_TRI: draw our own fullscreen triangle with raw GL (diagnostic). */
    if (getenv("PB_TEST_TRI") && G_n_draws > 0) {
        static unsigned t_n, t_prog;
        static unsigned (*oCS)(unsigned);
        static void (*oSS)(unsigned, int, const char *const *, const int *);
        static void (*oComp)(unsigned);
        static int (*oGsv)(unsigned, unsigned, int *);
        static void (*oIL)(unsigned, int, int *, char *);
        static unsigned (*oCP)(void);
        static void (*oAS)(unsigned, unsigned);
        static void (*oBAL)(unsigned, unsigned, const char *);
        static void (*oLP)(unsigned);
        static int (*oGpv)(unsigned, unsigned, int *);
        static void (*oPL)(unsigned, int, int *, char *);
        static void (*oUP)(unsigned);
        static void (*oVAP)(unsigned, int, unsigned, unsigned char, int, const void *);
        static void (*oEVA)(unsigned);
        static void (*oDA)(unsigned, int, int);
        static void (*oVp)(int, int, int, int);
        static void (*oDis)(unsigned);
        static void (*oCM)(unsigned char, unsigned char, unsigned char, unsigned char);
        static void (*oBB)(unsigned, unsigned);
        static const float tri[6] = { -1.f, -1.f, 3.f, -1.f, -1.f, 3.f };

        if (t_n < 2) {
            t_n++;
            if (!oCS) {
#define TRIRES(slot, name) do { if (!(slot)) (slot) = (__typeof__(slot)) (r_GetProcAddress ? r_GetProcAddress(name) : dlsym(G_gles, name)); } while (0)
                TRIRES(oCS, "glCreateShader");
                TRIRES(oSS, "glShaderSource");
                TRIRES(oComp, "glCompileShader");
                TRIRES(oGsv, "glGetShaderiv");
                TRIRES(oIL, "glGetShaderInfoLog");
                TRIRES(oCP, "glCreateProgram");
                TRIRES(oAS, "glAttachShader");
                TRIRES(oBAL, "glBindAttribLocation");
                TRIRES(oLP, "glLinkProgram");
                TRIRES(oGpv, "glGetProgramiv");
                TRIRES(oPL, "glGetProgramInfoLog");
                TRIRES(oUP, "glUseProgram");
                TRIRES(oVAP, "glVertexAttribPointer");
                TRIRES(oEVA, "glEnableVertexAttribArray");
                TRIRES(oDA, "glDrawArrays");
                TRIRES(oVp, "glViewport");
                TRIRES(oDis, "glDisable");
                TRIRES(oCM, "glColorMask");
                TRIRES(oBB, "glBindBuffer");
#undef TRIRES
            }
            if (oCS && oSS && oComp && oGsv && oCP && oAS && oBAL && oLP && oGpv && oUP && oVAP && oEVA && oDA && oVp && oDis && oCM && oBB) {
                const char *vs = "attribute vec4 aPos;\nvoid main(){ gl_Position = aPos; }\n";
                const char *fs = "precision mediump float;\nvoid main(){ gl_FragColor = vec4(1.0, 0.5, 0.0, 1.0); }\n";
                unsigned shv, shf;
                int okv = -1, okf = -1, okl = -1;

                shv = oCS(0x8B31);
                oSS(shv, 1, &vs, NULL);
                oComp(shv);
                oGsv(shv, 0x8B81, &okv);
                shf = oCS(0x8B30);
                oSS(shf, 1, &fs, NULL);
                oComp(shf);
                oGsv(shf, 0x8B81, &okf);
                if (okv == 0 && oIL) {
                    char lg[512] = {0};
                    oIL(shv, sizeof lg - 1, NULL, lg);
                    fprintf(stderr, "[tri] VS compile fail: %.400s\n", lg);
                }
                if (okf == 0 && oIL) {
                    char lg[512] = {0};
                    oIL(shf, sizeof lg - 1, NULL, lg);
                    fprintf(stderr, "[tri] FS compile fail: %.400s\n", lg);
                }
                t_prog = oCP();
                oAS(t_prog, shv);
                oAS(t_prog, shf);
                oBAL(t_prog, 0, "aPos");
                oLP(t_prog);
                oGpv(t_prog, 0x8B82, &okl);
                if (okl == 0 && oPL) {
                    char lg[512] = {0};
                    oPL(t_prog, sizeof lg - 1, NULL, lg);
                    fprintf(stderr, "[tri] link fail: %.400s\n", lg);
                }
                fprintf(stderr, "[tri] prog=%u vs=%d fs=%d link=%d\n", t_prog, okv, okf, okl);
                fflush(stderr);
                if (okl == 1) {
                    (void)cur_gl_error();
                    ((void (*)(unsigned, unsigned))G_glBindFramebuffer)(0x8D40, 0);
                    oVp(0, 0, r->w, r->h);
                    oDis(0x0B71);
                    oDis(0x0BE2);
                    oDis(0x0B44);
                    oDis(0x0C11);
                    oDis(0x0B90);
                    oCM(1, 1, 1, 1);
                    oBB(0x8892, 0);
                    oUP(t_prog);
                    oVAP(0, 2, 0x1406, 0, 0, tri);
                    oEVA(0);
                    oDA(0x0004, 0, 3);
                    fprintf(stderr, "[tri] drew err=%#x\n", cur_gl_error());
                    fflush(stderr);
                    uint8_t *tb = malloc(need);
                    if (tb) {
                        char path[512];
                        ((void (*)(int, int, int, int, unsigned, unsigned, void *))G_glReadPixels)
                            (0, 0, r->w, r->h, GL_RGBA, GL_UNSIGNED_BYTE, tb);
                        snprintf(path, sizeof path, "%s_tri.raw", getenv("PB_TEST_TRI"));
                        FILE *pf = fopen(path, "wb");
                        if (pf) {
                            char meta[96];
                            snprintf(meta, sizeof meta, "%d %d tri%u\n", r->w, r->h, t_n);
                            fwrite(meta, 1, strlen(meta), pf);
                            fwrite(tb, 1, need, pf);
                            fclose(pf);
                        }
                        free(tb);
                    }
                }
            } else {
                fprintf(stderr, "[tri] pointer resolution incomplete\n");
            }
        }
    }

    /* PB_TEST_CLEAR: probe the FB0 write+read path with a forced magenta clear */
    if (getenv("PB_TEST_CLEAR")) {
        static void *p_clr, *p_clrc;
        static unsigned probe_n;
        if (!p_clr)  p_clr  = r_GetProcAddress ? r_GetProcAddress("glClear")      : dlsym(G_gles, "glClear");
        if (!p_clrc) p_clrc = r_GetProcAddress ? r_GetProcAddress("glClearColor") : dlsym(G_gles, "glClearColor");
        if (p_clr && p_clrc && G_glReadPixels && probe_n < 6) {
            probe_n++;
            if (G_glBindFramebuffer) ((void (*)(unsigned, unsigned))G_glBindFramebuffer)(0x8D40, 0);
            ((void (*)(float, float, float, float))p_clrc)(1.0f, 0.0f, 1.0f, 1.0f);
            ((void (*)(unsigned))p_clr)(0x4000);
            uint8_t *pb = malloc(need);
            if (pb) {
                ((void (*)(int, int, int, int, unsigned, unsigned, void *))G_glReadPixels)
                    (0, 0, r->w, r->h, GL_RGBA, GL_UNSIGNED_BYTE, pb);
                char path[512];
                snprintf(path, sizeof path, "%s_probe.raw", getenv("PB_TEST_CLEAR"));
                FILE *pf = fopen(path, "wb");
                if (pf) {
                    char meta[96];
                    snprintf(meta, sizeof meta, "%d %d probe%u\n", r->w, r->h, probe_n);
                    fwrite(meta, 1, strlen(meta), pf);
                    fwrite(pb, 1, need, pf);
                    fclose(pf);
                }
                free(pb);
            }
            fprintf(stderr, "[gl] probe clear #%u done\n", probe_n);
            fflush(stderr);
        }
    }

    uint8_t *out = malloc(need);
    if (!out) return;
    for (int y = 0; y < r->h; y++)
        memcpy(out + (size_t)y * stride,
               G_scratch + (size_t)(r->h - 1 - y) * stride, stride);

    /* PB_DUMP_FRAME=<path>: periodically write the top-down RGBA frame */
    {
        const char *dump = getenv("PB_DUMP_FRAME");
        const char *fdump = getenv("PB_DUMP_FBO");
        static unsigned g_present_n = 0;
        g_present_n++;
        static int g_dump_every = -1;
        if (g_dump_every < 0) {
            const char *de = getenv("PB_DUMP_EVERY");
            g_dump_every = (de && *de) ? atoi(de) : 60;
            if (g_dump_every < 1) g_dump_every = 1;
        }
        if ((g_present_n % 60) == 0) {
            int vp[4] = {0,0,0,0};
            if (G_glGetIntegerv) ((void (*)(unsigned, int *))G_glGetIntegerv)(0x0BA2, vp);
            fprintf(stderr, "[gl] present #%u draws=%lu elems=%lu clears=%lu guest_fbo=%d last_draw_fbo=%d vp=%d,%d %dx%d\n",
                    g_present_n, G_n_draws, G_n_elems, G_n_clears, prev_fbo, G_last_draw_fbo,
                    vp[0], vp[1], vp[2], vp[3]);
        }
        static int g_dump_from = -1;
        if (g_dump_from < 0) {
            const char *df = getenv("PB_DUMP_FROM");
            g_dump_from = (df && *df) ? atoi(df) : 0;
        }
        if (dump && (g_present_n % g_dump_every) == 0 && (int)g_present_n >= g_dump_from) {
            char path[512];
            snprintf(path, sizeof path, "%s_%06u.raw", dump, g_present_n);
            FILE *df = fopen(path, "wb");
            if (df) {
                char meta[128];
                snprintf(meta, sizeof meta, "%d %d %u\n", r->w, r->h, g_present_n);
                fwrite(meta, 1, strlen(meta), df);
                fwrite(out, 1, need, df);
                fclose(df);
            }
        }
        /* PB_DUMP_TRIG=0xADDR:VALUE:MAX:/prefix — write the default-FB frame
         * whenever the guest u32 at image offset ADDR equals VALUE.  Lets us
         * capture the exact frames the engine builds in a given state. */
        {
            const char *trig = getenv("PB_DUMP_TRIG");
            static int trig_n;
            if (trig && trig_n < 64) {
                unsigned long off = strtoul(trig, NULL, 0);
                const char *c1 = strchr(trig, ':');
                if (c1 && PB.base) {
                    unsigned want = (unsigned)strtoul(c1 + 1, NULL, 0);
                    const char *c2 = strchr(c1 + 1, ':');
                    unsigned v = *(volatile unsigned *)((char *)PB.base + off);
                    if (v == want && c2) {
                        const char *c3 = strchr(c2 + 1, ':');
                        char path[512];
                        snprintf(path, sizeof path, "%s_%06u.raw",
                                 c3 ? c3 + 1 : "/tmp/trig", trig_n);
                        FILE *df = fopen(path, "wb");
                        if (df) {
                            char meta[128];
                            snprintf(meta, sizeof meta, "%d %d %u\n", r->w, r->h,
                                     g_present_n);
                            fwrite(meta, 1, strlen(meta), df);
                            fwrite(out, 1, need, df);
                            fclose(df);
                        }
                        fprintf(stderr, "[trig] present #%u off=%#lx v=%u -> %s\n",
                                g_present_n, off, v, path);
                        trig_n++;
                    }
                }
            }
        }
        /* PB_DUMP_ALLFBO=<path>: one-shot survey of every FBO's content */
        if (getenv("PB_DUMP_ALLFBO") && g_present_n == 120) {
            unsigned (*chk)(unsigned) = (unsigned (*)(unsigned))
                (r_GetProcAddress ? r_GetProcAddress("glCheckFramebufferStatus")
                                  : dlsym(G_gles, "glCheckFramebufferStatus"));
            unsigned char *pb = malloc(19200 * 4);
            for (int f = 1; f <= 12 && pb; f++) {
                ((void (*)(unsigned, unsigned))G_glBindFramebuffer)(0x8D40, (unsigned)f);
                unsigned st = chk ? chk(0x8D40) : 0;
                if (st != 0x8CD5) {
                    fprintf(stderr, "[allfbo] fbo=%d status=%#x\n", f, st);
                    continue;
                }
                memset(pb, 0, 19200 * 4);
                ((void (*)(int, int, int, int, unsigned, unsigned, void *))G_glReadPixels)
                    (0, 0, 160, 120, GL_RGBA, GL_UNSIGNED_BYTE, pb);
                {
                    unsigned seen[64];
                    int nseen = 0;
                    for (int i = 0; i < 19200 && nseen < 64; i += 7) {
                        uint32_t c = *(uint32_t *)(pb + (size_t)i * 4);
                        int j, found = 0;
                        for (j = 0; j < nseen; j++)
                            if (seen[j] == c) { found = 1; break; }
                        if (!found) seen[nseen++] = c;
                    }
                    fprintf(stderr, "[allfbo] fbo=%d complete distinct=%d c0=%08x c1=%08x c2=%08x\n",
                            f, nseen, nseen > 0 ? seen[0] : 0, nseen > 1 ? seen[1] : 0, nseen > 2 ? seen[2] : 0);
                }
            }
            free(pb);
            ((void (*)(unsigned, unsigned))G_glBindFramebuffer)(0x8D40, (unsigned)(prev_fbo > 0 ? prev_fbo : 0));
            fflush(stderr);
        }
        if (fdump && (g_present_n % 60) == 0)
            fprintf(stderr, "[fbodump] cap=%d buf=%p prev=%d gff=%d\n",
                    cap_fbo, (void *)fbo_buf, prev_fbo, G_frame_fbo);
        G_frame_fbo = -1;
        if (fdump && fbo_buf && (g_present_n % 60) == 0) {
            char path[512];
            snprintf(path, sizeof path, "%s_%06u.raw", fdump, g_present_n);
            FILE *df = fopen(path, "wb");
            if (df) {
                char meta[128];
                snprintf(meta, sizeof meta, "%d %d %u fbo=%d\n", r->w, r->h, g_present_n, prev_fbo);
                fwrite(meta, 1, strlen(meta), df);
                fwrite(fbo_buf, 1, need, df);
                fclose(df);
            }
        }
    }
    free(fbo_buf);

    pb_window_publish(out, r->w, r->h);              /* takes ownership   */
}

/* ------------------------------------------------------------ EGL wrappers */
static EGLDisplay w_eglGetDisplay(void *native)
{
    (void)native;
    return egl_get_display();
}

static EGLDisplay w_eglGetPlatformDisplayEXT(EGLenum plat, void *native, const EGLint *attr)
{
    (void)plat; (void)native; (void)attr;
    return egl_get_display();                        /* always Metal      */
}

static EGLBoolean w_eglInitialize(EGLDisplay dpy, EGLint *maj, EGLint *min)
{
    if (!r_Initialize) return EGL_FALSE;
    return r_Initialize(dpy, maj, min);
}

static EGLBoolean w_eglTerminate(EGLDisplay dpy)
{
    return r_Terminate ? r_Terminate(dpy) : EGL_TRUE;
}

static const char *w_eglQueryString(EGLDisplay dpy, EGLint name)
{
    return r_QueryString ? r_QueryString(dpy, name) : "";
}

static EGLBoolean w_eglChooseConfig(EGLDisplay dpy, const EGLint *attr, EGLConfig *cfgs,
                                    EGLint n, EGLint *got)
{
    return r_ChooseConfig ? r_ChooseConfig(dpy, attr, cfgs, n, got) : EGL_FALSE;
}

static EGLBoolean w_eglGetConfigs(EGLDisplay dpy, EGLConfig *cfgs, EGLint n, EGLint *got)
{
    return r_GetConfigs ? r_GetConfigs(dpy, cfgs, n, got) : EGL_FALSE;
}

static EGLBoolean w_eglGetConfigAttrib(EGLDisplay dpy, EGLConfig cfg, EGLint name, EGLint *val)
{
    return r_GetConfigAttrib ? r_GetConfigAttrib(dpy, cfg, name, val) : EGL_FALSE;
}

static EGLSurface w_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig cfg, void *win,
                                           const EGLint *attr)
{
    (void)win; (void)attr;
    int w = pb_win_w(), h = pb_win_h();
    const EGLint pb[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    EGLSurface s = r_CreatePbufferSurface ? r_CreatePbufferSurface(dpy, cfg, pb)
                                          : EGL_NO_SURFACE;
    if (s != EGL_NO_SURFACE) {
        surf_track(s, w, h);
        pb_log("[egl] window surface -> pbuffer %p (%dx%d)\n", s, w, h);
    } else {
        pb_log("[egl] createWindowSurface FAILED err=%#x\n", r_GetError ? r_GetError() : -1);
    }
    return s;
}

static EGLSurface w_eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig cfg, const EGLint *attr)
{
    EGLSurface s = r_CreatePbufferSurface ? r_CreatePbufferSurface(dpy, cfg, attr)
                                          : EGL_NO_SURFACE;
    if (s != EGL_NO_SURFACE && attr) {
        int w = 0, h = 0;
        for (const EGLint *p = attr; p[0] != EGL_NONE; p += 2) {
            if (p[0] == EGL_WIDTH) w = p[1];
            else if (p[0] == EGL_HEIGHT) h = p[1];
        }
        surf_track(s, w, h);
    }
    return s;
}

static EGLBoolean w_eglDestroySurface(EGLDisplay dpy, EGLSurface s)
{
    surf_untrack(s);
    return r_DestroySurface ? r_DestroySurface(dpy, s) : EGL_TRUE;
}

static EGLBoolean w_eglQuerySurface(EGLDisplay dpy, EGLSurface s, EGLint name, EGLint *val)
{
    surf_rec *r = surf_find(s);
    if (r) {
        if (name == EGL_WIDTH && r->w > 0)  { *val = r->w; return EGL_TRUE; }
        if (name == EGL_HEIGHT && r->h > 0) { *val = r->h; return EGL_TRUE; }
    }
    return r_QuerySurface ? r_QuerySurface(dpy, s, name, val) : EGL_FALSE;
}

static EGLContext w_eglCreateContext(EGLDisplay dpy, EGLConfig cfg, EGLContext share,
                                     const EGLint *attr)
{
    return r_CreateContext ? r_CreateContext(dpy, cfg, share, attr) : EGL_NO_CONTEXT;
}

static EGLBoolean w_eglDestroyContext(EGLDisplay dpy, EGLContext ctx)
{
    return r_DestroyContext ? r_DestroyContext(dpy, ctx) : EGL_TRUE;
}

static EGLBoolean w_eglMakeCurrent(EGLDisplay dpy, EGLSurface d, EGLSurface r, EGLContext ctx)
{
    EGLBoolean ok = r_MakeCurrent ? r_MakeCurrent(dpy, d, r, ctx) : EGL_FALSE;
    {
        static int mc_n;
        if (mc_n < 40) {
            mc_n++;
            char tb[32];
            fprintf(stderr, "[egl] makeCurrent ctx=%p draw=%p read=%p ok=%d %s\n", (void *)ctx, (void *)d, (void *)r, (int)ok, tidtag(tb, sizeof tb));
            fflush(stderr);
        }
    }
    if (!ok && r_GetError && G_mc_fails < 3) {
        G_mc_fails++;
        pb_log("[egl] makeCurrent FAILED err=%#x (d=%p r=%p ctx=%p)\n",
               r_GetError(), d, r, ctx);
    }
    return ok;
}

static EGLContext w_eglGetCurrentContext(void)
{
    return r_GetCurrentContext ? r_GetCurrentContext() : EGL_NO_CONTEXT;
}

static EGLSurface w_eglGetCurrentSurface(EGLint which)
{
    return r_GetCurrentSurface ? r_GetCurrentSurface(which) : EGL_NO_SURFACE;
}

static EGLDisplay w_eglGetCurrentDisplay(void)
{
    return r_GetCurrentDisplay ? r_GetCurrentDisplay() : EGL_NO_DISPLAY;
}

extern void pb_prot_refresh(void);
static EGLBoolean w_eglSwapBuffers(EGLDisplay dpy, EGLSurface s)
{
    static unsigned swap_n;
    swap_n++;
    {
        static double t0;
        static unsigned n0;
        double tn = now_ms();
        if (swap_n % 120 == 0) {
            fprintf(stderr, "[swap] %u swaps, %.1f fps\n", swap_n,
                    (t0 > 0 && tn > t0) ? 120000.0 / (tn - t0) : 0.0);
            t0 = tn;
            (void)n0;
        }
    }
    if (getenv("PB_RING") && swap_n > 30) {
        fprintf(stderr, "[swap] #%u\n", swap_n);
        ring_dump("swap");
    }
    present_surface(s);          /* read the freshly-rendered back buffer FIRST */
    framesum_dump();
    EGLBoolean ok = r_SwapBuffers ? r_SwapBuffers(dpy, s) : EGL_TRUE;
    pb_prot_refresh();
    return ok;
}

static EGLBoolean w_eglSwapInterval(EGLDisplay dpy, EGLint interval)
{
    (void)dpy; (void)interval;
    return EGL_TRUE;                                 /* we cap in present  */
}

static EGLint w_eglGetError(void)
{
    return r_GetError ? r_GetError() : 0x3000 /* EGL_SUCCESS */;
}

static EGLBoolean w_eglBindAPI(EGLenum api)
{
    (void)api;
    return EGL_TRUE;
}

static EGLBoolean w_eglReleaseThread(void)
{
    return EGL_TRUE;
}

static EGLBoolean w_eglSurfaceAttrib(EGLDisplay dpy, EGLSurface s, EGLint a, EGLint v)
{
    (void)dpy; (void)s; (void)a; (void)v;
    return EGL_TRUE;
}

/* ------------------------------------------------------------------- GL list
 * Exactly the GLES2 names the engine imports (mirrors android_shim.c).    */
static const char *gl_names[] = {
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

/* --------------------------------------------------------------- install */
void pb_shim_install_egl(void)
{
    if (getenv("PB_HEADLESS")) {
        pb_log("[egl] PB_HEADLESS set; keeping zero stubs\n");
        return;
    }

    const char *env = getenv("PB_ANGLE_DIR");
    if (env && !load_pair(env)) {
        pb_log("[egl] PB_ANGLE_DIR=%s has no usable ANGLE pair\n", env);
        return;
    }
    if (!G_egl) {
        for (size_t i = 0; i < sizeof angle_candidates / sizeof angle_candidates[0]; i++) {
            if (load_pair(angle_candidates[i])) { G_dir = angle_candidates[i]; break; }
        }
    }
    if (!G_egl) {
        pb_log("[egl] ANGLE not found; keeping zero stubs\n");
        return;
    }

    resolve_egl();
    if (!r_Initialize || !r_CreatePbufferSurface || !r_MakeCurrent || !r_GetProcAddress) {
        pb_log("[egl] ANGLE surface too thin; keeping zero stubs\n");
        G_egl = NULL;
        return;
    }

    EGLDisplay dpy = egl_get_display();
    EGLint maj = 0, min = 0;
    if (!dpy || !r_Initialize(dpy, &maj, &min)) {
        pb_log("[egl] ANGLE eglInitialize failed; keeping zero stubs\n");
        G_dpy = NULL;
        G_egl = NULL;
        return;
    }
    const char *ver = r_QueryString ? r_QueryString(dpy, 0x3054 /* EGL_VERSION */) : "";
    pb_log("[egl] ANGLE ready: %s  (%s)  EGL %d.%d\n", G_dir ? G_dir : "?", ver, maj, min);

    /* EGL entry points -> our wrappers */
    pb_shim_register("eglGetDisplay",            (void *)w_eglGetDisplay, false);
    pb_shim_register("eglGetPlatformDisplayEXT", (void *)w_eglGetPlatformDisplayEXT, false);
    pb_shim_register("eglInitialize",            (void *)w_eglInitialize, false);
    pb_shim_register("eglTerminate",             (void *)w_eglTerminate, false);
    pb_shim_register("eglQueryString",           (void *)w_eglQueryString, false);
    pb_shim_register("eglChooseConfig",          (void *)w_eglChooseConfig, false);
    pb_shim_register("eglGetConfigs",            (void *)w_eglGetConfigs, false);
    pb_shim_register("eglGetConfigAttrib",       (void *)w_eglGetConfigAttrib, false);
    pb_shim_register("eglCreateWindowSurface",   (void *)w_eglCreateWindowSurface, false);
    pb_shim_register("eglCreatePbufferSurface",  (void *)w_eglCreatePbufferSurface, false);
    pb_shim_register("eglDestroySurface",        (void *)w_eglDestroySurface, false);
    pb_shim_register("eglQuerySurface",          (void *)w_eglQuerySurface, false);
    pb_shim_register("eglCreateContext",         (void *)w_eglCreateContext, false);
    pb_shim_register("eglDestroyContext",        (void *)w_eglDestroyContext, false);
    pb_shim_register("eglMakeCurrent",           (void *)w_eglMakeCurrent, false);
    pb_shim_register("eglGetCurrentContext",     (void *)w_eglGetCurrentContext, false);
    pb_shim_register("eglGetCurrentSurface",     (void *)w_eglGetCurrentSurface, false);
    pb_shim_register("eglGetCurrentDisplay",     (void *)w_eglGetCurrentDisplay, false);
    pb_shim_register("eglSwapBuffers",           (void *)w_eglSwapBuffers, false);
    pb_shim_register("eglSwapInterval",          (void *)w_eglSwapInterval, false);
    pb_shim_register("eglGetError",              (void *)w_eglGetError, false);
    pb_shim_register("eglBindAPI",               (void *)w_eglBindAPI, false);
    pb_shim_register("eglReleaseThread",         (void *)w_eglReleaseThread, false);
    pb_shim_register("eglSurfaceAttrib",         (void *)w_eglSurfaceAttrib, false);

    /* GL -> straight through to ANGLE (draws/clears wrapped for accounting) */
    r_DrawArrays   = dlsym(G_gles, "glDrawArrays");
    r_DrawElements = dlsym(G_gles, "glDrawElements");
    r_Clear        = dlsym(G_gles, "glClear");
    if (r_DrawArrays)   pb_shim_register("glDrawArrays",   (void *)w_glDrawArrays,   false);
    if (r_DrawElements) pb_shim_register("glDrawElements", (void *)w_glDrawElements, false);
    if (r_Clear)        pb_shim_register("glClear",        (void *)w_glClear,        false);

    /* diagnostic wrappers registered before the generic GL loop */
    r_TexImage2D       = dlsym(G_gles, "glTexImage2D");
    r_CompTexImage2D   = dlsym(G_gles, "glCompressedTexImage2D");
    r_TexSubImage2D    = dlsym(G_gles, "glTexSubImage2D");
    r_VertAttribPtr    = dlsym(G_gles, "glVertexAttribPointer");
    r_UseProgram       = dlsym(G_gles, "glUseProgram");
    r_BufferData       = dlsym(G_gles, "glBufferData");
    r_CompileShader    = dlsym(G_gles, "glCompileShader");
    r_GetShaderiv      = dlsym(G_gles, "glGetShaderiv");
    r_GetShaderInfoLog = dlsym(G_gles, "glGetShaderInfoLog");
    r_LinkProgram      = dlsym(G_gles, "glLinkProgram");
    r_GetProgramiv     = dlsym(G_gles, "glGetProgramiv");
    r_GetActiveUniform = dlsym(G_gles, "glGetActiveUniform");
    r_GetUniformfv     = dlsym(G_gles, "glGetUniformfv");
    r_GetProgramInfoLog= dlsym(G_gles, "glGetProgramInfoLog");
    if (r_TexImage2D)       pb_shim_register("glTexImage2D", (void *)w_glTexImage2D, false);
    if (r_CompTexImage2D)   pb_shim_register("glCompressedTexImage2D", (void *)w_glCompressedTexImage2D, false);
    if (r_TexSubImage2D)    pb_shim_register("glTexSubImage2D", (void *)w_glTexSubImage2D, false);
    if (r_VertAttribPtr)    pb_shim_register("glVertexAttribPointer", (void *)w_glVertexAttribPointer, false);
    if (r_UseProgram)       pb_shim_register("glUseProgram", (void *)w_glUseProgram, false);
    if (r_BufferData)       pb_shim_register("glBufferData", (void *)w_glBufferData, false);
    if (r_CompileShader)    pb_shim_register("glCompileShader", (void *)w_glCompileShader, false);
    r_IsEnabled        = dlsym(G_gles, "glIsEnabled");
    r_ClearColorFn     = dlsym(G_gles, "glClearColor");
    if (r_ClearColorFn)     pb_shim_register("glClearColor", (void *)w_glClearColor, false);
    r_ViewportFn       = dlsym(G_gles, "glViewport");
    if (r_ViewportFn)       pb_shim_register("glViewport", (void *)w_glViewport, false);
    r_DepthMaskW       = dlsym(G_gles, "glDepthMask");
    if (r_DepthMaskW)       pb_shim_register("glDepthMask", (void *)w_glDepthMask, false);
    r_BindBuffer       = dlsym(G_gles, "glBindBuffer");
    r_GetVertAttribiv  = dlsym(G_gles, "glGetVertexAttribiv");
    if (r_BindBuffer)       pb_shim_register("glBindBuffer", (void *)w_glBindBuffer, false);
    r_CreateProgram = dlsym(G_gles, "glCreateProgram");
    if (r_CreateProgram)    pb_shim_register("glCreateProgram", (void *)w_glCreateProgram, false);
    if (r_LinkProgram)      pb_shim_register("glLinkProgram", (void *)w_glLinkProgram, false);

    r_ActiveTexture       = dlsym(G_gles, "glActiveTexture");
    r_BindTexture         = dlsym(G_gles, "glBindTexture");
    r_FramebufferTexture2D= dlsym(G_gles, "glFramebufferTexture2D");
    r_GenFB               = dlsym(G_gles, "glGenFramebuffers");
    r_DelFB               = dlsym(G_gles, "glDeleteFramebuffers");
    r_CheckFB             = dlsym(G_gles, "glCheckFramebufferStatus");
    r_FramebufferRenderbuffer = dlsym(G_gles, "glFramebufferRenderbuffer");
    r_U1i                 = dlsym(G_gles, "glUniform1i");
    r_TexParamI           = dlsym(G_gles, "glTexParameteri");
    if (r_ActiveTexture)        pb_shim_register("glActiveTexture", (void *)w_glActiveTexture, false);
    if (r_BindTexture)          pb_shim_register("glBindTexture", (void *)w_glBindTexture, false);
    if (r_FramebufferTexture2D) pb_shim_register("glFramebufferTexture2D", (void *)w_glFramebufferTexture2D, false);
    if (r_FramebufferRenderbuffer) pb_shim_register("glFramebufferRenderbuffer", (void *)w_glFramebufferRenderbuffer, false);
    if (r_U1i)                  pb_shim_register("glUniform1i", (void *)w_glUniform1i, false);
    if (r_TexParamI)            pb_shim_register("glTexParameteri", (void *)w_glTexParameteri, false);

    r_ShaderSource     = dlsym(G_gles, "glShaderSource");
    r_BindAttrib       = dlsym(G_gles, "glBindAttribLocation");
    if (r_BindAttrib)   pb_shim_register("glBindAttribLocation", (void *)w_glBindAttribLocation, false);
    r_GetAttrib        = dlsym(G_gles, "glGetAttribLocation");
    if (r_GetAttrib)    pb_shim_register("glGetAttribLocation", (void *)w_glGetAttribLocation, false);
    if (getenv("PB_GDEBUG")) {
        void *cb = r_GetProcAddress ? r_GetProcAddress("glDebugMessageCallbackKHR") : NULL;
        void *cb2 = r_GetProcAddress ? r_GetProcAddress("glDebugMessageCallback") : NULL;
        if (cb || cb2) {
            ((void (*)(void *, void *))(cb ? cb : cb2))((void *)pb_gldebug_cb, NULL);
            void *en = r_GetProcAddress ? r_GetProcAddress("glEnable") : dlsym(G_gles, "glEnable");
            if (en) ((void (*)(unsigned))en)(0x8242 /*GL_DEBUG_OUTPUT*/);
            pb_log("[egl] debug callback installed\n");
        } else {
            pb_log("[egl] no debug callback entry point\n");
        }
    }
    if (r_ShaderSource)     pb_shim_register("glShaderSource", (void *)w_glShaderSource, false);
    r_BufferSubData    = dlsym(G_gles, "glBufferSubData");
    r_GUL              = dlsym(G_gles, "glGetUniformLocation");
    r_UM4              = dlsym(G_gles, "glUniformMatrix4fv");
    if (r_BufferSubData)    pb_shim_register("glBufferSubData", (void *)w_glBufferSubData, false);
    if (r_GUL)              pb_shim_register("glGetUniformLocation", (void *)w_glGetUniformLocation, false);
    if (r_UM4)              pb_shim_register("glUniformMatrix4fv", (void *)w_glUniformMatrix4fv, false);

    int got = 0;
    for (size_t i = 0; i < sizeof gl_names / sizeof gl_names[0]; i++) {
        const char *n = gl_names[i];
        void *p = dlsym(G_gles, n);
        if (!p) p = r_GetProcAddress(n);
        if (p) { pb_shim_register(n, p, false); got++; }
    }
    pb_log("[egl] registered: real EGL+%d/%zu GL functions\n",
           got, sizeof gl_names / sizeof gl_names[0]);
}
