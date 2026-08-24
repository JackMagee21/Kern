#include <stdint.h>

#include "drivers/serial.h"
#include "drivers/vga.h"
#include "drivers/console.h"
#include "drivers/pic.h"
#include "drivers/pit.h"
#include "drivers/keyboard.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/tss.h"
#include "arch/x86_64/syscall.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "sched/scheduler.h"
#include "sched/task.h"
#include "panic.h"
#include "shell.h"

#define TIMER_FREQUENCY_HZ 100u
#define TIMER_SELFTEST_TARGET_TICKS 100u /* ~1 real second at 100Hz */

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289u

/* Milestone 6 self-test: two kernel threads that never voluntarily
   yield, proving the scheduler forcibly preempts a task that never
   gives up the CPU on its own -- not just that cooperative switching
   works. Single-writer-per-counter (each task only increments its own),
   kernel_main only reads them, so no synchronization is needed (same
   reasoning as pit.c's tick_count). */
static volatile uint64_t demo_task_a_ticks;
static volatile uint64_t demo_task_b_ticks;

static void demo_task_a(void)
{
    for (;;) {
        demo_task_a_ticks++;
    }
}

static void demo_task_b(void)
{
    for (;;) {
        demo_task_b_ticks++;
    }
}

void kernel_main(uint32_t magic, uint32_t mbi_addr)
{
    serial_init();
    vga_init();

    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        panic("invalid multiboot2 magic");
    }

    console_write("[OK] hello kernel\n");

    gdt_init();
    idt_init();
    console_write("[OK] gdt/idt installed\n");

    pmm_init(mbi_addr);
    console_write("[OK] pmm initialized, free frames: 0x");
    console_write_hex(pmm_frames_free());
    console_write(" / total: 0x");
    console_write_hex(pmm_frames_total());
    console_write("\n");

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
    console_write("[OK] pmm self-test passed (alloc/free/reuse)\n");

    heap_init();
    console_write("[OK] kernel heap initialized\n");

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
    console_write("[OK] heap self-test passed (alloc/write/verify/free/reuse)\n");

    /* Milestone 2 self-test: deliberately take a #BP fault and check its
       dump reports the right vector -- exceptions.c's isr_handler
       resumes normally after vector 3 specifically (see its comment),
       so boot continues right after this. */
    __asm__ volatile("int3");

    pic_remap();
    pit_init(TIMER_FREQUENCY_HZ);

    /* Milestone 7: TSS (needed for TSS.RSP0 whenever a ring-3 task is
       interrupted -- see task.h) and SYSCALL/SYSRET MSR programming.
       tss_init() needs the heap (its default RSP0 stack), so this runs
       after heap_init(), and both need gdt_init() already done (the TSS
       descriptor slot and the GDT selector layout STAR depends on). */
    tss_init();
    syscall_init();
    console_write("[OK] tss/syscall initialized\n");

    /* Milestone 6: scheduler owns IRQ0 now (it calls pit_tick() itself
       -- see kernel/sched/scheduler.c), so it must be wired up before
       ticks start flowing. Bootstrap task represents kernel_main's own
       context; the demo tasks join the round-robin before interrupts
       are enabled. */
    scheduler_init();
    scheduler_add_task(task_create(demo_task_a));
    scheduler_add_task(task_create(demo_task_b));

    /* Two independent ring-3 processes, not one -- proves per-process
       address spaces actually isolate rather than just "didn't crash
       with one process like Milestone 7's shared design did." Safe to
       reuse the same demo code (mapped read-only into both, ADR 0009);
       each gets its own private stack and its own top-level page
       table. */
    task_t *process_a = task_create_user();
    task_t *process_b = task_create_user();
    scheduler_add_task(process_a);
    scheduler_add_task(process_b);
    console_write("[OK] scheduler initialized, 2 kernel + 2 ring-3 processes created\n");
    console_write("[OK] process A pml4: 0x");
    console_write_hex(process_a->pml4);
    console_write(", process B pml4: 0x");
    console_write_hex(process_b->pml4);
    console_write(" (different address spaces)\n");

    keyboard_init();
    pic_clear_mask(0); /* IRQ0: timer */
    pic_clear_mask(1); /* IRQ1: keyboard */
    __asm__ volatile("sti");
    console_write("[OK] pic/pit/keyboard initialized, IRQ0+IRQ1 unmasked\n");

    /* Milestone 5 self-test: wait for real IRQ0 ticks to accumulate
       instead of just checking pit_init() didn't crash -- proves the
       PIC remap, the IDT's IRQ gates, and the timer are all actually
       wired together and firing, not just individually plausible. */
    while (pit_get_ticks() < TIMER_SELFTEST_TARGET_TICKS) {
        __asm__ volatile("hlt");
    }
    console_write("[OK] timer self-test passed (");
    console_write_hex(pit_get_ticks());
    console_write(" ticks received via IRQ0)\n");

    /* Milestone 6 self-test: by now ~1 real second (100 ticks) has
       elapsed. If preemption weren't actually forcing the CPU away from
       a busy-looping task, one of these would still be exactly 0 --
       whichever task happened to run first would simply never give it
       up. Deliberately checking ">0", not some large threshold: the
       exact count depends on host/QEMU speed, but "made any progress at
       all while sharing the CPU with another non-yielding task" is a
       fixed, meaningful bar regardless of speed. */
    if (demo_task_a_ticks == 0 || demo_task_b_ticks == 0) {
        panic("scheduler self-test failed: a demo task never got scheduled");
    }
    console_write("[OK] scheduler self-test passed, task A: 0x");
    console_write_hex(demo_task_a_ticks);
    console_write(", task B: 0x");
    console_write_hex(demo_task_b_ticks);
    console_write(" (both made progress under preemption)\n");

    /* Milestone 7/9 self-test: by now both ring-3 processes should have
       made their one-shot sys_write calls (proving the validated-
       pointer path, independently, from two different address spaces)
       and be well into their sys_nop loops (proving SYSCALL/SYSRET
       round-trips reliably under concurrent multi-process load, not
       just once from one process). syscall_get_count() covers every
       syscall from both processes, so ">1" remains the meaningful
       floor: exactly one sys_write and nothing else would mean sys_nop
       never landed. */
    if (syscall_get_count() <= 1) {
        panic("syscall self-test failed: ring-3 processes' syscalls did not land repeatedly");
    }
    console_write("[OK] syscall self-test passed, ");
    console_write_hex(syscall_get_count());
    console_write(" syscalls serviced from 2 ring-3 processes\n");

    /* Steady state: an interactive shell instead of a bare idle loop --
       this is what actually makes the kernel usable sitting at real
       hardware. Still just one more participant in the scheduler's
       round-robin (kernel_main's own bootstrap task), competing fairly
       with the Milestone 6/7 demo tasks the same way any task does;
       waiting on keyboard input via hlt naturally yields the rest of
       its time slice each round. */
    shell_run();
}
