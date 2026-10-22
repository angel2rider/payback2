/* elf_load.c — map the patched ELF natively, relocate, wire imports.
 *
 * Mirrors what the Linker does for a PIE .so: every PT_LOAD lands at
 * base+vaddr, every relocation is applied with real linker semantics
 * (RELATIVE / ABS64 / GLOB_DAT / JUMP_SLOT), undefined symbols resolve to
 * the shim registry, and the ~120 PairIP-stripped .got.plt slots are
 * restored from the live-GOT oracle table.
 */
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "pb.h"

pb_image PB;
int pb_verbose = 1;

/* ---- minimal ELF64 ------------------------------------------------ */
typedef struct {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Ehdr;

typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} Phdr;

typedef struct {
    uint32_t name, type;
    uint64_t flags, addr, offset, size;
    uint32_t link, info;
    uint64_t addralign, entsize;
} Shdr;

typedef struct { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; } Sym;

typedef struct { uint64_t offset, info; int64_t addend; } Rela;

enum {
    PT_LOAD = 1,
    SHT_DYNSYM = 11,
    SHT_INIT_ARRAY = 14,
    SHT_DYNAMIC = 6,
};

#define R_AARCH64_ABS64    257
#define R_AARCH64_GLOB_DAT 1025
#define R_AARCH64_JUMP_SLOT 1026
#define R_AARCH64_RELATIVE 1027

static const char *sname(Shdr *sh, int idx)
{
    if (idx < 0 || idx >= PB.shnum) return "";
    return (const char *)PB.shstr + sh[idx].name;
}

static void *map_image(uint64_t base, uint64_t span)
{
    void *p = mmap((void *)base, span, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) return NULL;
    return p;
}

int pb_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[-] cannot open %s: %s\n", path, strerror(errno)); return -1; }
    fseek(f, 0, SEEK_END);
    size_t sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sz);
    if (fread(buf, 1, sz, f) != sz) { fprintf(stderr, "[-] short read\n"); return -1; }
    fclose(f);

    PB.img = buf; PB.file_size = sz;
    Ehdr *eh = (Ehdr *)buf;
    if (eh->ident[0] != 0x7f || eh->ident[4] != 2) { fprintf(stderr, "[-] not ELF64\n"); return -1; }
    PB.ehdr = eh;
    Phdr *ph = (Phdr *)(buf + eh->phoff);
    PB.phdr = ph; PB.phnum = eh->phnum;
    Shdr *sh = (Shdr *)(buf + eh->shoff);
    PB.shdr = sh; PB.shnum = eh->shnum;
    PB.shstr = buf + sh[eh->shstrndx].offset;

    /* find span of PT_LOADs */
    uint64_t vmin = UINT64_MAX, vmax = 0;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_LOAD) continue;
        if (ph[i].vaddr < vmin) vmin = ph[i].vaddr;
        uint64_t end = ph[i].vaddr + ph[i].memsz;
        if (end > vmax) vmax = end;
    }
    PB.vmin = vmin; PB.vmax = vmax;
    uint64_t span = (vmax + 0xFFFF) & ~0xFFFFULL;

    /* pick a load base: try a few candidates */
    static const uint64_t cands[] = { 0x1000000000ULL, 0x2000000000ULL, 0x3000000000ULL,
                                      0x8000000000ULL, 0x100000000ULL };
    void *mapped = NULL;
    uint64_t base = 0;
    for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
        base = cands[i] - vmin;
        mapped = map_image(base, span + 0x10000);
        if (mapped) break;
    }
    if (!mapped) { fprintf(stderr, "[-] mmap of %#" PRIx64 " failed: %s\n", span, strerror(errno)); return -1; }
    PB.base = (uint8_t *)base;
    PB.span = span;
    if (mapped != (void *)base) { fprintf(stderr, "[-] mapped at unexpected %p (want %p)\n", mapped, (void *)base); return -1; }

    /* copy segments */
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_LOAD) continue;
        memcpy(PB.base + ph[i].vaddr, buf + ph[i].offset, ph[i].filesz);
        memset(PB.base + ph[i].vaddr + ph[i].filesz, 0, ph[i].memsz - ph[i].filesz);
    }

    /* section table access */
    for (int i = 0; i < PB.shnum; i++) {
        if (sh[i].type == SHT_DYNSYM) {
            PB.symtab = (void *)(buf + sh[i].offset);
            PB.symcount = (int)(sh[i].size / sizeof(Sym));
            PB.strtab = (void *)(buf + sh[sh[i].link].offset);
        }
    }
    if (!PB.symtab) { fprintf(stderr, "[-] no .dynsym\n"); return -1; }

    /* init_array is all zeros in the file — read AFTER relocations */
    for (int i = 0; i < PB.shnum; i++) {
        if (sh[i].type == SHT_INIT_ARRAY) {
            PB.init_array = PB.base + sh[i].addr;
            PB.init_count = (int)(sh[i].size / 8);
        }
    }

    /* exported JNI_OnLoad */
    Sym *sym = PB.symtab;
    for (int i = 0; i < PB.symcount; i++) {
        const char *nm = (const char *)PB.strtab + sym[i].name;
        if (sym[i].shndx && !strcmp(nm, "JNI_OnLoad"))
            PB.jni_onload = PB.base + sym[i].value;
    }

    char *dup = strdup(path); (void)dup;
    pb_patch_tp_reads();
    pb_log("[loader] %s: %zu bytes, PT_LOAD span %#" PRIx64 "..%#" PRIx64
           " -> base %p (%d ctors, JNI_OnLoad %p)\n",
           path, sz, vmin, vmax, PB.base, PB.init_count, PB.jni_onload);
    return 0;
}

