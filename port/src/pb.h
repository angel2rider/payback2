/* pb.h — Payback 2 native port: common declarations.
 *
 * The port rehouses the user's own libpayback2.patched.so (arm64) natively:
 * map PT_LOADs at a fixed base, apply relocations, resolve the ~268 imports
 * against (a) macOS libc via dlsym or (b) hand-written Bionic/Android shims,
 * then run the Android boot sequence (ctors -> JNI_OnLoad ->
 * initializeNativeCode -> lifecycle natives).
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

#define PB_MAX_IMPORTS 1024

typedef struct {
    const char *name;       /* symbol name                        */
    void       *addr;       /* resolved address (code or data)    */
    uint64_t    got_slots[8]; /* GOT slots wired to this import   */
    int         n_slots;
    bool        is_data;
} pb_import;

typedef struct {
    uint8_t  *img;          /* file bytes of the ELF              */
    size_t    file_size;
    uint8_t  *base;         /* mapped image base                  */
    uint64_t  span;         /* mapped size                        */
    uint64_t  vmin, vmax;
    /* ELF tables (file pointers) */
    void     *ehdr, *phdr, *shdr, *shstr;
    int       phnum, shnum;
    /* symbol table */
    void     *symtab, *strtab;
    int       symcount;
    /* init */
    void     *init_array;   /* points into mapped image (post-reloc) */
    int       init_count;
    void     *jni_onload;   /* resolved address                   */
} pb_image;

extern pb_image PB;

/* ---- logging ---------------------------------------------------------- */
void pb_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int pb_verbose;

/* ---- shim registry (shims.c) ------------------------------------------ */
void  *pb_shim_lookup(const char *name, bool *is_data);
void   pb_shim_register(const char *name, void *addr, bool is_data);
void   pb_shim_report_missing(void);

/* ---- guest varargs / guest-ABI printf family (gvarargs.c) ------------- */
typedef struct {            /* exactly the frame the asm stubs build     */
    uint64_t lr;            /* [0x00]                                   */
    uint64_t fp;            /* [0x08]                                   */
    uint64_t gp[7];         /* [0x10] x1..x7                            */
    uint64_t pad;           /* [0x48]                                   */
    uint64_t fq[16];        /* [0x50] q0..q7                            */
} gvar_frame;

typedef struct {
    /* AAPCS64 GP/FP slot cursor (slots 0..7 are x0..x7 / q0..q7) */
    int      gp_slot;       /* next GP slot number to consume           */
    int      fp_slot;
    const gvar_frame *fr;   /* register snapshot (may be NULL)          */
    uint64_t *stack;        /* pointer to first stack argument          */
    /* AAPCS64 va_list mode (for vsnprintf-family)                       */
    int      va_mode;       /* 1 = fields below valid                   */
    const uint8_t *va;      /* guest AAPCS64 va_list struct             */
} gvar_reader;

void pb_gvar_register(void);        /* wires the stub symbols           */
void pb_pthread_register(void);     /* Bionic pthread shims + guest TLS */
void pb_tls_check(const char *label);
void pb_patch_tp_reads(void);       /* mrs tpidr_el0 -> adrp, canary@0x28 */
void pb_shim_install_android(void); /* NDK surface: ALooper/AAsset/EGL/GL */
void pb_shim_install_egl(void);     /* real EGL/GLES2 via ANGLE (Metal);
                                     * call BEFORE install_android, first-wins;
                                     * falls back silently to the zero stubs  */
void pb_shim_install_opensl(void);  /* minimal OpenSL ES object model (silent)  */
void pb_opensl_fixup_slots(void);   /* force guest SL_IID_* GOT slots after wire */
void pb_jni_lang_fixup(void);       /* seed the glyph language-row index         */

/* ---- window + presentation (window.m) ---------------------------------- */
int  pb_win_w(void);                /* engine buffer width  (default 1920)   */
int  pb_win_h(void);                /* engine buffer height (default 1080)   */
int  pb_window_ready(void);
void pb_window_create(int w, int h, const char *title);
void pb_window_publish(uint8_t *rgba_topdown, int w, int h); /* takes ownership */
void pb_window_run(void);           /* NSApp run; quits on close / PB_HOLD   */

/* ---- FILE* translation (stdio.c) -------------------------------------- */
void  pb_stdio_init(void);          /* builds __sF tokens               */
FILE *pb_xfile(void *guest_file);   /* token -> host FILE*              */

/* ---- JNI (jni_env.c) ---------------------------------------------------- */
void  pb_jni_init(void);
void  *pb_jni_env(void);            /* JNIEnv* to hand to the engine     */
void  *pb_jni_vm(void);             /* JavaVM*                           */
uint64_t pb_jni_mint_id(void);      /* a dummy jmethodID                 */
uint64_t pb_cur_guest_thread(void); /* guest pthread handle of caller (0 = main) */
void    *pb_jni_native(const char *name);   /* fn registered via RegisterNatives */
void     pb_jni_set_motion(int action, float x, float y, long long ms, long long down_ms);
void     pb_jni_set_key(int code, int unicode, int repeat, int flags);

/* ---- input (main.c) ----------------------------------------------------- */
void pb_input_attach(uint64_t game);          /* after initializeNativeCode */
void pb_input_motion(int action, float x, float y);
void pb_input_key(int action, int code);

/* ---- loader (elf_load.c) ----------------------------------------------- */
int   pb_load(const char *path);
void  pb_relocate(void);
void  pb_wire_imports(void);
void  pb_finish(void);              /* ctors + JNI_OnLoad                */
void *pb_sym(const char *name);     /* exported symbol address           */
void *pb_global(uint64_t off);      /* image base + off                  */

/* calls a guest function pointer with up to 8 integer args (non-variadic) */
uint64_t pb_call(void *fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t pb_call8(void *fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                  uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
void pb_stdio_register(void);
uint64_t pb_jni_handle_string(const char *s);
uint64_t pb_jni_new_object(const char *cls);
void pb_backtrace(const char *tag);
