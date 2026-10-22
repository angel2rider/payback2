/* jni_env.c — an in-process canonical JNIEnv/JavaVM.
 *
 * The engine's Java surface is tiny (PaybackActivity callbacks + a handful of
 * statics).  Instead of embedding a JVM we provide the canonical
 * JNINativeInterface table with reference handles, mirroring what the harness
 * proved sufficient.  Slot numbers come from jni_slots.h (generated from
 * AOSP jni.h, so 219=GetJavaVM, 228=ExceptionCheck, 94=GetFieldID, ...).
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pb.h"
#include "jni_slots.h"

#define H_BASE   0x60200000u
#define H_STRIDE 0x10u
#define H_MAX    4096

enum { K_FREE = 0, K_CLASS, K_OBJ, K_ID, K_STR, K_DBB, K_ARR };

typedef struct {
    int kind;
    char cls[128];
    char name[128];
    char sig[160];
    char *str;          /* K_STR payload */
    uint64_t dbb_addr, dbb_cap;
    uint64_t arr_len;
} jref;

static jref refs[H_MAX];

static void *env_table[SLOT_COUNT];
static void *env_cell[1];
static void *vm_table[8];
static void *vm_cell[1];
static char empty_str[1];

static uint64_t mint(int kind, const char *cls, const char *name, const char *sig)
{
    for (int i = 0; i < H_MAX; i++) {
        if (refs[i].kind == K_FREE) {
            refs[i].kind = kind;
            refs[i].cls[0] = 0; refs[i].name[0] = 0; refs[i].sig[0] = 0;
            if (cls) snprintf(refs[i].cls, sizeof refs[i].cls, "%s", cls);
            if (name) snprintf(refs[i].name, sizeof refs[i].name, "%s", name);
            if (sig) snprintf(refs[i].sig, sizeof refs[i].sig, "%s", sig);
            return H_BASE + (uint64_t)i * H_STRIDE;
        }
    }
    static int warned;
    if (!warned) {
        warned = 1;
        fprintf(stderr, "[jni] REF TABLE EXHAUSTED (%d slots); handles now collide\n", H_MAX);
        fflush(stderr);
    }
    return H_BASE; /* exhausted */
}

/* ---- RegisterNatives: the engine's statically linked GameActivity -------
 * JNI_OnLoad registers the GameActivity lifecycle natives (onTouchEventNative,
 * onKeyDownNative, onSurfaceChangedNative, ...) through RegisterNatives.  The
 * port has no Java VM to call them, so we record the table here and invoke the
 * entries ourselves when the host window produces input. */
typedef struct {
    uint64_t cls;
    char     name[64];
    char     sig[96];
    void    *fn;
} pb_native;

static pb_native g_natives[96];
static int g_n_natives;

static int j_RegisterNatives(void *e, uint64_t cls, const void *methods, int n)
{
    (void)e;
    const uint8_t *p = (const uint8_t *)methods;
    for (int i = 0; i < n && p; i++) {
        uint64_t name_p = *(const uint64_t *)(p + (size_t)i * 24);
        uint64_t sig_p  = *(const uint64_t *)(p + (size_t)i * 24 + 8);
        uint64_t fn     = *(const uint64_t *)(p + (size_t)i * 24 + 16);
        if (g_n_natives >= (int)(sizeof g_natives / sizeof g_natives[0])) break;
        pb_native *r = &g_natives[g_n_natives++];
        r->cls = cls;
        snprintf(r->name, sizeof r->name, "%s", (const char *)(uintptr_t)name_p);
        snprintf(r->sig, sizeof r->sig, "%s", (const char *)(uintptr_t)sig_p);
        r->fn = (void *)(uintptr_t)fn;
        pb_log("[jni] RegisterNatives %s%s -> %p (cls=%#llx)\n",
               r->name, r->sig, r->fn, (unsigned long long)cls);
    }
    return 0;
}

void *pb_jni_native(const char *name)
{
    for (int i = 0; i < g_n_natives; i++)
        if (!strcmp(g_natives[i].name, name)) return g_natives[i].fn;
    return NULL;
}

/* ---- input event state -------------------------------------------------
 * The GameActivity helpers convert a Java MotionEvent/KeyEvent into their
 * internal structs by calling these getters through the JNI env.  There is
 * only ever one event in flight, so a small global is enough. */
static struct {
    int       action;
    float     x, y;
    long long event_ms, down_ms;
    int       key_code, key_scan, key_repeat, key_flags, key_unicode;
} g_ev;

void pb_jni_set_motion(int action, float x, float y, long long event_ms, long long down_ms)
{
    g_ev.action = action;
    g_ev.x = x;
    g_ev.y = y;
    g_ev.event_ms = event_ms;
    g_ev.down_ms = down_ms;
}

void pb_jni_set_key(int code, int unicode, int repeat, int flags)
{
    g_ev.key_code = code;
    g_ev.key_unicode = unicode;
    g_ev.key_repeat = repeat;
    g_ev.key_flags = flags;
    g_ev.key_scan = 0;
}

/* Method/field IDs are stable in real JNI: GetMethodID returns the same id
 * for the same (class, name, sig).  The engine looks IDs up on every call
 * (hundreds of times a second); without this dedupe the ref table fills up
 * and every handle after that collides with slot 0, which silently changes
 * what j_prim() dispatches on. */
