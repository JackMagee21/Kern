#include <stdint.h>

#include "drivers/serial.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "mm/pmm.h"

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289u

static void panic(const char *message)
{
    serial_write("[PANIC] ");
    serial_write(message);
    serial_write("\n");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

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
