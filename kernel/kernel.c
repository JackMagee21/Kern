#include <stdint.h>

#include "drivers/serial.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "panic.h"

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289u

void kernel_main(uint32_t magic, uint32_t mbi_addr)
{
    serial_init();

    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        panic("invalid multiboot2 magic");
    }

    serial_write("[OK] hello kernel\n");

    gdt_init();
    idt_init();
    serial_write("[OK] gdt/idt installed\n");

    pmm_init(mbi_addr);
    serial_write("[OK] pmm initialized, free frames: 0x");
    serial_write_hex(pmm_frames_free());
    serial_write(" / total: 0x");
    serial_write_hex(pmm_frames_total());
    serial_write("\n");

    /* Milestone 3 self-test: allocate two distinct frames, free one,
       and confirm the next allocation reuses exactly that frame --
       proves the bitmap is actually tracking state, not just that
       pmm_init() ran without crashing. */
    uint64_t frame_a = pmm_alloc_frame();
    uint64_t frame_b = pmm_alloc_frame();
    if (frame_a == 0 || frame_b == 0 || frame_a == frame_b) {
        panic("pmm self-test failed: bad allocation");
    }
    pmm_free_frame(frame_a);
    uint64_t frame_c = pmm_alloc_frame();
    if (frame_c != frame_a) {
        panic("pmm self-test failed: freed frame not reused");
    }
    serial_write("[OK] pmm self-test passed (alloc/free/reuse)\n");

    heap_init();
    serial_write("[OK] kernel heap initialized\n");

    /* Milestone 4 self-test: allocate two distinct blocks, write and
       read back distinct fill patterns (catches overlap bugs the
       splitting/coalescing logic could introduce, not just NULL
       returns), then free one and confirm the next allocation reuses
       it -- same "prove it actually works, not just that init() didn't
       crash" standard as Milestones 2/3's self-tests. */
    uint8_t *heap_a = (uint8_t *)kmalloc(64);
    uint8_t *heap_b = (uint8_t *)kmalloc(128);
    if (heap_a == NULL || heap_b == NULL || heap_a == heap_b) {
        panic("heap self-test failed: bad allocation");
    }
    for (int i = 0; i < 64; i++) {
        heap_a[i] = 0xaa;
    }
    for (int i = 0; i < 128; i++) {
        heap_b[i] = 0xbb;
    }
    for (int i = 0; i < 64; i++) {
        if (heap_a[i] != 0xaa) {
            panic("heap self-test failed: allocation A corrupted (overlap?)");
        }
    }
    for (int i = 0; i < 128; i++) {
        if (heap_b[i] != 0xbb) {
            panic("heap self-test failed: allocation B corrupted (overlap?)");
        }
    }
    kfree(heap_a);
    void *heap_c = kmalloc(64);
    if (heap_c != heap_a) {
        panic("heap self-test failed: freed block not reused");
    }
    serial_write("[OK] heap self-test passed (alloc/write/verify/free/reuse)\n");

    /* Milestone 2 self-test: every exception handler is a terminal fault
       dump for now (no recovery/scheduler exists to resume into), so the
       only way to prove the IDT path works end to end is to deliberately
       take a fault and check its dump. #BP is used because it's benign
       and carries no error code. This line -- and the kernel's ability to
       do anything useful after it -- goes away once Milestone 5/6 give
       handlers somewhere to return to. */
    __asm__ volatile("int3");

    for (;;) {
        __asm__ volatile("hlt");
    }
}