static uint64_t mint_member(int kind, const char *cls, const char *name, const char *sig)
{
    const char *c = cls ? cls : "", *n = name ? name : "", *s = sig ? sig : "";
    for (int i = 0; i < H_MAX; i++) {
        jref *r = &refs[i];
        if (r->kind == kind && !strcmp(r->cls, c) && !strcmp(r->name, n) && !strcmp(r->sig, s))
            return H_BASE + (uint64_t)i * H_STRIDE;
    }
    return mint(kind, c, n, s);
}

static jref *deref(uint64_t h)
{
    if (h < H_BASE) return NULL;
    uint64_t i = (h - H_BASE) / H_STRIDE;
    if (i >= H_MAX || (h - H_BASE) % H_STRIDE) return NULL;
    return refs[i].kind ? &refs[i] : NULL;
}

static const char *cls_of(uint64_t h)
{
    jref *r = deref(h);
    return r ? (r->kind == K_CLASS ? r->name : r->cls) : "";
}

static const char *name_of_jref(uint64_t h, const char *def)
{
    jref *r = deref(h);
    return (r && r->name[0]) ? r->name : def;
}

/* ---------------- handlers (all take the raw guest register args) ------ */

static uint64_t j_GetVersion(void *e, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t f)
{ return 0x00010006; }

static uint64_t j_FindClass(void *e, const char *name)
{ return mint(K_CLASS, name, name, 0); }

static uint64_t j_GetObjectClass(void *e, uint64_t obj)
{ return mint(K_CLASS, cls_of(obj), cls_of(obj), 0); }

static uint64_t j_NewGlobalRef(void *e, uint64_t obj)
{
    jref *r = deref(obj);
    if (!r) return 0;
    uint64_t h = mint(r->kind == K_CLASS ? K_CLASS : K_OBJ, r->cls, r->name, r->sig);
    return h;
}

static uint64_t j_NewLocalRef(void *e, uint64_t obj) { return j_NewGlobalRef(e, obj); }
static void j_DeleteRef(void *e, uint64_t obj) { jref *r = deref(obj); if (r) r->kind = K_FREE; }
static uint64_t j_IsSameObject(void *e, uint64_t a, uint64_t b) { return a == b; }

/* PB_JNIDBG: log each distinct method/field lookup once, so we can see
 * exactly which Java surface the engine expects. */
static void j_lookup_log(const char *what, uint64_t cls, const char *name, const char *sig)
{
    static char seen[256][160];
    static int nseen;
    if (!getenv("PB_JNIDBG")) return;
    char key[160];
    snprintf(key, sizeof key, "%s %s.%s%s", what, cls_of(cls) ? cls_of(cls) : "?",
             name ? name : "?", sig ? sig : "");
    for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], key)) return;
    if (nseen < 256) snprintf(seen[nseen++], sizeof seen[0], "%s", key);
    fprintf(stderr, "[jni-lookup] %s\n", key);
    fflush(stderr);
}

static uint64_t j_GetMethodID(void *e, uint64_t cls, const char *name, const char *sig)
{ j_lookup_log("method", cls, name, sig); return mint_member(K_ID, cls_of(cls), name, sig); }
static uint64_t j_GetStaticMethodID(void *e, uint64_t cls, const char *name, const char *sig)
{ j_lookup_log("smethod", cls, name, sig); return mint_member(K_ID, cls_of(cls), name, sig); }
static uint64_t j_GetFieldID(void *e, uint64_t cls, const char *name, const char *sig)
{ j_lookup_log("field", cls, name, sig); return mint_member(K_ID, cls_of(cls), name, sig); }

static void pb_jni_note(const char *cls, const char *name, const char *sig, uint64_t ret);

/* android.content.res.Configuration: GameActivity's native half reads these.
 * The values mirror a 420 dpi phone in portrait (the AVD). */
static uint64_t j_GetIntField(void *e, uint64_t obj, uint64_t id)
{
    const char *nm = name_of_jref(id, "");
    uint64_t v = 0;
    if (!strcmp(nm, "densityDpi")) v = 420;
    else if (!strcmp(nm, "orientation")) v = 1;          /* ORIENTATION_PORTRAIT */
    else if (!strcmp(nm, "screenLayout")) v = 2;         /* SIZE_NORMAL */
    else if (!strcmp(nm, "touchscreen")) v = 3;
    else if (!strcmp(nm, "keyboard")) v = 1;
    else if (!strcmp(nm, "keyboardHidden")) v = 1;
    else if (!strcmp(nm, "hardKeyboardHidden")) v = 1;
    else if (!strcmp(nm, "navigationHidden")) v = 1;
    else if (!strcmp(nm, "uiMode")) v = 0;
    pb_jni_note(cls_of(obj), nm, "int", v);
    return v;
}
static float j_GetFloatFieldF(void *e, uint64_t obj, uint64_t id)
{
    const char *nm = name_of_jref(id, "");
    float v = 0.0f;
    if (!strcmp(nm, "fontScale")) v = 1.0f;
    pb_jni_note(cls_of(obj), nm, "float", 0);
    return v;
}
static uint64_t j_GetStaticFieldID(void *e, uint64_t cls, const char *name, const char *sig)
{ j_lookup_log("sfield", cls, name, sig); return mint(K_ID, cls_of(cls), name, sig); }

/* ---- device language: PaybackActivity.ANDROID_getLanguage() ----------- *
 * The glyph loader strcmps its first 2 chars against the engine's own
 * locale table (de=0 en=1 es=2 fr=3 it=4, img+0x396430..0x396450) and
 * stores the index at img+0x396458 (the glyph-table row selector).
 * PB_LANG overrides the answer; default "en". */
#define PB_LANG_IDX_DE 0
#define PB_LANG_IDX_EN 1
#define PB_LANG_IDX_ES 2
#define PB_LANG_IDX_FR 3
#define PB_LANG_IDX_IT 4

