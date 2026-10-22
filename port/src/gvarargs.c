/* gvarargs.c — guest-ABI variadic formatting.
 *
 * The asm stubs in gstubs.s snapshot x1..x7/q0..q7 + the caller's sp and call
 * the gv_* entries here.  Those interpret arguments with AAPCS64 rules (both
 * register-snapshot mode and true guest va_list mode for the v* family) and
 * drive a local formatter so we never mix ABIs.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pb.h"

/* ======================================================= reader plumbing */

static uint64_t rd_gp(gvar_reader *r)
{
    if (r->va_mode) {
        struct { void *stack; void *gr_top; void *vr_top; int gr_offs; int vr_offs; } *v =
            (void *)r->va;
        uint64_t val;
        if (v->gr_offs < 0) {
            val = *(uint64_t *)((char *)v->gr_top + v->gr_offs);
            v->gr_offs += 8;
        } else {
            val = *(uint64_t *)v->stack;
            v->stack = (char *)v->stack + 8;
        }
        return val;
    }
    if (r->gp_slot <= 7)
        return r->fr->gp[r->gp_slot++ - 1];
    return *r->stack++;
}

static void rd_fp(gvar_reader *r, uint64_t out[2])
{
    if (r->va_mode) {
        struct { void *stack; void *gr_top; void *vr_top; int gr_offs; int vr_offs; } *v =
            (void *)r->va;
        if (v->vr_offs < 0) {
            uint64_t *p = (uint64_t *)((char *)v->vr_top + v->vr_offs);
            out[0] = p[0]; out[1] = p[1];
            v->vr_offs += 16;
        } else {
            uintptr_t sp = (uintptr_t)v->stack;
            sp = (sp + 15) & ~(uintptr_t)15;
            uint64_t *p = (uint64_t *)sp;
            out[0] = p[0]; out[1] = p[1];
            v->stack = (char *)(sp + 16);
        }
        return;
    }
    if (r->fp_slot < 8) {
        out[0] = r->fr->fq[r->fp_slot * 2];
        out[1] = r->fr->fq[r->fp_slot * 2 + 1];
        r->fp_slot++;
    } else {
        uintptr_t sp = (uintptr_t)r->stack;
        sp = (sp + 15) & ~(uintptr_t)15;
        out[0] = ((uint64_t *)sp)[0];
        out[1] = ((uint64_t *)sp)[1];
        r->stack = (uint64_t *)(sp + 16);
    }
}

/* ============================================================ formatter */
typedef struct { char *p; size_t left; size_t total; } sink;

static void emit(sink *s, const char *b, size_t n)
{
    s->total += n;
    if (s->left > 1) {
        size_t k = n < s->left - 1 ? n : s->left - 1;
        memcpy(s->p, b, k);
        s->p += k;
        s->left -= k;
        *s->p = 0;
    }
}

static size_t guest_strnlen(const char *p, size_t cap)
{
    size_t n = 0;
    while (n < cap && p[n]) n++;
    return n;
}

/* format a float/double via the host (host<->host varargs are consistent) */
static int fmt_double(char *tmp, size_t tn, const char *subfmt, long double v, int wide)
{
    if (wide) return snprintf(tmp, tn, subfmt, v);
    return snprintf(tmp, tn, subfmt, (double)v);
}

