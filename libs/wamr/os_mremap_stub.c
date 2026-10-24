// WAMR calls os_mremap when it remaps a shared wasm memory, which is a POSIX
// primitive the Windows platform port of WAMR never defines. The .ast decoder
// loads its own module with no shared memory, so this is unreachable for us -
// but the interpreter still references it at link time.
//
// Returning 0 mimics a failed remap, which WAMR already handles.

#include <stddef.h>

int os_mremap(void* addr, size_t old_size, size_t new_size, int flags) {
    (void)addr;
    (void)old_size;
    (void)new_size;
    (void)flags;
    return 0;
}