static const char *pb_lang_code(void)
{
    static char buf[3];
    const char *l = getenv("PB_LANG");
    if (!l || !*l) l = "en";
    buf[0] = (char)tolower((unsigned char)l[0]);
    buf[1] = l[1] ? (char)tolower((unsigned char)l[1]) : 0;
    buf[2] = 0;
    return buf;
}

static uint32_t pb_lang_index(void)
{
    const char *c = pb_lang_code();
    if (!strcmp(c, "de")) return PB_LANG_IDX_DE;
    if (!strcmp(c, "es")) return PB_LANG_IDX_ES;
    if (!strcmp(c, "fr")) return PB_LANG_IDX_FR;
    if (!strcmp(c, "it")) return PB_LANG_IDX_IT;
    return PB_LANG_IDX_EN;
}

void pb_jni_lang_fixup(void)
{
    if (!PB.base) return;
    /* The engine's file ships a scrambled initial value in the language
       slot; the compare chain rewrites it only on a locale match.  Seed
       the PB_LANG row so an unmatched locale still indexes a valid row. */
    *(uint32_t *)(PB.base + 0x396458) = pb_lang_index();
}

/* ---------------------------------------------------------------- handlers */

/* ---- PB_JNIDBG: trace Java-side calls the engine makes ----------------- */
static void pb_jni_note(const char *cls, const char *name, const char *sig, uint64_t ret)
{
    if (!getenv("PB_JNIDBG")) return;
    static struct { char key[192]; unsigned n; } seen[256];
    static int nseen;
    char key[192];
    snprintf(key, sizeof key, "%s.%s%s", cls && *cls ? cls : "?", name && *name ? name : "?",
             sig ? sig : "");
    for (int i = 0; i < nseen; i++) {
        if (!strcmp(seen[i].key, key)) {
            seen[i].n++;
            if (seen[i].n <= 4 || (seen[i].n % 1000) == 0) {
                fprintf(stderr, "[jni] #%u %s -> %#llx\n", seen[i].n, key, (unsigned long long)ret);
                fflush(stderr);
            }
            return;
        }
    }
    if (nseen < 256) {
        snprintf(seen[nseen].key, sizeof seen[nseen].key, "%s", key);
        seen[nseen].n = 1;
        nseen++;
        fprintf(stderr, "[jni] #1 %s -> %#llx\n", key, (unsigned long long)ret);
        fflush(stderr);
    }
}

static uint64_t j_CallObject(void *e, uint64_t obj, uint64_t id, uint64_t va)
{
    jref *m = deref(id);
    if (m && m->kind == K_ID && !strcmp(m->name, "ANDROID_getLanguage")) {
        const char *lang = pb_lang_code();
        uint64_t h = mint(K_STR, 0, 0, 0);
        jref *r = deref(h);
        if (r) r->str = strdup(lang);
        pb_log("[jni] ANDROID_getLanguage -> \"%s\" (row %u)\n", lang, pb_lang_index());
        pb_jni_note(cls_of(id), name_of_jref(id, "obj"), "", h);
        return h;
    }
    {
        uint64_t h = mint(K_OBJ, cls_of(id), name_of_jref(id, "obj"), 0);
        pb_jni_note(cls_of(obj), name_of_jref(id, "obj"), "", h);
        return h;
    }
}
static uint64_t j_CallStaticObject(void *e, uint64_t cls, uint64_t id, uint64_t va)
{
    jref *m = deref(id);
    if (m && m->kind == K_ID && !strcmp(m->name, "ANDROID_getLanguage")) {
        const char *lang = pb_lang_code();
        uint64_t h = mint(K_STR, 0, 0, 0);
        jref *r = deref(h);
        if (r) r->str = strdup(lang);
        pb_log("[jni] ANDROID_getLanguage (static) -> \"%s\"\n", lang);
        pb_jni_note(cls_of(cls), name_of_jref(id, "obj"), "", h);
        return h;
    }
    {
        uint64_t h = mint(K_OBJ, cls_of(cls), name_of_jref(id, "obj"), 0);
        pb_jni_note(cls_of(cls), name_of_jref(id, "obj"), "", h);
        return h;
    }
}
/* ---- fake intro video ------------------------------------------------ *
 * JNI_OnLoad resolves PaybackActivity.PlayIntro()V and
 * PaybackActivity.ANDROID_isIntroPlaying()Z.  On device the Java layer starts
 * a video and reports whether it is still playing; the native loop polls the
 * boolean.  Emulate a short clip so the engine can leave the intro state. */
static double pb_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static double g_intro_start_ms = -1;
static int g_intro_dbg = -1, g_intro_was = -1;
static int pb_intro_env(void)
{
    if (g_intro_dbg < 0) g_intro_dbg = getenv("PB_INTRODBG") != NULL;
    return g_intro_dbg;
}
static int pb_intro_playing(void)
{
    const char *e = getenv("PB_FAKE_INTRO");
    if (!e || g_intro_start_ms < 0) return 0;
    double dur = atof(e);
    if (dur <= 0) dur = 2.0;
    double t = pb_mono_ms() - g_intro_start_ms;
    int playing = t < dur * 1000.0;
    if (pb_intro_env() && playing != g_intro_was) {
        g_intro_was = playing;
        pb_log("[jni] intro state -> %d (t=%.2fs)\n", playing, t / 1000.0);
    }
    return playing;
}