int pb_vformat(char *out, size_t n, const char *fmt, gvar_reader *rd)
{
    sink s = { out, n ? n : 0, 0 };
    if (n) out[0] = 0;

    while (*fmt) {
        if (*fmt != '%') {
            const char *p = fmt;
            while (*fmt && *fmt != '%') fmt++;
            emit(&s, p, (size_t)(fmt - p));
            continue;
        }
        fmt++;                                  /* '%' */
        if (*fmt == '%') { emit(&s, "%", 1); fmt++; continue; }

        char spec[48]; int si = 0;
        spec[si++] = '%';
        /* flags */
        while (*fmt == '-' || *fmt == '0' || *fmt == '+' || *fmt == ' ' || *fmt == '#') {
            if (si < 40) spec[si++] = *fmt;
            fmt++;
        }
        /* width (incl. '*') */
        if (*fmt == '*') { int w = (int)rd_gp(rd); si += snprintf(spec + si, 12, "%d", w); fmt++; }
        else while (isdigit((unsigned char)*fmt)) { if (si < 40) spec[si++] = *fmt; fmt++; }
        /* precision */
        if (*fmt == '.') {
            spec[si++] = *fmt++;
            if (*fmt == '*') { int pr = (int)rd_gp(rd); si += snprintf(spec + si, 12, "%d", pr); fmt++; }
            else while (isdigit((unsigned char)*fmt)) { if (si < 40) spec[si++] = *fmt; fmt++; }
        }
        /* Length modifiers are consumed (never appended to spec): they only
           decide how the 64-bit guest value gets narrowed.  The conversion
           letter itself is appended at the very end, so spec must never be
           handed to the host snprintf() without it ("%ll" would print
           nothing on Darwin). */
        int wflag = 0;      /* 'l' — wide string for %ls */
        int hhflag = 0, hflag = 0, long64 = 0;
        if (*fmt == 'h' && fmt[1] == 'h') { hhflag = 1; fmt += 2; }
        else if (*fmt == 'h') { hflag = 1; fmt++; }
        else if (*fmt == 'l' && fmt[1] == 'l') { long64 = 1; fmt += 2; }
        else if (*fmt == 'l') { long64 = 1; wflag = 1; fmt++; }
        else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j') { long64 = 1; fmt++; }
        else if (*fmt == 'L') { spec[si++] = 'L'; fmt++; }
        char conv = *fmt ? *fmt++ : '%';

        char tmp[512];
        switch (conv) {
        case 'd': case 'i': {
            long long v = (long long)rd_gp(rd);
            if (hhflag) v = (signed char)v;
            else if (hflag) v = (short)v;
            else if (!long64) v = (int)v;
            spec[si++] = 'l'; spec[si++] = 'l'; spec[si++] = conv; spec[si] = 0;
            int k = snprintf(tmp, sizeof tmp, spec, v);
            emit(&s, tmp, k > 0 ? (size_t)k : 0);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v = rd_gp(rd);
            if (hhflag) v = (unsigned char)v;
            else if (hflag) v = (unsigned short)v;
            else if (!long64) v = (unsigned int)v;
            spec[si++] = 'l'; spec[si++] = 'l'; spec[si++] = conv; spec[si] = 0;
            int k = snprintf(tmp, sizeof tmp, spec, v);
            emit(&s, tmp, k > 0 ? (size_t)k : 0);
            break;
        }
        case 'p': {
            uint64_t v = rd_gp(rd);
            int k = snprintf(tmp, sizeof tmp, "0x%llx", (unsigned long long)v);
            emit(&s, tmp, k > 0 ? (size_t)k : 0);
            break;
        }
        case 'c': {
            char c = (char)rd_gp(rd);
            emit(&s, &c, 1);
            break;
        }
        case 's': {
            if (wflag) {   /* %ls — BMP to UTF-8 */
                const uint32_t *w = (const uint32_t *)(uintptr_t)rd_gp(rd);
                char ub[8];
                while (*w) {
                    uint32_t cp = *w++;
                    int k = 0;
                    if (cp < 0x80) ub[k++] = (char)cp;
                    else if (cp < 0x800) { ub[k++] = 0xC0 | (cp >> 6); ub[k++] = 0x80 | (cp & 0x3F); }
                    else { ub[k++] = 0xE0 | (cp >> 12); ub[k++] = 0x80 | ((cp >> 6) & 0x3F); ub[k++] = 0x80 | (cp & 0x3F); }
                    emit(&s, ub, k);
                }
            } else {
                const char *p = (const char *)(uintptr_t)rd_gp(rd);
                size_t len = p ? guest_strnlen(p, 1 << 20) : 0;
                if (!p) { emit(&s, "(null)", 6); break; }
                const char *pp = strchr(spec, '.');
                if (pp) { int pr = atoi(pp + 1); if (pr >= 0 && (size_t)pr < len) len = (size_t)pr; }
                emit(&s, p, len);
            }
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            uint64_t q[2];
            rd_fp(rd, q);
            int wide = strchr(spec, 'L') != NULL;
            int k;
            spec[si++] = conv; spec[si] = 0;
            if (wide) {
                /* Apple arm64 long double == double; guest quad read as low double */
                double v; memcpy(&v, q, 8);
                k = fmt_double(tmp, sizeof tmp, spec, v, 1);
            } else {
                double v; memcpy(&v, q, 8);
                k = fmt_double(tmp, sizeof tmp, spec, v, 0);
            }
            emit(&s, tmp, k > 0 ? (size_t)k : 0);
            break;
        }
        case 'n': {
            int *p = (int *)(uintptr_t)rd_gp(rd);
            if (p) *p = (int)s.total;
            break;
        }
        default:
            emit(&s, "%", 1);
            emit(&s, &conv, 1);
            break;
        }
    }
    return (int)s.total;
}

