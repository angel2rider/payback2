/* angletest.c — P3 stack validation:
 *   1. ANGLE Metal display + ES2 config + plain pbuffer (known good).
 *   2. Battery of IOSurface client-buffer variants (pbuffer + EGLImage).
 *   3. OpenGL-backend display as fallback.
 *   4. glReadPixels readback (the v1 presentation path).
 *
 * run:  DYLD_LIBRARY_PATH=<dir> ./angletest
 */
#include <dlfcn.h>
#include <IOSurface/IOSurface.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *EGLDisplay, *EGLSurface, *EGLContext, *EGLConfig, *EGLClientBuffer, *EGLImage;
typedef int32_t EGLint;
typedef uint32_t EGLBoolean, EGLenum;

#define EGL_DEFAULT_DISPLAY ((void *)0)
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_IMAGE ((EGLImage)0)
#define EGL_NONE 0x3038
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define EGL_EXTENSIONS 0x3055
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_WINDOW_BIT 0x0004
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_CONTEXT_CLIENT_VERSION 0x3098

#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489
#define EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE 0x320D
#define EGL_IOSURFACE_ANGLE 0x3454
#define EGL_IOSURFACE_PLANE_ANGLE 0x345A
#define EGL_TEXTURE_RECTANGLE_ANGLE 0x345B
#define EGL_TEXTURE_TYPE_ANGLE 0x345C
#define EGL_TEXTURE_INTERNAL_FORMAT_ANGLE 0x345D
#define EGL_IOSURFACE_USAGE_HINT_ANGLE 0x348A
#define EGL_IOSURFACE_READ_HINT_ANGLE 0x0001
#define EGL_IOSURFACE_WRITE_HINT_ANGLE 0x0002
#define EGL_BIND_TO_TEXTURE_TARGET_ANGLE 0x348D

#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_UNSIGNED_BYTE 0x1401
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_COLOR_BUFFER_BIT 0x4000

