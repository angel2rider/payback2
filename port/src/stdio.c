/* stdio.c — Bionic `__sF` token <-> host FILE* translation.
 *
 * The engine takes `&__sF[2]` (Bionic's stderr slot) and passes that pointer
 * through the whole stdio family.  We fabricate the __sF array and translate
 * its three tokens to the host's stdin/stdout/stderr; real host FILE* values
 * returned by fopen() pass through unchanged.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pb.h"

#define BIONIC_FILE_SIZE 0x98

static unsigned char sF_blob[BIONIC_FILE_SIZE * 3] __attribute__((aligned(16)));

void pb_stdio_init(void)
{
    pb_shim_register("__sF", sF_blob, true);
}

FILE *pb_xfile(void *gf)
{
    if (!gf) return NULL;
    if (gf == (void *)&sF_blob[0 * BIONIC_FILE_SIZE]) return stdin;
    if (gf == (void *)&sF_blob[1 * BIONIC_FILE_SIZE]) return stdout;
    if (gf == (void *)&sF_blob[2 * BIONIC_FILE_SIZE]) return stderr;
    return (FILE *)gf;
}

/* --- stdio shims that must translate the FILE* argument ------------- */

/* --- path tracing (PB_TRACE_FILE=1) --------------------------------- */
static int pb_trace_file(void) { static int on = -1; if (on < 0) on = getenv("PB_TRACE_FILE") != NULL; return on; }

/* Guest return address (inside libpayback2) — lets PB_TRACE_FILE show which
 * engine call site does the I/O.  The guest reaches us through its PLT, which
 * is a plain branch, so the link register still holds the engine address. */
static unsigned long pb_guest_ra(void)
{
    return (unsigned long)__builtin_return_address(0);
}

/* Walk the engine's frame-pointer chain.  We are tail-called from the guest's
 * PLT, so our own frame record holds the guest's x29 in [fp]. */
static void pb_trace_stack(const char *what)
{
    unsigned long sp = (unsigned long)__builtin_frame_address(0);
    unsigned long gfp = *(unsigned long *)sp;
    for (int i = 0; i < 8; i++) {
        if (!gfp || (gfp & 7) || gfp < sp || gfp > sp + 0x40000) break;
        unsigned long prev = *(unsigned long *)gfp;
        unsigned long lr = *(unsigned long *)(gfp + 8);
        fprintf(stderr, "[file] %s stack[%d] lr=%#lx\n", what, i, lr);
        if (prev <= gfp) break;
        gfp = prev;
    }
}

static FILE *sh_fopen(const char *path, const char *mode)
{
    FILE *f = fopen(path, mode ? mode : "r");
    if (pb_trace_file())
        fprintf(stderr, "[file] fopen(\"%s\", \"%s\") -> %p ra=%#lx\n",
                path ? path : "(null)", mode ? mode : "(null)", (void *)f,
                pb_guest_ra());
    if (pb_trace_file() && getenv("PB_TRACE_STACK"))
        pb_trace_stack(path);
    return f;
}

static int sh_remove(const char *path)
{
    int r = remove(path);
    if (pb_trace_file())
        fprintf(stderr, "[file] remove(\"%s\") -> %d ra=%#lx\n", path ? path : "(null)", r,
                pb_guest_ra());
    if (pb_trace_file() && getenv("PB_TRACE_STACK"))
        pb_trace_stack(path);
    return r;
}

static int sh_rename(const char *a, const char *b)
{
    int r = rename(a, b);
    if (pb_trace_file())
        fprintf(stderr, "[file] rename(\"%s\", \"%s\") -> %d\n",
                a ? a : "(null)", b ? b : "(null)", r);
    return r;
}

static int sh_fclose(void *f) { return fclose(pb_xfile(f)); }
static int sh_fflush(void *f) { return fflush(pb_xfile(f)); }
static int sh_ferror(void *f) { FILE *h = pb_xfile(f); return h ? ferror(h) : 1; }
static int sh_feof(void *f)  { FILE *h = pb_xfile(f); return h ? feof(h) : 1; }
static int sh_fileno(void *f) { FILE *h = pb_xfile(f); return h ? fileno(h) : -1; }
static int sh_fseek(void *f, long off, int whence) { FILE *h = pb_xfile(f); return h ? fseek(h, off, whence) : -1; }
static long sh_ftell(void *f) { FILE *h = pb_xfile(f); return h ? ftell(h) : -1; }
static int sh_fseeko(void *f, long long off, int whence) { FILE *h = pb_xfile(f); return h ? fseeko(h, (off_t)off, whence) : -1; }
static long long sh_ftello(void *f) { FILE *h = pb_xfile(f); return h ? (long long)ftello(h) : -1; }
static size_t sh_fread(void *p, size_t sz, size_t n, void *f) { FILE *h = pb_xfile(f); return h ? fread(p, sz, n, h) : 0; }
static size_t sh_fwrite(const void *p, size_t sz, size_t n, void *f) { FILE *h = pb_xfile(f); return h ? fwrite(p, sz, n, h) : 0; }
static char *sh_fgets(char *b, int n, void *f) { FILE *h = pb_xfile(f); return h ? fgets(b, n, h) : NULL; }
static int sh_fputs(const char *s, void *f) { FILE *h = pb_xfile(f); return h ? fputs(s, h) : -1; }
static int sh_fputc(int c, void *f) { FILE *h = pb_xfile(f); return h ? fputc(c, h) : -1; }
static int sh_fgetc(void *f) { FILE *h = pb_xfile(f); return h ? fgetc(h) : -1; }
static int sh_ungetc(int c, void *f) { FILE *h = pb_xfile(f); return h ? ungetc(c, h) : -1; }
static int sh_setvbuf(void *f, char *buf, int mode, size_t sz) { FILE *h = pb_xfile(f); return h ? setvbuf(h, buf, mode, sz) : -1; }
static int sh_putc(int c, void *f) { FILE *h = pb_xfile(f); return h ? putc(c, h) : -1; }
static int sh_getc(void *f) { FILE *h = pb_xfile(f); return h ? getc(h) : -1; }

void pb_stdio_register(void)
{
    pb_shim_register("fclose", (void *)sh_fclose, false);
    pb_shim_register("fopen", (void *)sh_fopen, false);
    pb_shim_register("remove", (void *)sh_remove, false);
    pb_shim_register("rename", (void *)sh_rename, false);
    pb_shim_register("fflush", (void *)sh_fflush, false);
    pb_shim_register("ferror", (void *)sh_ferror, false);
    pb_shim_register("feof", (void *)sh_feof, false);
    pb_shim_register("fileno", (void *)sh_fileno, false);
    pb_shim_register("fseek", (void *)sh_fseek, false);
    pb_shim_register("ftell", (void *)sh_ftell, false);
    pb_shim_register("fseeko", (void *)sh_fseeko, false);
    pb_shim_register("ftello", (void *)sh_ftello, false);
    pb_shim_register("fread", (void *)sh_fread, false);
    pb_shim_register("fwrite", (void *)sh_fwrite, false);
    pb_shim_register("fgets", (void *)sh_fgets, false);
    pb_shim_register("fputs", (void *)sh_fputs, false);
    pb_shim_register("fputc", (void *)sh_fputc, false);
    pb_shim_register("fgetc", (void *)sh_fgetc, false);
    pb_shim_register("ungetc", (void *)sh_ungetc, false);
    pb_shim_register("setvbuf", (void *)sh_setvbuf, false);
    pb_shim_register("putc", (void *)sh_putc, false);
    pb_shim_register("getc", (void *)sh_getc, false);
}
