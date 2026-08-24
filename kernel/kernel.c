#include <stdint.h>

#include "drivers/serial.h"
#include "drivers/pic.h"
#include "drivers/pit.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "panic.h"

#define TIMER_FREQUENCY_HZ 100u
#define TIMER_SELFTEST_TARGET_TICKS 100u /* ~1 real second at 100Hz */

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

    /* Milestone 2 self-test: deliberately take a #BP fault and check its
       dump reports the right vector -- exceptions.c's isr_handler
       resumes normally after vector 3 specifically (see its comment),
       so boot continues right after this. */
    __asm__ volatile("int3");

    pic_remap();
    pit_init(TIMER_FREQUENCY_HZ);
    pic_clear_mask(0); /* unmask IRQ0 (timer) only -- nothing else has a handler yet */
    __asm__ volatile("sti");
    serial_write("[OK] pic/pit initialized, timer IRQ0 unmasked\n");

    /* Milestone 5 self-test: wait for real IRQ0 ticks to accumulate
       instead of just checking pit_init() didn't crash -- proves the
       PIC remap, the IDT's IRQ gates, and the timer are all actually
       wired together and firing, not just individually plausible. */
    while (pit_get_ticks() < TIMER_SELFTEST_TARGET_TICKS) {
        __asm__ volatile("hlt");
    }
    serial_write("[OK] timer self-test passed (");
    serial_write_hex(pit_get_ticks());
    serial_write(" ticks received via IRQ0)\n");

    /* Steady state: idle, servicing timer interrupts forever. Unlike
       every earlier milestone, there's no reason to end in a deliberate
       panic anymore -- the kernel now has a legitimate, ongoing reason
       to keep running. */
    for (;;) {
        __asm__ volatile("hlt");
    }
}