/* ================================================= helper entry points */

int gv_printf(const char *fmt, const gvar_frame *fr, uint64_t *stack)
{
    gvar_reader rd = { .gp_slot = 1, .fr = fr, .stack = stack };
    char buf[4096];
    int k = pb_vformat(buf, sizeof buf, fmt, &rd);
    ssize_t w = write(1, buf, k > 4095 ? 4095 : k);
    (void)w;
    return k;
}

int gv_fprintf(FILE *f, const char *fmt, const gvar_frame *fr, uint64_t *stack)
{
    FILE *hf = pb_xfile(f);
    if (!hf) hf = stderr;
    gvar_reader rd = { .gp_slot = 2, .fr = fr, .stack = stack };
    char buf[8192];
    int k = pb_vformat(buf, sizeof buf, fmt, &rd);
    size_t wn = k > 8191 ? 8191 : (size_t)k;
    size_t w = fwrite(buf, 1, wn, hf);
    (void)w;
    if (wn && buf[wn - 1] == '\n') fflush(hf);
    return k;
}

int gv_sprintf(char *out, const char *fmt, const gvar_frame *fr, uint64_t *stack)
{
    gvar_reader rd = { .gp_slot = 2, .fr = fr, .stack = stack };
    return pb_vformat(out, (size_t)-1 / 2, fmt, &rd);
}

int gv_snprintf(char *out, size_t n, const char *fmt, const gvar_frame *fr, uint64_t *stack)
{
    gvar_reader rd = { .gp_slot = 3, .fr = fr, .stack = stack };
    return pb_vformat(out, n, fmt, &rd);
}

int gv_fscanf(FILE *f, const char *fmt, const gvar_frame *fr, uint64_t *stack)
{
    (void)f; (void)fmt; (void)fr; (void)stack;
    return 0;   /* no scan conversions implemented yet */
}

int gv_sscanf(const char *s, const char *fmt, const gvar_frame *fr, uint64_t *stack)
{
    (void)s; (void)fmt; (void)fr; (void)stack;
    return 0;
}

int gv_swprintf(uint32_t *out, size_t n, const uint32_t *fmt, const gvar_frame *fr, uint64_t *stack)
{
    /* widen: run the narrow formatter, then convert UTF-8 -> wide (BMP). */
    char nf[1024], nb[4096];
    size_t i = 0;
    while (fmt[i] && i < sizeof nf - 1) { nf[i] = (char)fmt[i]; i++; }
    nf[i] = 0;
    gvar_reader rd = { .gp_slot = 3, .fr = fr, .stack = stack };
    int k = pb_vformat(nb, sizeof nb, nf, &rd);
    size_t w = 0;
    int ret = k;
    for (int j = 0; j < k && (size_t)j < sizeof nb; ) {
        unsigned char c = (unsigned char)nb[j];
        uint32_t cp; int adv = 1;
        if (c < 0x80) cp = c;
        else if ((c & 0xE0) == 0xC0 && j + 1 < k) { cp = ((c & 0x1F) << 6) | (nb[j+1] & 0x3F); adv = 2; }
        else if ((c & 0xF0) == 0xE0 && j + 2 < k) { cp = ((c & 0x0F) << 12) | ((nb[j+1] & 0x3F) << 6) | (nb[j+2] & 0x3F); adv = 3; }
        else cp = '?';
        j += adv;
        if (w + 1 < n) out[w] = cp;
        w++;
        if (w == n) break;
    }
    if (n) out[w < n ? w : n - 1] = 0;
    return ret;
}

void gv_syslog(int prio, const char *fmt, const gvar_frame *fr, uint64_t *stack)
{
    (void)prio;
    gvar_reader rd = { .gp_slot = 2, .fr = fr, .stack = stack };
    char buf[4096];
    int k = pb_vformat(buf, sizeof buf, fmt, &rd);
    fprintf(stderr, "[syslog] %.*s\n", k > 4000 ? 4000 : k, buf);
}

int gv_android_log_print(int prio, const char *tag, const char *fmt,
                         const gvar_frame *fr, uint64_t *stack)
{
    (void)prio;
    gvar_reader rd = { .gp_slot = 3, .fr = fr, .stack = stack };
    char buf[4096];
    int k = pb_vformat(buf, sizeof buf, fmt, &rd);
    int kn = k > 4000 ? 4000 : k;
    fprintf(stderr, "[%s] %.*s\n", tag ? tag : "?", kn, buf);
    return k;
}