static void wire_slot(uint64_t r_offset, const char *name, bool *is_data)
{
    void *tgt = pb_shim_lookup(name, is_data);
    if (!tgt && pb_verbose)
        fprintf(stderr, "[import] MISSING %s\n", name);
    *(uint64_t *)(PB.base + r_offset) = (uint64_t)tgt;
}

static const char *reloc_symname(uint32_t symidx)
{
    Sym *sym = PB.symtab;
    if (symidx == 0 || symidx >= (uint32_t)PB.symcount) return NULL;
    return (const char *)PB.strtab + sym[symidx].name;
}

void pb_relocate(void)
{
    Shdr *sh = PB.shdr;
    int applied = 0, unknown = 0;
    for (int i = 0; i < PB.shnum; i++) {
        if (sh[i].type != 4 /* SHT_RELA */) continue;
        const char *nm = sname(sh, i);
        if (strcmp(nm, ".rela.dyn") && strcmp(nm, ".rela.plt")) continue;
        Rela *rel = (Rela *)(PB.img + sh[i].offset);
        int n = (int)(sh[i].size / sizeof(Rela));
        for (int j = 0; j < n; j++) {
            uint32_t type = (uint32_t)(rel[j].info & 0xFFFFFFFF);
            uint32_t symidx = (uint32_t)(rel[j].info >> 32);
            uint64_t *slot = (uint64_t *)(PB.base + rel[j].offset);
            switch (type) {
            case R_AARCH64_RELATIVE:
                *slot = (uint64_t)PB.base + (uint64_t)rel[j].addend;
                break;
            case R_AARCH64_ABS64: {
                Sym *sym = PB.symtab;
                if (symidx && sym[symidx].shndx) {
                    *slot = (uint64_t)PB.base + sym[symidx].value + (uint64_t)rel[j].addend;
                } else {
                    const char *nm2 = reloc_symname(symidx);
                    bool data = false;
                    void *tgt = nm2 ? pb_shim_lookup(nm2, &data) : NULL;
                    if (!tgt) {
                        fprintf(stderr, "[reloc] ABS64 unresolved sym %s at %#" PRIx64 "\n",
                                nm2 ? nm2 : "(none)", rel[j].offset);
                        unknown++;
                    }
                    *slot = (uint64_t)tgt;
                }
                break;
            }
            case R_AARCH64_GLOB_DAT:
            case R_AARCH64_JUMP_SLOT: {
                Sym *sym = PB.symtab;
                if (symidx && sym[symidx].shndx) {
                    *slot = (uint64_t)PB.base + sym[symidx].value;
                } else {
                    const char *nm2 = reloc_symname(symidx);
                    bool data = false;
                    void *tgt = nm2 ? pb_shim_lookup(nm2, &data) : NULL;
                    if (!tgt) {
                        if (pb_verbose && nm2) fprintf(stderr, "[reloc] JUMP/GLOB unresolved sym %s\n", nm2);
                        unknown++;
                    }
                    *slot = (uint64_t)tgt;
                }
                break;
            }
            default:
                fprintf(stderr, "[reloc] unhandled type %u at %#" PRIx64 "\n",
                        type, rel[j].offset);
                break;
            }
            applied++;
        }
    }
    pb_log("[loader] applied %d relocations (%d unresolved)\n", applied, unknown);
}

void pb_wire_imports(void)
{
    /* oracle holes (PairIP-stripped .got.plt slots) */
#include "oracle_holes.h"
    int wired = 0, missing = 0;
    for (size_t i = 0; i < sizeof(oracle_holes) / sizeof(oracle_holes[0]); i++) {
        bool data = false;
        void *tgt = pb_shim_lookup(oracle_holes[i].sym, &data);
        if (!tgt) {
            if (pb_verbose) fprintf(stderr, "[holes] MISSING %s (slot %#x)\n",
                                    oracle_holes[i].sym, oracle_holes[i].off);
            missing++;
            continue;
        }
        *(uint64_t *)(PB.base + oracle_holes[i].off) = (uint64_t)tgt;
        wired++;
    }
    pb_log("[holes] wired %d live-oracle slots (%d missing)\n", wired, missing);
}

