// SPDX-License-Identifier: Apache-2.0

#include "guest/dlmain.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static void set_error(permute_guest_t *g, const char *what)
{
    const char *detail = dlerror();
    if (detail == NULL)
        detail = "no detail";
    snprintf(g->error, sizeof g->error, "%s: %s", what, detail);
}

int permute_guest_load(permute_guest_t *g, const char *path)
{
    if (g == NULL || path == NULL)
        return -1;

    // Copy the path before clearing, because the caller may pass g->path.
    char local[PERMUTE_GUEST_PATH_MAX];
    snprintf(local, sizeof local, "%s", path);

    memset(g, 0, sizeof *g);
    snprintf(g->path, sizeof g->path, "%s", local);

    // Clear any stale error before the first dl call.
    (void)dlerror();
    g->handle = dlopen(g->path, RTLD_NOW | RTLD_LOCAL);
    if (g->handle == NULL) {
        set_error(g, "dlopen");
        return -1;
    }

    (void)dlerror();
    void *symbol = dlsym(g->handle, "permute_main");
    if (symbol == NULL) {
        set_error(g, "dlsym(permute_main)");
        dlclose(g->handle);
        g->handle = NULL;
        return -1;
    }

    // Converting void* to a function pointer is not valid ISO C; copy the
    // representation instead of casting.
    _Static_assert(sizeof symbol == sizeof g->main_fn,
                   "function pointer and void pointer must have equal size");
    memcpy(&g->main_fn, &symbol, sizeof symbol);
    return 0;
}

int permute_guest_start(permute_guest_t *g, int argc, char **argv)
{
    if (g == NULL || g->handle == NULL || g->main_fn == NULL) {
        if (g != NULL)
            snprintf(g->error, sizeof g->error, "guest is not loaded");
        return -1;
    }
    return g->main_fn(argc, argv);
}

void permute_guest_unload(permute_guest_t *g)
{
    if (g == NULL)
        return;
    if (g->handle != NULL)
        dlclose(g->handle);
    g->handle = NULL;
    g->main_fn = NULL;
}
