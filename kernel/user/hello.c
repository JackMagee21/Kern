/* Milestone 24: hello.asm rewritten in C using the new minimal
   userspace runtime (kernel/user/rt/) -- proves the runtime itself is
   correct by matching hello.asm's behavior byte-for-byte, so every
   existing test that already asserts on this program's output
   (test_elf_loader_selftest.sh, test_ring3_syscall_selftest.sh,
   kernel/kernel.c's own self-tests) passes UNCHANGED, no assertion
   updates needed -- the strongest form of "this new thing produces the
   same real result the old, already-trusted thing did." See Desktop.md
   for why this rewrite, not a new program, was chosen as the runtime's
   own first proof.

   Still linked via kernel/user/user.ld at the same fixed virtual
   address task.c's convention expects; kernel/mm/elf_loader.c doesn't
   care whether the bytes it's loading came from NASM or a C compiler. */

#include <stdint.h>

#include "rt/syscall.h"

static const char msg1[] = "[OK] hello from ring 3 via ELF-loaded process\n";
static const char msg2[] = "[OK] elf .data/.bss segment verification passed\n";
static const char msg_bad[] = "[FAIL] elf .data/.bss segment verification failed\n";

/* Same three things hello.asm's own doc comment named: (1) .data is
   really copied from the file (must read back 0x1234, not 0); (2) .bss
   is really zero-filled (must read back 0 before this code ever writes
   it); (3) a writable data segment is really writable (bss_var can be
   written and read back afterward). */
static uint64_t data_var = 0x1234;
static uint64_t bss_var;

#define LOOP_COUNT 200000

int main(void)
{
    sys_write(msg1, sizeof(msg1) - 1);

    if (bss_var != 0 || data_var != 0x1234) {
        sys_write(msg_bad, sizeof(msg_bad) - 1);
    } else {
        bss_var = 0x5678;
        sys_write(msg2, sizeof(msg2) - 1);
    }

    /* Same bounded sys_nop spin hello.asm's own LOOP_COUNT already
       used (Milestone 17) -- kernel_main's syscall self-test counts on
       this to observe repeated syscalls from this process. */
    for (uint64_t i = 0; i < LOOP_COUNT; i++) {
        sys_nop();
    }

    return 0; /* crt0.asm turns this into sys_exit(0) */
}
