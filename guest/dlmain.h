// SPDX-License-Identifier: Apache-2.0
//
// Guest process loading.
//
// One shared object per process instance, loaded with RTLD_NOW | RTLD_LOCAL
// (ADR-005). RTLD_LOCAL keeps each instance's symbols out of the global
// namespace, and compiling one object per instance is what gives each process
// genuine .data/.bss. `dlmopen(LM_ID_NEWLM)` was considered and rejected in
// the requirements: glibc's per-namespace libc copy is fragile, the namespace
// count is capped, and a bug there would present as simulator nondeterminism.
//
// Load and start are separate calls on purpose. Constructors run during
// `permute_guest_load`, so the executor controls order by calling load in
// EV_SPAWN order; `permute_guest_start` runs permute_main at the event itself.

#ifndef PERMUTE_DLMAIN_H
#define PERMUTE_DLMAIN_H

#include <stddef.h>
#include <stdint.h>

#define PERMUTE_GUEST_PATH_MAX 512
#define PERMUTE_GUEST_ERROR_MAX 512

typedef int (*permute_main_fn)(int argc, char **argv);

typedef struct {
    void *handle;
    permute_main_fn main_fn;
    char path[PERMUTE_GUEST_PATH_MAX];
    char error[PERMUTE_GUEST_ERROR_MAX];
} permute_guest_t;

// Loads the object, which runs its constructors. Returns 0 on success, -1 on
// failure with `error` set.
int permute_guest_load(permute_guest_t *g, const char *path);

// Bind the object to the already-mapped hypercall page. Each module has its
// own statics, so a guest that never binds would see a null page and fail
// every call with ENOSYS.
int permute_guest_bind(permute_guest_t *g, uintptr_t base);

// Calls permute_main. Returns its return value, or -1 with `error` set.
int permute_guest_start(permute_guest_t *g, int argc, char **argv);

void permute_guest_unload(permute_guest_t *g);

#endif // PERMUTE_DLMAIN_H