/* PB_JNI_CALLER=0xOFF[,0xOFF...]: when a JNI call originates from one of the
 * given guest return addresses, log the method name and the guest frame
 * chain.  Used to identify cached jmethodIDs without static analysis. */
static int pb_jni_stack_has(uint64_t want)
{
#ifndef __EMSCRIPTEN__
    uint64_t fp;
    __asm__ volatile("mov %0, x29" : "=r"(fp));
    for (int i = 0; i < 24 && fp >= 0x10000 && fp < 0x800000000000ULL && !(fp & 7); i++) {
        uint64_t ret = 0, prev = 0;
        memcpy(&ret, (void *)(fp + 8), 8);
        if (ret == want) return 1;
        memcpy(&prev, (void *)fp, 8);
        if (prev <= fp) break;
        fp = prev;
    }
#else
    (void)want;
#endif
    return 0;
}

static void pb_jni_caller_check(const char *name, const char *sig)
{
#ifndef __EMSCRIPTEN__
    const char *e = getenv("PB_JNI_CALLER");
    if (!e || !*e) return;
    static int shown;
    if (shown > 12) return;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", e);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save)) {
        uint64_t want = strtoull(tok, NULL, 0);
        int always = !strcmp(tok, "all");
        if (!always && !pb_jni_stack_has(want)) continue;
        if (always) {
            static struct { char key[128]; int n; } seen[128];
            static int nseen;
            char key[128];
            snprintf(key, sizeof key, "%s%s", name, sig);
            int i;
            for (i = 0; i < nseen && strcmp(seen[i].key, key); i++);
            if (i == nseen && nseen < 128) { snprintf(seen[nseen].key, sizeof seen[nseen].key, "%s", key); seen[nseen].n = 0; nseen++; }
            if (i < 128 && ++seen[i].n > 2) continue;
        }
        {
            char out[512];
            int o = snprintf(out, sizeof out, "[jnicall] from %#llx: %s%s chain:", (unsigned long long)want, name, sig);
            uint64_t fp;
            __asm__ volatile("mov %0, x29" : "=r"(fp));
            for (int i = 0; i < 12 && fp >= 0x10000 && fp < 0x800000000000ULL && !(fp & 7); i++) {
                uint64_t ret = 0, prev = 0;
                memcpy(&ret, (void *)(fp + 8), 8);
                o += snprintf(out + o, sizeof out - (size_t)o, " %#llx", (unsigned long long)ret);
                if (o > (int)sizeof out - 40) break;
                memcpy(&prev, (void *)fp, 8);
                if (prev <= fp) break;
                fp = prev;
            }
            fprintf(stderr, "%s\n", out);
            fflush(stderr);
            shown++;
            return;
        }
    }
#else
    (void)name; (void)sig;
#endif
}

static void pb_jni_watch(const char *name, const char *sig)
{
    const char *w = getenv("PB_JNI_WATCH_NAME");
    if (!w || !*w || !strstr(name, w)) return;
    static unsigned n;
    n++;
    pb_log("[jniw] #%u %s%s\n", n, name, sig);
}

static uint64_t j_prim(void *e, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t f)
{
    jref *m = deref(b);
    if (m && m->kind == K_ID) { pb_jni_caller_check(m->name, m->sig); pb_jni_watch(m->name, m->sig); }
    if (!m || m->kind != K_ID) m = deref(c);
    /* GameActivity converts Java MotionEvent/KeyEvent objects into its own
     * structs through these getters.  Dispatch by name on the fake event. */
    if (m && m->kind == K_ID && strstr(m->cls, "MotionEvent")) {
        const char *n = m->name;
        if (!strcmp(n, "getAction"))                          return (uint64_t)(uint32_t)g_ev.action;
        if (!strcmp(n, "getPointerCount"))                    return 1;
        if (!strcmp(n, "getPointerId") || !strcmp(n, "getActionIndex")) return 0;
        if (!strcmp(n, "getHistorySize"))                     return 0;
        if (!strcmp(n, "getEventTime") || !strcmp(n, "getHistoricalEventTime"))
            return (uint64_t)g_ev.event_ms;
        if (!strcmp(n, "getDownTime"))                        return (uint64_t)g_ev.down_ms;
        if (!strcmp(n, "getDeviceId"))                        return 0;
        if (!strcmp(n, "getSource"))                          return 0x1002; /* touchscreen */
        if (!strcmp(n, "getMetaState") || !strcmp(n, "getButtonState") ||
            !strcmp(n, "getFlags") || !strcmp(n, "getActionButton") ||
            !strcmp(n, "getClassification") || !strcmp(n, "getEdgeFlags")) return 0;
        if (!strcmp(n, "getToolType"))                        return 1; /* FINGER */
        return 0;
    }
    if (m && m->kind == K_ID && strstr(m->cls, "KeyEvent")) {
        const char *n = m->name;
        if (!strcmp(n, "getKeyCode"))      return (uint64_t)(uint32_t)g_ev.key_code;
        if (!strcmp(n, "getScanCode"))     return (uint64_t)(uint32_t)g_ev.key_scan;
        if (!strcmp(n, "getUnicodeChar"))  return (uint64_t)(uint32_t)g_ev.key_unicode;
        if (!strcmp(n, "getRepeatCount"))  return (uint64_t)(uint32_t)g_ev.key_repeat;
        if (!strcmp(n, "getFlags"))        return (uint64_t)(uint32_t)g_ev.key_flags;
        if (!strcmp(n, "getMetaState") || !strcmp(n, "getModifiers") ||
            !strcmp(n, "getDeviceId") || !strcmp(n, "getActionButton")) return 0;
        if (!strcmp(n, "getEventTime"))    return (uint64_t)g_ev.event_ms;
        if (!strcmp(n, "getDownTime"))     return (uint64_t)g_ev.down_ms;
        if (!strcmp(n, "getSource"))       return 0x101; /* KEYBOARD */
        return 0;
    }
    if (m && m->kind == K_ID) {
        if (!strcmp(m->name, "PlayIntro")) {
            if (pb_intro_env()) pb_log("[jni] PlayIntro() called\n");
            g_intro_start_ms = pb_mono_ms();
        } else if (!strcmp(m->name, "ANDROID_StorePhrase") && getenv("PB_PHRASES")) {
            /* the engine hands its whole text corpus to Java here */
            jref *st = deref(d);
            fprintf(stderr, "[phrase] %d = %s\n", (int)(int64_t)c,
                    (st && st->str) ? st->str : "<non-string>");
            fflush(stderr);
        } else if (!strcmp(m->name, "ANDROID_isIntroPlaying")) {
            int v = pb_intro_playing();
            if (pb_intro_env()) {
                static int n;
                if (n < 6) { n++; pb_log("[jni] ANDROID_isIntroPlaying -> %d\n", v); }
            }
            pb_jni_note(m->cls, m->name, m->sig, (uint64_t)v);
            return (uint64_t)v;
        } else if (!strcmp(m->name, "GMS_getLastInviteVariant")) {
            /* Java: `int i = mLastAcceptedInviteVariant; ... return i;` with
             * the field initialised to -1.  The engine polls this every
             * second and treats anything != -1 as "an invite was just
             * accepted" -- it then writes the invite code, sets the
             * reload/restart byte [0x1526024] = 1 and tears the front-end
             * level down and back up forever.  Report -1: no invite. */
            pb_jni_note(m->cls, m->name, m->sig, (uint64_t)(int64_t)-1);
            return (uint64_t)(int64_t)-1;
        } else if (!strcmp(m->name, "HW_GetTellAFriendSentType")) {
            /* Java: `int i = shareSentType; ... shareSentType = 3; return i;`
             * -- 3 means "not sent" (shareSentType is initialised to 3).
             * Returning 0 made the engine think a share had just completed
             * on every single frame: it played the reward sound, flagged an
             * achievement and rewrote Backup.sav + Payback.ini (~56x/s). */
            pb_jni_note(m->cls, m->name, m->sig, 3);
            return 3;
        }
    }
    if (m && m->kind == K_ID) {
        pb_jni_note(m->cls, m->name, m->sig, 0);
    } else {
        pb_jni_note("?", "?", "", 0);
    }
    return 0;
}