void gv_android_log_assert(const char *cond, const char *tag, const char *fmt,
                           const gvar_frame *fr, uint64_t *stack)
{
    gvar_reader rd = { .gp_slot = 3, .fr = fr, .stack = stack };
    char buf[4096];
    int k = pb_vformat(buf, sizeof buf, fmt, &rd);
    fprintf(stderr, "[ASSERT %s][%s] %.*s (cond: %s)\n", tag ? tag : "?", tag ? tag : "?",
            k > 3000 ? 3000 : k, buf, cond ? cond : "?");
}

/* ============================================ non-variadic v* family */

int pbg_vsnprintf(char *out, size_t n, const char *fmt, va_list ap)
{
    gvar_reader rd = { .gp_slot = 1, .va_mode = 1, .va = (const uint8_t *)ap };
    return pb_vformat(out, n, fmt, &rd);
}

int pbg_vfprintf(FILE *f, const char *fmt, va_list ap)
{
    FILE *hf = pb_xfile(f);
    if (!hf) hf = stderr;
    gvar_reader rd = { .gp_slot = 2, .va_mode = 1, .va = (const uint8_t *)ap };
    char buf[8192];
    int k = pb_vformat(buf, sizeof buf, fmt, &rd);
    fwrite(buf, 1, k > 8191 ? 8191 : (size_t)k, hf);
    return k;
}

int pbg_vprintf(const char *fmt, va_list ap)
{
    gvar_reader rd = { .gp_slot = 1, .va_mode = 1, .va = (const uint8_t *)ap };
    char buf[4096];
    int k = pb_vformat(buf, sizeof buf, fmt, &rd);
    ssize_t w = write(1, buf, k > 4095 ? 4095 : k);
    (void)w;
    return k;
}

int pbg_vasprintf(char **strp, const char *fmt, va_list ap)
{
    gvar_reader rd = { .gp_slot = 2, .va_mode = 1, .va = (const uint8_t *)ap };
    size_t cap = 1024;
    char *buf = malloc(cap);
    int k;
    for (;;) {
        k = pb_vformat(buf, cap, fmt, &rd);
        if ((size_t)k < cap - 1) break;
        cap *= 2;
        if (cap > (1u << 24)) break;
        buf = realloc(buf, cap);
    }
    *strp = buf;
    return k;
}

/* the guest's long-double based helpers are not used so far; keep symbols
   for anything libstdc++-style that may reference them. */

/* ============================================ registration */

void pb_gvar_register(void)
{
    /* The asm stubs and the guest-ABI v* entries.  Their C names are prefixed
       so this binary itself keeps the host stdio; the registry binds the
       IMPORT names onto them. */
    extern char pb_stub_printf[], pb_stub_fprintf[], pb_stub_sprintf[],
                pb_stub_snprintf[], pb_stub_fscanf[], pb_stub_sscanf[],
                pb_stub_swprintf[], pb_stub_syslog[],
                pb_stub_android_log_print[], pb_stub_android_log_assert[];
    extern int pbg_vsnprintf(char *, size_t, const char *, va_list);
    extern int pbg_vfprintf(FILE *, const char *, va_list);
    extern int pbg_vprintf(const char *, va_list);
    extern int pbg_vasprintf(char **, const char *, va_list);

    pb_shim_register("printf", (void *)pb_stub_printf, false);
    pb_shim_register("fprintf", (void *)pb_stub_fprintf, false);
    pb_shim_register("sprintf", (void *)pb_stub_sprintf, false);
    pb_shim_register("snprintf", (void *)pb_stub_snprintf, false);
    pb_shim_register("fscanf", (void *)pb_stub_fscanf, false);
    pb_shim_register("sscanf", (void *)pb_stub_sscanf, false);
    pb_shim_register("swprintf", (void *)pb_stub_swprintf, false);
    pb_shim_register("syslog", (void *)pb_stub_syslog, false);
    pb_shim_register("__android_log_print", (void *)pb_stub_android_log_print, false);
    pb_shim_register("__android_log_assert", (void *)pb_stub_android_log_assert, false);

    pb_shim_register("vsnprintf", (void *)pbg_vsnprintf, false);
    pb_shim_register("vfprintf", (void *)pbg_vfprintf, false);
    pb_shim_register("vprintf", (void *)pbg_vprintf, false);
    pb_shim_register("vasprintf", (void *)pbg_vasprintf, false);
}