static void *must_dlopen(const char *path)
{
    void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "FATAL dlopen %s: %s\n", path, dlerror()); exit(2); }
    return h;
}
static void *sym(void *h, const char *name)
{
    void *p = dlsym(h, name);
    if (!p) fprintf(stderr, "  (missing %s)\n", name);
    return p;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/Applications/Mac.apk.app/Contents/Resources";
    char p1[4096], p2[4096];
    snprintf(p1, sizeof p1, "%s/libGLESv2.dylib", dir);
    snprintf(p2, sizeof p2, "%s/libEGL.dylib", dir);
    void *hgl = must_dlopen(p1);
    void *hegl = must_dlopen(p2);

    EGLDisplay (*GetPlatformDisplayEXT)(EGLenum, void *, const EGLint *) = (void *)sym(hegl, "eglGetPlatformDisplayEXT");
    EGLBoolean (*Initialize)(EGLDisplay, EGLint *, EGLint *) = (void *)sym(hegl, "eglInitialize");
    const char *(*QueryString)(EGLDisplay, EGLint) = (void *)sym(hegl, "eglQueryString");
    EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) = (void *)sym(hegl, "eglChooseConfig");
    EGLSurface (*CreatePbufferFromClientBuffer)(EGLDisplay, EGLenum, EGLClientBuffer, EGLConfig, const EGLint *) =
        (void *)sym(hegl, "eglCreatePbufferFromClientBuffer");
    EGLSurface (*CreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *) = (void *)sym(hegl, "eglCreatePbufferSurface");
    EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = (void *)sym(hegl, "eglCreateContext");
    EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = (void *)sym(hegl, "eglMakeCurrent");
    EGLBoolean (*SwapBuffers)(EGLDisplay, EGLSurface) = (void *)sym(hegl, "eglSwapBuffers");
    EGLint (*GetError)(void) = (void *)sym(hegl, "eglGetError");
    EGLImage (*CreateImageKHR)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *) =
        (void *)sym(hegl, "eglCreateImageKHR");

    const char *(*glGetString)(unsigned) = (void *)sym(hgl, "glGetString");
    void (*glClearColor)(float, float, float, float) = (void *)sym(hgl, "glClearColor");
    void (*glClear)(unsigned) = (void *)sym(hgl, "glClear");
    void (*glViewport)(int, int, int, int) = (void *)sym(hgl, "glViewport");
    void (*glFinish)(void) = (void *)sym(hgl, "glFinish");
    void (*glReadPixels)(int, int, int, int, unsigned, unsigned, void *) = (void *)sym(hgl, "glReadPixels");

    const int W = 1920, H = 1080;

    CFMutableDictionaryRef props = CFDictionaryCreateMutable(NULL, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
#define SETNUM(k, v) do { CFNumberRef _n = CFNumberCreate(NULL, kCFNumberIntType, &(int){v}); CFDictionarySetValue(props, k, _n); CFRelease(_n); } while (0)
    SETNUM(kIOSurfaceWidth, W);
    SETNUM(kIOSurfaceHeight, H);
    SETNUM(kIOSurfaceBytesPerElement, 4);
    SETNUM(kIOSurfacePixelFormat, 'BGRA');
    IOSurfaceRef io = IOSurfaceCreate(props);
    CFRelease(props);
    printf("[io] surface=%p\n", io);

    const EGLint cfg_attr[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT | EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE
    };

    typedef struct { const char *name; EGLenum type; } plat_t;
    plat_t plats[] = {
        { "metal", EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE },
        { "opengl", EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE },
    };

    for (int pi = 0; pi < 2; pi++) {
        const EGLint plat[] = { EGL_PLATFORM_ANGLE_TYPE_ANGLE, plats[pi].type, EGL_NONE };
        EGLDisplay dpy = GetPlatformDisplayEXT ? GetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, plat) : EGL_NO_DISPLAY;
        EGLint maj = 0, min = 0;
        if (dpy == EGL_NO_DISPLAY || !Initialize(dpy, &maj, &min)) {
            printf("\n[%s] display failed err=%#x\n", plats[pi].name, GetError());
            continue;
        }
        printf("\n[%s] EGL %d.%d \"%s\"\n", plats[pi].name, maj, min, QueryString(dpy, EGL_VERSION));

        EGLConfig cfg = NULL; EGLint n = 0;
        ChooseConfig(dpy, cfg_attr, &cfg, 1, &n);
        if (!cfg) { printf("[%s] no config\n", plats[pi].name); continue; }

        struct { const char *name; EGLint a[10]; int len; } variants[] = {
            { "W,H,PLANE,NONE",                 { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_IOSURFACE_PLANE_ANGLE, 0, EGL_NONE }, 7 },
            { "W,H,NONE",                       { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE }, 5 },
            { "W,H,PLANE,USAGE=WRITE,NONE",     { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_IOSURFACE_PLANE_ANGLE, 0, EGL_IOSURFACE_USAGE_HINT_ANGLE, EGL_IOSURFACE_WRITE_HINT_ANGLE, EGL_NONE }, 9 },
            { "W,H,PLANE,IFMT=GL_RGBA,NONE",    { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_IOSURFACE_PLANE_ANGLE, 0, EGL_TEXTURE_INTERNAL_FORMAT_ANGLE, GL_RGBA, EGL_NONE }, 9 },
            { "W,H,PLANE,IFMT=RGBA8,TYPE=UB,NONE", { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_IOSURFACE_PLANE_ANGLE, 0, EGL_TEXTURE_INTERNAL_FORMAT_ANGLE, GL_RGBA8, EGL_TEXTURE_TYPE_ANGLE, GL_UNSIGNED_BYTE }, 0 }, /* too long for 10; special */
        };

        for (int i = 0; i < 4; i++) {
            EGLSurface s = CreatePbufferFromClientBuffer
                ? CreatePbufferFromClientBuffer(dpy, EGL_IOSURFACE_ANGLE, (EGLClientBuffer)io, cfg, variants[i].a)
                : EGL_NO_SURFACE;
            printf("[%s] pbuffer-cl: %-32s -> %p err=%#x\n", plats[pi].name, variants[i].name, s, GetError());
        }
        {   /* the long variant, full list */
            const EGLint v[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_IOSURFACE_PLANE_ANGLE, 0,
                                 EGL_TEXTURE_INTERNAL_FORMAT_ANGLE, GL_RGBA8,
                                 EGL_TEXTURE_TYPE_ANGLE, GL_UNSIGNED_BYTE, EGL_NONE };
            EGLSurface s = CreatePbufferFromClientBuffer
                ? CreatePbufferFromClientBuffer(dpy, EGL_IOSURFACE_ANGLE, (EGLClientBuffer)io, cfg, v) : EGL_NO_SURFACE;
            printf("[%s] pbuffer-cl: %-32s -> %p err=%#x\n", plats[pi].name, "W,H,PLANE,IFMT=TYPE,UB", s, GetError());
        }
        {   /* BIND_TO_TEXTURE_TARGET */
            const EGLint v[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_IOSURFACE_PLANE_ANGLE, 0,
                                 EGL_BIND_TO_TEXTURE_TARGET_ANGLE, EGL_TEXTURE_RECTANGLE_ANGLE, EGL_NONE };
            EGLSurface s = CreatePbufferFromClientBuffer
                ? CreatePbufferFromClientBuffer(dpy, EGL_IOSURFACE_ANGLE, (EGLClientBuffer)io, cfg, v) : EGL_NO_SURFACE;
            printf("[%s] pbuffer-cl: %-32s -> %p err=%#x\n", plats[pi].name, "W,H,PLANE,BIND=RECT", s, GetError());
        }
        {   /* EGLImage variants */
            const EGLint v1[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_IOSURFACE_PLANE_ANGLE, 0, EGL_NONE };
            EGLImage img = CreateImageKHR ? CreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_IOSURFACE_ANGLE, (EGLClientBuffer)io, v1) : EGL_NO_IMAGE;
            printf("[%s] image:    %-32s -> %p err=%#x\n", plats[pi].name, "W,H,PLANE,NONE", img, GetError());
            const EGLint v2[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_IOSURFACE_PLANE_ANGLE, 0,
                                  EGL_TEXTURE_INTERNAL_FORMAT_ANGLE, GL_RGBA, EGL_TEXTURE_TYPE_ANGLE, GL_UNSIGNED_BYTE, EGL_NONE };
            EGLImage img2 = CreateImageKHR ? CreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_IOSURFACE_ANGLE, (EGLClientBuffer)io, v2) : EGL_NO_IMAGE;
            printf("[%s] image:    %-32s -> %p err=%#x\n", plats[pi].name, "W,H,PLANE,IFMT,TYPE", img2, GetError());
        }

        /* plain pbuffer + clear + readback (v1 presentation path) */
        const EGLint pb[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
        EGLSurface surf = CreatePbufferSurface(dpy, cfg, pb);
        const EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
        EGLContext ctx = CreateContext(dpy, cfg, EGL_NO_CONTEXT, ca);
        printf("[%s] pbuffer=%p ctx=%p makeCurrent=%d renderer=\"%s\"\n",
               plats[pi].name, surf, ctx, MakeCurrent(dpy, surf, surf, ctx),
               glGetString ? (const char *)glGetString(GL_RENDERER) : "?");
        glViewport(0, 0, W, H);
        glClearColor(0.2f, 0.4f, 0.9f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        SwapBuffers(dpy, surf);
        if (glReadPixels) {
            static uint8_t px[4];
            glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            printf("[%s] readback RGBA bottom-left = %02x %02x %02x %02x (expect 33 66 e6 ff)\n",
                   plats[pi].name, px[0], px[1], px[2], px[3]);
        }
    }
    printf("\n[angle] BATTERY DONE\n");
    return 0;
}