/* Float-returning Java methods: the result must come back in s0, so these
 * must be declared float (j_prim's integer 0 left s0 = stale garbage). */
static float pb_density_value(void)
{
    const char *d = getenv("PB_DENSITY");
    float v = (d && *d) ? (float)atof(d) : 2.625f;   /* AVD hw.lcd.density = 420 */
    if (!(v >= 0.5f && v <= 8.0f)) v = 2.625f;
    return v;
}

static float j_float_impl(uint64_t id, uint64_t a1, uint64_t a2)
{
    jref *m = deref(id);
    const char *nm = (m && m->kind == K_ID) ? m->name : "";
    if (m && m->kind == K_ID && strstr(m->cls, "MotionEvent")) {
        if (!strcmp(nm, "getX") || !strcmp(nm, "getRawX")) return g_ev.x;
        if (!strcmp(nm, "getY") || !strcmp(nm, "getRawY")) return g_ev.y;
        if (!strcmp(nm, "getAxisValue") || !strcmp(nm, "getHistoricalAxisValue"))
            return ((int)(a1 & 0xff) == 1) ? g_ev.y : g_ev.x;   /* AXIS_Y = 1 */
        if (!strcmp(nm, "getXPrecision") || !strcmp(nm, "getYPrecision") ||
            !strcmp(nm, "getPressure") || !strcmp(nm, "getSize") ||
            !strcmp(nm, "getTouchMajor") || !strcmp(nm, "getTouchMinor")) return 1.0f;
        return 0.0f;
    }
    if (!strcmp(nm, "ANDROID_getScreenDensity")) {
        float v = pb_density_value();
        static int once;
        if (!once) { once = 1; pb_log("[jni] ANDROID_getScreenDensity -> %.4f\n", v); }
        return v;
    }
    pb_jni_note((m && m->kind == K_ID) ? m->cls : "?", nm, m ? m->sig : "", 0);
    return 0.0f;
}

static float j_float_call(void *e, uint64_t obj, uint64_t id, uint64_t a1, uint64_t a2, uint64_t a3)
{
    (void)e; (void)obj; (void)a3;
    return j_float_impl(id, a1, a2);
}

/* CallFloatMethodV: the arguments live in a guest AAPCS64 va_list
 * ({stack, gr_top, vr_top, gr_offs, vr_offs}) pointed to by the 4th arg.
 * GameActivity uses this form for getAxisValue(axis, pointerIndex). */
static float j_float_call_v(void *e, uint64_t obj, uint64_t id, uint64_t va)
{
    (void)e; (void)obj;
    uint64_t a1 = 0, a2 = 0;
    if (va) {
        const uint8_t *p = (const uint8_t *)(uintptr_t)va;
        uint64_t gr_top = *(const uint64_t *)(p + 8);
        int32_t gr_offs = *(const int32_t *)(p + 24);
        if (gr_top && gr_offs < 0) {
            a1 = *(const uint64_t *)(gr_top + gr_offs);
            a2 = *(const uint64_t *)(gr_top + gr_offs + 8);
        }
    }
    return j_float_impl(id, a1, a2);
}

