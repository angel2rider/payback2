/* gstubs.s — guest-ABI entry trampolines for the variadic imports.
 *
 * Apple arm64 passes variadic arguments on the stack; AAPCS64 (the guest's
 * ABI) passes them in x1..x7 / q0..q7 first.  Each stub snapshots the guest
 * register file + the caller's stack pointer and hands them to a C helper
 * that interprets the arguments with AAPCS64 rules.
 *
 * The symbols are deliberately NOT named printf/fprintf/... : the port binary
 * itself must keep using the host stdio.  The shim registry maps the import
 * names onto these addresses.
 *
 * Frame (0xD0) layout matches struct gvar_frame:
 *   0x00 lr   0x08 fp   0x10..0x48 x1..x7   0x50..0xCF q0..q7
 */
.text

.macro GVAR_STUB name, helper, nargs
.globl \name
.p2align 2
\name:
    stp x29, x30, [sp, #-0xD0]!
    mov x29, sp
    stp x1, x2, [sp, #0x10]
    stp x3, x4, [sp, #0x20]
    stp x5, x6, [sp, #0x30]
    str x7, [sp, #0x40]
    stp q0, q1, [sp, #0x50]
    stp q2, q3, [sp, #0x70]
    stp q4, q5, [sp, #0x90]
    stp q6, q7, [sp, #0xB0]
.if \nargs == 1
    mov x1, sp
    add x2, sp, #0xD0
.elseif \nargs == 2
    add x2, sp, #0
    add x3, sp, #0xD0
.else
    add x3, sp, #0
    add x4, sp, #0xD0
.endif
    bl \helper
    ldp x29, x30, [sp], #0xD0
    ret
.endm

/* --- int printf(const char*, ...) */
GVAR_STUB _pb_stub_printf, _gv_printf, 1

/* --- int fprintf(FILE*, const char*, ...) */
GVAR_STUB _pb_stub_fprintf, _gv_fprintf, 2

/* --- int sprintf(char*, const char*, ...) */
GVAR_STUB _pb_stub_sprintf, _gv_sprintf, 2

/* --- int snprintf(char*, size_t, const char*, ...) */
GVAR_STUB _pb_stub_snprintf, _gv_snprintf, 3

/* --- int fscanf(FILE*, const char*, ...) */
GVAR_STUB _pb_stub_fscanf, _gv_fscanf, 2

/* --- int sscanf(const char*, const char*, ...) */
GVAR_STUB _pb_stub_sscanf, _gv_sscanf, 2

/* --- int swprintf(wchar_t*, size_t, const wchar_t*, ...) */
GVAR_STUB _pb_stub_swprintf, _gv_swprintf, 3

/* --- void syslog(int, const char*, ...) */
GVAR_STUB _pb_stub_syslog, _gv_syslog, 2

/* --- int __android_log_print(int, const char*, const char*, ...) */
GVAR_STUB _pb_stub_android_log_print, _gv_android_log_print, 3

/* --- void __android_log_assert(const char*, const char*, const char*, ...) */
GVAR_STUB _pb_stub_android_log_assert, _gv_android_log_assert, 3