/* ---- TP-read takeover -------------------------------------------------
 * Measured on this machine: the kernel clobbers TPIDR_EL0 with junk
 * (0x2, 0x1004, 0x1005, ...) on every syscall return and preemption, and
 * x18 gets zeroed the same way.  A guest TCB therefore cannot live in any
 * register.  Every one of the guest's 519 TPIDR_EL0 reads is the stack
 * canary:  mrs xN, tpidr_el0 ; ldr xX, [xN, #0x28].
 * Rewrite the mrs into  adrp xN, <image page 0>  (same 4 bytes) and put a
 * constant Bionic-style canary at image offset 0x28 (inside the ELF header,
 * dead bytes once loading is done).  The canary read then always comes from
 * the mapped image and survives every kernel TP clobber.
 */
void pb_patch_tp_reads(void)
{
    uint64_t canary = (((uint64_t)arc4random() << 32) | arc4random()) & ~0xffULL;
    *(uint64_t *)(PB.base + 0x28) = canary;

    Shdr *sh = PB.shdr;
    int patched = 0;
    for (int i = 0; i < PB.shnum; i++) {
        if (sh[i].type != 1 /* PROGBITS */ || !(sh[i].flags & 0x4) /* EXECINSTR */)
            continue;
        uint32_t *w = (uint32_t *)(PB.base + sh[i].addr);
        size_t n = sh[i].size / 4;
        for (size_t j = 0; j < n; j++) {
            uint32_t insn = w[j];
            if ((insn & 0xFFFFFFE0u) != 0xD53BD040u) continue;   /* mrs xN, tpidr_el0 */
            uint32_t rd = insn & 0x1Fu;
            int64_t pages = -(int64_t)((sh[i].addr + j * 4) >> 12);
            uint32_t imm21 = (uint32_t)pages & 0x1FFFFFu;
            w[j] = 0x90000000u | ((imm21 & 3u) << 29) | (((imm21 >> 2) & 0x7FFFFu) << 5) | rd;
            patched++;
        }
        __builtin___clear_cache((char *)(PB.base + sh[i].addr),
                                (char *)(PB.base + sh[i].addr + sh[i].size));
    }
    pb_log("[patch] rewrote %d tpidr_el0 reads -> canary %#llx @ %p\n",
           patched, (unsigned long long)canary, PB.base + 0x28);
}

uint64_t pb_call(void *fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    uint64_t (*f)(uint64_t, uint64_t, uint64_t, uint64_t) = fn;
    return f(a0, a1, a2, a3);
}

uint64_t pb_call8(void *fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                  uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    uint64_t (*f)(uint64_t, uint64_t, uint64_t, uint64_t,
                  uint64_t, uint64_t, uint64_t, uint64_t) = fn;
    return f(a0, a1, a2, a3, a4, a5, a6, a7);
}

void *pb_sym(const char *name)
{
    Sym *sym = PB.symtab;
    for (int i = 0; i < PB.symcount; i++) {
        if (!sym[i].shndx) continue;
        if (!strcmp((const char *)PB.strtab + sym[i].name, name))
            return PB.base + sym[i].value;
    }
    return NULL;
}

void *pb_global(uint64_t off) { return PB.base + off; }

void pb_finish(void)
{
    /* mprotect segments to their final permissions */
    Phdr *ph = PB.phdr;
    for (int i = 0; i < PB.phnum; i++) {
        if (ph[i].type != PT_LOAD) continue;
        int prot = PROT_READ;
        if (ph[i].flags & 2) prot |= PROT_WRITE;
        if (ph[i].flags & 1) prot |= PROT_EXEC;
        uintptr_t lo = (uintptr_t)(PB.base + ph[i].vaddr) & ~0x3FFFULL;
        uintptr_t hi = ((uintptr_t)(PB.base + ph[i].vaddr + ph[i].memsz) + 0x3FFF) & ~0x3FFFULL;
        if (mprotect((void *)lo, hi - lo, prot) != 0)
            fprintf(stderr, "[loader] mprotect seg %d (%#x) failed: %s\n",
                    i, prot, strerror(errno));
    }

    /* ctors */
    pb_log("[init] running %d constructors\n", PB.init_count);
    void **ctors = PB.init_array;
    const char *watch = getenv("PB_DUMP_SLOT");
    unsigned long woff = watch ? strtoul(watch, NULL, 0) : 0;
    if (watch)
        pb_log("[init] watch slot %#lx = %#llx\n", woff,
               *(unsigned long long *)(PB.base + woff));
    for (int i = 0; i < PB.init_count; i++) {
        if (!ctors[i]) continue;
        pb_log("[init]   ctor[%d] %p\n", i, (void *)ctors[i]);
        pb_call((void *)ctors[i], 0, 0, 0, 0);
        pb_log("[init]   ctor[%d] ok\n", i);
        if (watch)
            pb_log("[init]   watched slot now %#llx\n",
                   *(unsigned long long *)(PB.base + woff));
    }
}