static uint64_t j_GetJavaVM(void *e, uint64_t *out)
{ if (out) *out = (uint64_t)(uintptr_t)vm_cell; return 0; }

static uint64_t j_ExceptionCheck(void *e, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t f) { return 0; }

static void j_FatalError(void *e, const char *msg)
{ fprintf(stderr, "[jni] FatalError: %s\n", msg ? msg : "?"); abort(); }

static uint64_t j_NewStringUTF(void *e, const char *s)
{
    if (!s) return 0;
    uint64_t h = mint(K_STR, 0, 0, 0);
    jref *r = deref(h);
    if (r) r->str = strdup(s);
    return h;
}

static uint64_t j_GetStringUTFChars(void *e, uint64_t s, uint64_t isCopy)
{
    jref *r = deref(s);
    if (r && r->kind == K_STR && r->str) return (uint64_t)(uintptr_t)r->str;
    return (uint64_t)(uintptr_t)empty_str;
}

static uint64_t j_GetStringUTFLength(void *e, uint64_t s)
{ jref *r = deref(s); return (r && r->str) ? strlen(r->str) : 0; }

static uint64_t j_GetStringLength(void *e, uint64_t s) { return j_GetStringUTFLength(e, s); }

static void j_GetStringUTFRegion(void *e, uint64_t s, int start, int len, char *buf)
{
    jref *r = deref(s);
    const char *src = (r && r->str) ? r->str : "";
    int n = (int)strlen(src);
    if (start < 0) start = 0;
    if (len < 0 || start + len > n) len = n - start;
    if (len > 0) memcpy(buf, src + start, (size_t)len);
}

static uint64_t j_NewString(void *e, const uint16_t *u16, int len)
{
    char *buf = malloc((size_t)len * 4 + 1);
    int o = 0;
    for (int i = 0; i < len; i++) {
        uint32_t cp = u16[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len) {
            uint32_t lo = u16[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i++; }
        }
        if (cp < 0x80) buf[o++] = (char)cp;
        else if (cp < 0x800) { buf[o++] = 0xC0 | (cp >> 6); buf[o++] = 0x80 | (cp & 0x3F); }
        else if (cp < 0x10000) { buf[o++] = 0xE0 | (cp >> 12); buf[o++] = 0x80 | ((cp >> 6) & 0x3F); buf[o++] = 0x80 | (cp & 0x3F); }
        else { buf[o++] = 0xF0 | (cp >> 18); buf[o++] = 0x80 | ((cp >> 12) & 0x3F); buf[o++] = 0x80 | ((cp >> 6) & 0x3F); buf[o++] = 0x80 | (cp & 0x3F); }
    }
    buf[o] = 0;
    uint64_t h = mint(K_STR, 0, 0, 0);
    jref *r = deref(h);
    if (r) r->str = buf; else free(buf);
    return h;
}

static uint64_t j_NewDirectByteBuffer(void *e, void *addr, long long cap)
{
    uint64_t h = mint(K_DBB, 0, 0, 0);
    jref *r = deref(h);
    if (r) { r->dbb_addr = (uint64_t)(uintptr_t)addr; r->dbb_cap = (uint64_t)cap; }
    return h;
}
static uint64_t j_GetDirectBufferAddress(void *e, uint64_t b)
{ jref *r = deref(b); return (r && r->kind == K_DBB) ? r->dbb_addr : 0; }
static uint64_t j_GetDirectBufferCapacity(void *e, uint64_t b)
{ jref *r = deref(b); return (r && r->kind == K_DBB) ? r->dbb_cap : 0; }

static uint64_t j_NewArray(void *e, int len)
{
    uint64_t h = mint(K_ARR, 0, 0, 0);
    jref *r = deref(h);
    if (r) r->arr_len = (uint64_t)len;
    return h;
}
static uint64_t j_GetArrayLength(void *e, uint64_t a)
{ jref *r = deref(a); return r ? r->arr_len : 0; }

static uint64_t j_NewObjectArray(void *e, int len, uint64_t cls, uint64_t init)
{
    (void)cls; (void)init;
    return j_NewArray(e, len);
}

static uint64_t j_NewObject(void *e, uint64_t cls, uint64_t id, uint64_t va)
{ return mint(K_OBJ, cls_of(cls), name_of_jref(id, "new"), 0); }

static uint64_t j_AllocObject(void *e, uint64_t cls)
{ return mint(K_OBJ, cls_of(cls), "alloc", 0); }

/* generic no-ops */
static uint64_t j_zero(void *e, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t f) { return 0; }
static void j_void(void *e, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t f) { }
static uint64_t j_null(void *e, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t f) { return 0; }
static uint64_t j_PopLocalFrame(void *e, uint64_t result, uint64_t b, uint64_t c, uint64_t d, uint64_t f) { return result; }

/* -------------------------------- JavaVM ------------------------------- */

static uint64_t vm_DestroyJavaVM(void *vm) { return 0; }
static uint64_t vm_AttachCurrentThread(void *vm, void **env, void *args)
{ if (env) *env = (void *)env_cell; return 0; }
static uint64_t vm_DetachCurrentThread(void *vm) { return 0; }
static uint64_t vm_GetEnv(void *vm, void **env, int version)
{ (void)version; if (env) *env = (void *)env_cell; return 0; }
static uint64_t vm_AttachAsDaemon(void *vm, void **env, void *args)
{ if (env) *env = (void *)env_cell; return 0; }

