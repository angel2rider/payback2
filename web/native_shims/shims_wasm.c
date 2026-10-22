/* shims_wasm.c — Payback 2 Web Port: Symbol Registry for WASM.
 */
#include "pb_wasm.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

void pb_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static pb_import imports[PB_MAX_IMPORTS];
static int n_imports = 0;

void pb_shim_register(const char *name, void *addr, bool is_data)
{
    for (int i = 0; i < n_imports; i++) {
        if (!strcmp(imports[i].name, name)) {
            imports[i].addr = addr;
            imports[i].is_data = is_data;
            return;
        }
    }
    if (n_imports >= PB_MAX_IMPORTS) {
        fprintf(stderr, "[shim] Import table full!\n");
        return;
    }
    imports[n_imports].name = name;
    imports[n_imports].addr = addr;
    imports[n_imports].is_data = is_data;
    imports[n_imports].n_slots = 0;
    n_imports++;
}

void *pb_shim_lookup(const char *name, bool *is_data)
{
    for (int i = 0; i < n_imports; i++) {
        if (!strcmp(imports[i].name, name)) {
            if (is_data) *is_data = imports[i].is_data;
            return imports[i].addr;
        }
    }
    return NULL;
}

void pb_shim_report_missing(void)
{
}
