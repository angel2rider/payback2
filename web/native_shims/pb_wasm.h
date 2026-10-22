/* pb_wasm.h — Payback 2 Web Port: WASM Shim Declarations.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define PB_MAX_IMPORTS 1024

typedef struct {
    const char *name;
    void       *addr;
    uint64_t    got_slots[8];
    int         n_slots;
    bool        is_data;
} pb_import;

typedef struct {
    uint8_t  *img;
    size_t    file_size;
    uint8_t  *base;
    uint64_t  span;
    uint64_t  vmin, vmax;
    void     *ehdr, *phdr, *shdr, *shstr;
    int       phnum, shnum;
    void     *symtab, *strtab;
    int       symcount;
    void     *init_array;
    int       init_count;
    void     *jni_onload;
} pb_image;

extern pb_image PB;

void pb_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void *pb_shim_lookup(const char *name, bool *is_data);
void  pb_shim_register(const char *name, void *addr, bool is_data);
void  pb_shim_report_missing(void);

void pb_jni_init(void);
void pb_shim_install_android(void);
void pb_shim_install_opensl(void);

// JavaScript Bridges (declared extern, implemented in main_wasm.c)
#ifdef __EMSCRIPTEN__
int js_asset_open(const char *path);
int js_asset_read(int handle, void *buf, int n);
int js_asset_length(int handle);
void js_asset_close(int handle);
void js_audio_enqueue_pcm(const int16_t *buf, int n_samples);
#endif