void pb_jni_init(void)
{
    for (int i = 0; i < SLOT_COUNT; i++) env_table[i] = (void *)j_zero;

    env_table[SLOT_GetVersion] = (void *)j_GetVersion;
    env_table[SLOT_DefineClass] = (void *)j_zero;
    env_table[SLOT_FindClass] = (void *)j_FindClass;
    env_table[SLOT_GetObjectClass] = (void *)j_GetObjectClass;
    env_table[SLOT_IsInstanceOf] = (void *)j_zero;
    env_table[SLOT_Throw] = (void *)j_zero;
    env_table[SLOT_ThrowNew] = (void *)j_zero;
    env_table[SLOT_ExceptionOccurred] = (void *)j_null;
    env_table[SLOT_ExceptionDescribe] = (void *)j_void;
    env_table[SLOT_ExceptionClear] = (void *)j_void;
    env_table[SLOT_FatalError] = (void *)j_FatalError;
    env_table[SLOT_PushLocalFrame] = (void *)j_zero;
    env_table[SLOT_PopLocalFrame] = (void *)j_PopLocalFrame;
    env_table[SLOT_NewGlobalRef] = (void *)j_NewGlobalRef;
    env_table[SLOT_DeleteGlobalRef] = (void *)j_DeleteRef;
    env_table[SLOT_DeleteLocalRef] = (void *)j_DeleteRef;
    env_table[SLOT_IsSameObject] = (void *)j_IsSameObject;
    env_table[SLOT_NewLocalRef] = (void *)j_NewLocalRef;
    env_table[SLOT_EnsureLocalCapacity] = (void *)j_zero;
    env_table[SLOT_AllocObject] = (void *)j_AllocObject;
    env_table[SLOT_NewObject] = (void *)j_NewObject;
    env_table[SLOT_NewObjectV] = (void *)j_NewObject;
    env_table[SLOT_NewObjectA] = (void *)j_NewObject;
    env_table[SLOT_GetMethodID] = (void *)j_GetMethodID;

    /* Call<Type>Method(V/A) — object-returning mint, primitives zero */
    env_table[SLOT_CallObjectMethod] = (void *)j_CallObject;
    env_table[SLOT_CallObjectMethodV] = (void *)j_CallObject;
    env_table[SLOT_CallObjectMethodA] = (void *)j_CallObject;
    env_table[SLOT_CallBooleanMethod] = (void *)j_prim;
    env_table[SLOT_CallBooleanMethodV] = (void *)j_prim;
    env_table[SLOT_CallBooleanMethodA] = (void *)j_prim;
    env_table[SLOT_CallByteMethod] = (void *)j_prim;
    env_table[SLOT_CallByteMethodV] = (void *)j_prim;
    env_table[SLOT_CallByteMethodA] = (void *)j_prim;
    env_table[SLOT_CallCharMethod] = (void *)j_prim;
    env_table[SLOT_CallCharMethodV] = (void *)j_prim;
    env_table[SLOT_CallCharMethodA] = (void *)j_prim;
    env_table[SLOT_CallShortMethod] = (void *)j_prim;
    env_table[SLOT_CallShortMethodV] = (void *)j_prim;
    env_table[SLOT_CallShortMethodA] = (void *)j_prim;
    env_table[SLOT_CallIntMethod] = (void *)j_prim;
    env_table[SLOT_CallIntMethodV] = (void *)j_prim;
    env_table[SLOT_CallIntMethodA] = (void *)j_prim;
    env_table[SLOT_CallLongMethod] = (void *)j_prim;
    env_table[SLOT_CallLongMethodV] = (void *)j_prim;
    env_table[SLOT_CallLongMethodA] = (void *)j_prim;
    env_table[SLOT_CallFloatMethod] = (void *)j_float_call;
    env_table[SLOT_CallFloatMethodV] = (void *)j_float_call_v;
    env_table[SLOT_CallFloatMethodA] = (void *)j_float_call_v;
    env_table[SLOT_CallDoubleMethod] = (void *)j_prim;
    env_table[SLOT_CallDoubleMethodV] = (void *)j_prim;
    env_table[SLOT_CallDoubleMethodA] = (void *)j_prim;
    env_table[SLOT_CallVoidMethod] = (void *)j_prim;
    env_table[SLOT_CallVoidMethodV] = (void *)j_prim;
    env_table[SLOT_CallVoidMethodA] = (void *)j_prim;

    env_table[SLOT_CallNonvirtualObjectMethod] = (void *)j_CallObject;
    env_table[SLOT_CallNonvirtualObjectMethodV] = (void *)j_CallObject;
    env_table[SLOT_CallNonvirtualObjectMethodA] = (void *)j_CallObject;
    for (int s = SLOT_CallNonvirtualBooleanMethod; s <= SLOT_CallNonvirtualVoidMethodA; s++)
        env_table[s] = (void *)j_prim;
    env_table[SLOT_CallNonvirtualFloatMethod] = (void *)j_float_call;
    env_table[SLOT_CallNonvirtualFloatMethodV] = (void *)j_float_call_v;
    env_table[SLOT_CallNonvirtualFloatMethodA] = (void *)j_float_call_v;

    env_table[SLOT_GetFieldID] = (void *)j_GetFieldID;
    for (int s = SLOT_GetObjectField; s <= SLOT_GetDoubleField; s++)
        env_table[s] = (void *)j_zero;
    env_table[SLOT_GetIntField] = (void *)j_GetIntField;
    env_table[SLOT_GetFloatField] = (void *)j_GetFloatFieldF;
    for (int s = SLOT_SetObjectField; s <= SLOT_SetDoubleField; s++)
        env_table[s] = (void *)j_void;

    env_table[SLOT_GetStaticMethodID] = (void *)j_GetStaticMethodID;
    env_table[SLOT_CallStaticObjectMethod] = (void *)j_CallStaticObject;
    env_table[SLOT_CallStaticObjectMethodV] = (void *)j_CallStaticObject;
    env_table[SLOT_CallStaticObjectMethodA] = (void *)j_CallStaticObject;
    for (int s = SLOT_CallStaticBooleanMethod; s <= SLOT_CallStaticVoidMethodA; s++)
        env_table[s] = (void *)j_prim;
    env_table[SLOT_CallStaticFloatMethod] = (void *)j_float_call;
    env_table[SLOT_CallStaticFloatMethodV] = (void *)j_float_call_v;
    env_table[SLOT_CallStaticFloatMethodA] = (void *)j_float_call_v;
    env_table[SLOT_GetStaticFieldID] = (void *)j_GetStaticFieldID;
    for (int s = SLOT_GetStaticObjectField; s <= SLOT_GetStaticDoubleField; s++)
        env_table[s] = (void *)j_zero;
    for (int s = SLOT_SetStaticObjectField; s <= SLOT_SetStaticDoubleField; s++)
        env_table[s] = (void *)j_void;

    env_table[SLOT_NewStringUTF] = (void *)j_NewStringUTF;
    env_table[SLOT_GetStringUTFLength] = (void *)j_GetStringUTFLength;
    env_table[SLOT_GetStringUTFChars] = (void *)j_GetStringUTFChars;
    env_table[SLOT_ReleaseStringUTFChars] = (void *)j_void;
    env_table[SLOT_NewString] = (void *)j_NewString;
    env_table[SLOT_GetStringLength] = (void *)j_GetStringLength;
    env_table[SLOT_GetStringRegion] = (void *)j_void;
    env_table[SLOT_GetStringUTFRegion] = (void *)j_GetStringUTFRegion;

    env_table[SLOT_GetArrayLength] = (void *)j_GetArrayLength;
    env_table[SLOT_NewObjectArray] = (void *)j_NewObjectArray;
    env_table[SLOT_GetObjectArrayElement] = (void *)j_null;
    env_table[SLOT_SetObjectArrayElement] = (void *)j_void;
    for (int s = SLOT_NewBooleanArray; s <= SLOT_NewDoubleArray; s++)
        env_table[s] = (void *)j_NewArray;
    for (int s = SLOT_GetBooleanArrayElements; s <= SLOT_GetDoubleArrayElements; s++)
        env_table[s] = (void *)j_null;
    for (int s = SLOT_ReleaseBooleanArrayElements; s <= SLOT_ReleaseDoubleArrayElements; s++)
        env_table[s] = (void *)j_void;
    for (int s = SLOT_GetBooleanArrayRegion; s <= SLOT_SetDoubleArrayRegion; s++)
        env_table[s] = (void *)j_void;

    env_table[SLOT_RegisterNatives] = (void *)j_RegisterNatives;
    env_table[SLOT_UnregisterNatives] = (void *)j_zero;
    env_table[SLOT_MonitorEnter] = (void *)j_zero;
    env_table[SLOT_MonitorExit] = (void *)j_zero;
    env_table[SLOT_GetJavaVM] = (void *)j_GetJavaVM;
    env_table[SLOT_GetPrimitiveArrayCritical] = (void *)j_null;
    env_table[SLOT_ReleasePrimitiveArrayCritical] = (void *)j_void;
    env_table[SLOT_GetStringCritical] = (void *)j_null;
    env_table[SLOT_ReleaseStringCritical] = (void *)j_void;
    env_table[SLOT_NewWeakGlobalRef] = (void *)j_NewGlobalRef;
    env_table[SLOT_DeleteWeakGlobalRef] = (void *)j_DeleteRef;
    env_table[SLOT_ExceptionCheck] = (void *)j_ExceptionCheck;
    env_table[SLOT_NewDirectByteBuffer] = (void *)j_NewDirectByteBuffer;
    env_table[SLOT_GetDirectBufferAddress] = (void *)j_GetDirectBufferAddress;
    env_table[SLOT_GetDirectBufferCapacity] = (void *)j_GetDirectBufferCapacity;
    env_table[SLOT_GetObjectRefType] = (void *)j_zero;

    env_cell[0] = env_table;

    vm_table[0] = vm_table[1] = vm_table[2] = NULL;
    vm_table[3] = (void *)vm_DestroyJavaVM;
    vm_table[4] = (void *)vm_AttachCurrentThread;
    vm_table[5] = (void *)vm_DetachCurrentThread;
    vm_table[6] = (void *)vm_GetEnv;
    vm_table[7] = (void *)vm_AttachAsDaemon;
    vm_cell[0] = vm_table;

    pb_log("[jni] env=%p (table %d slots) vm=%p\n", (void *)env_cell, SLOT_COUNT, (void *)vm_cell);
}

void *pb_jni_env(void) { return env_cell; }
void *pb_jni_vm(void) { return vm_cell; }
uint64_t pb_jni_mint_id(void) { return mint(K_ID, "PairIP", "cached", "()Z"); }

uint64_t pb_jni_new_object(const char *cls)
{
    return mint(K_OBJ, cls, "obj", 0);
}

uint64_t pb_jni_handle_string(const char *s)
{
    uint64_t h = mint(K_STR, 0, 0, 0);
    jref *r = deref(h);
    if (r) r->str = strdup(s ? s : "");
    return h;
}
