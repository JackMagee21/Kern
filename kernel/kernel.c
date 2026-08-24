#include <stdbool.h>
#include <stdint.h>

#include "drivers/serial.h"
#include "drivers/vga.h"
#include "drivers/console.h"
#include "drivers/pic.h"
#include "drivers/pit.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/pci.h"
#include "drivers/rtc.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/tss.h"
#include "arch/x86_64/syscall.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "sched/scheduler.h"
#include "sched/task.h"
#include "panic.h"
#include "shell.h"

#define TIMER_FREQUENCY_HZ 100u
#define TIMER_SELFTEST_TARGET_TICKS 100u /* ~1 real second at 100Hz */

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289u

extern const uint8_t fork_demo_image_start[]; /* kernel/sched/fork_demo_blob.asm: embedded build/kernel/user/fork_demo.elf */
extern const uint8_t fork_demo_image_end[];

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

/* Milestone 13 (ADR 0013) self-test state: single-writer (pci_scan()'s
   callback runs synchronously, once per found device, all from one
   pci_scan() call on the bootstrap task) -- no synchronization needed,
   same reasoning as the demo task counters above. */
static bool pci_found_host_bridge;

static void pci_report_device(const pci_device_t *dev, void *ctx)
{
    (void)ctx;
    console_write("[PCI] bus 0x");
    console_write_hex(dev->bus);
    console_write(" dev 0x");
    console_write_hex(dev->device);
    console_write(" fn 0x");
    console_write_hex(dev->function);
    console_write(": vendor 0x");
    console_write_hex(dev->vendor_id);
    console_write(" device 0x");
    console_write_hex(dev->device_id);
    console_write(" class 0x");
    console_write_hex(dev->class_code);
    console_write("\n");

    /* Every QEMU i440fx-based machine (the default "-M pc") has an
       Intel (vendor 0x8086) host bridge at bus 0, device 0, function 0
       -- the one assertion this self-test can rely on without
       depending on which OTHER peripherals a particular QEMU version/
       invocation happens to expose (IDE, VGA, etc. vary; the host
       bridge doesn't). */
    if (dev->bus == 0 && dev->device == 0 && dev->function == 0 && dev->vendor_id == 0x8086) {
        pci_found_host_bridge = true;
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

    /* Milestone 11 (ADR 0011): must run before any VMM_FLAG_NX mapping
       is created (heap_init() below, and every task_create_user()
       process's stack) -- panics if the CPU doesn't actually support
       NX rather than letting a later mapping fault with a confusing
       reserved-bit #PF instead. */
    vmm_enable_nx();
    console_write("[OK] NX (no-execute) enabled\n");

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

    /* Milestone 19 (ADR 0019): must run before the first
       vmm_create_address_space() call (before heap_init() is fine --
       the heap doesn't create address spaces -- but well before
       task_create_user()/task_fork() ever do), so the new PDPT
       entries it adds under the shared PML4[511] are already present
       when copied by reference into every future process's table
       (ADR 0009). */
    vmm_direct_map_init();
    console_write("[OK] physical memory direct-map initialized\n");

    /* Self-test: write a known pattern to a fresh frame through
       vmm_phys_to_virt(), then read it back through a COMPLETELY
       INDEPENDENT translation path -- the raw low identity mapping
       boot.asm already set up (valid here specifically because only a
       couple of frames have been allocated so far, so pmm_alloc_frame's
       lowest-numbered-first policy guarantees this one is still well
       within the low identity window, the same reasoning
       VMM_IDENTITY_WINDOW_LIMIT's own doc comment relies on). Two
       different address-translation paths agreeing on the same
       physical byte is real evidence the direct-map's arithmetic is
       correct, not just "reads back what it wrote through itself". */
    uint64_t direct_map_test_frame = pmm_alloc_frame();
    if (direct_map_test_frame == 0) {
        panic("direct-map self-test failed: pmm exhausted");
    }
    uint8_t *via_direct_map = (uint8_t *)(uintptr_t)vmm_phys_to_virt(direct_map_test_frame);
    for (int i = 0; i < 16; i++) {
        via_direct_map[i] = (uint8_t)(0xd0 + i);
    }
    uint8_t *via_identity_map = (uint8_t *)(uintptr_t)direct_map_test_frame;
    for (int i = 0; i < 16; i++) {
        if (via_identity_map[i] != (uint8_t)(0xd0 + i)) {
            panic("direct-map self-test failed: write via vmm_phys_to_virt not visible via the low identity mapping");
        }
    }
    pmm_free_frame(direct_map_test_frame);
    console_write("[OK] direct-map self-test passed (write via vmm_phys_to_virt visible via the low identity mapping)\n");

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

    /* Milestone 11 (ADR 0011) self-test: confirm heap_init()'s
       VMM_FLAG_NX mapping actually produced a non-executable PTE, and
       (so this isn't a spuriously-always-false check) that ordinary
       kernel code is still correctly reported executable. Checking the
       real page-table state, not triggering a live NX fault -- see
       vmm_page_is_executable_in()'s doc comment for why: there's no
       exception-recovery mechanism yet to safely resume past one. */
    if (vmm_page_is_executable_in(vmm_current_pml4(), (uint64_t)(uintptr_t)heap_c)) {
        panic("NX self-test failed: kernel heap page reported executable");
    }
    if (!vmm_page_is_executable_in(vmm_current_pml4(), (uint64_t)(uintptr_t)kernel_main)) {
        panic("NX self-test failed: ordinary kernel code reported non-executable");
    }
    console_write("[OK] NX self-test passed (heap is non-executable, kernel code still is)\n");

    /* Milestone 2 self-test: deliberately take a #BP fault and check its
       dump reports the right vector -- exceptions.c's isr_handler
       resumes normally after vector 3 specifically (see its comment),
       so boot continues right after this. */
    __asm__ volatile("int3");

    /* Milestone 13 (ADR 0013): pure port I/O, no dependency on paging/
       heap/scheduler state, so this can run anywhere -- placed here,
       grouped with the rest of hardware/driver bring-up rather than
       the earlier memory-management self-tests. */
    pci_found_host_bridge = false;
    uint32_t pci_device_count = pci_scan(pci_report_device, NULL);
    if (pci_device_count == 0 || !pci_found_host_bridge) {
        panic("pci self-test failed: no devices found or host bridge missing");
    }
    console_write("[OK] pci self-test passed (0x");
    console_write_hex(pci_device_count);
    console_write(" device(s) found, host bridge present)\n");

    /* Milestone 14 (ADR 0014): can't check against a known expected
       wall-clock value (there isn't one -- this runs whenever it runs),
       so the self-test instead confirms every field decoded into a
       SANE range. A BCD-vs-binary or register-index bug would very
       likely produce an out-of-range value in at least one field (e.g.
       a raw BCD 0x59 misread as binary 89), so this is a real
       correctness check, not just "didn't crash." */
    rtc_time_t boot_time;
    rtc_read(&boot_time);
    if (boot_time.second > 59 || boot_time.minute > 59 || boot_time.hour > 23
        || boot_time.day < 1 || boot_time.day > 31
        || boot_time.month < 1 || boot_time.month > 12
        || boot_time.year < 2020 || boot_time.year > 2100) {
        panic("rtc self-test failed: decoded time field out of sane range");
    }
    console_write("[OK] rtc self-test passed, boot time (fields in hex, same as every other\n     field in this log): year 0x");
    console_write_hex(boot_time.year);
    console_write(" month 0x");
    console_write_hex(boot_time.month);
    console_write(" day 0x");
    console_write_hex(boot_time.day);
    console_write(" hour 0x");
    console_write_hex(boot_time.hour);
    console_write(" min 0x");
    console_write_hex(boot_time.minute);
    console_write(" sec 0x");
    console_write_hex(boot_time.second);
    console_write("\n");

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
    task_t *task_a = task_create(demo_task_a);
    scheduler_add_task(task_a);
    scheduler_add_task(task_create(demo_task_b));

    /* Milestone 12 (ADR 0012) self-test: task_a's kernel-mode stack
       (kernel_stack_base) is one of alloc_kernel_stack()'s dedicated,
       guard-paged VA slots (kernel/sched/task.c) -- confirm the page
       immediately below it is genuinely unmapped, not just that
       nothing has crashed yet. Checking real page-table state, same
       reasoning as the NX self-test above: there's no exception-
       recovery mechanism yet to safely trigger a live overflow and
       watch it fault. */
    uint64_t guard_page = task_a->kernel_stack_base - PMM_FRAME_SIZE;
    uint64_t unused_phys;
    if (vmm_translate(guard_page, &unused_phys)) {
        panic("guard page self-test failed: kernel stack's guard page is mapped");
    }
    console_write("[OK] guard page self-test passed (kernel stack guard page is unmapped)\n");

    /* Milestone 10 (ADR 0010) self-test setup: captured BEFORE either
       process exists, so that once both have exited and been fully
       reaped, comparing against this baseline proves every frame
       vmm_create_address_space()/vmm_map_page_in() consumed for them
       actually came back -- not just "didn't crash while freeing." */
    uint64_t frames_before_processes = pmm_frames_free();

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

    /* Milestone 18 (ADR 0018): a THIRD orphan process (parent_id == 0,
       same as process_a/process_b -- kernel_main itself never
       scheduler_try_wait()s for anything) whose own job is to fork a
       FOURTH process at runtime and prove fork/wait end to end
       (kernel/user/fork_demo.asm). Deliberately a separate log line
       from the "2 ring-3 processes" one above -- keeps that exact,
       already-tested marker text unchanged rather than folding this
       into it. */
    task_t *fork_demo_process = task_create_user_image(fork_demo_image_start, fork_demo_image_end);
    scheduler_add_task(fork_demo_process);
    console_write("[OK] fork/wait demo process created, pid 0x");
    console_write_hex(fork_demo_process->id);
    console_write("\n");

    keyboard_init();
    mouse_init();
    pic_clear_mask(0);  /* IRQ0: timer */
    pic_clear_mask(1);  /* IRQ1: keyboard */
    pic_clear_mask(2);  /* IRQ2: master PIC's cascade line -- MUST be unmasked or IRQ8-15
                            (the slave PIC, including IRQ12's mouse) can never reach the CPU
                            at all, regardless of IRQ12's own mask bit (ADR 0016). */
    pic_clear_mask(12); /* IRQ12: mouse */
    __asm__ volatile("sti");
    console_write("[OK] pic/pit/keyboard/mouse initialized, IRQ0+IRQ1+IRQ2+IRQ12 unmasked\n");

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
       and either be deep into their bounded sys_nop loops or have
       already run them to completion and exited (Milestone 10 -- see
       kernel/user/hello.asm's LOOP_COUNT, Milestone 17). Either way
       syscall_get_count() only
       ever grows, so ">1" remains the meaningful floor: exactly one
       sys_write and nothing else would mean sys_nop never landed. */
    if (syscall_get_count() <= 1) {
        panic("syscall self-test failed: ring-3 processes' syscalls did not land repeatedly");
    }
    console_write("[OK] syscall self-test passed, ");
    console_write_hex(syscall_get_count());
    console_write(" syscalls serviced from 2 ring-3 processes\n");

    /* Milestone 10 (ADR 0010) self-test: both processes' loaded program
       (kernel/user/hello.asm, Milestone 17) runs a BOUNDED sys_nop loop
       specifically so this is observable --
       wait (the same hlt-loop-until-a-counter-advances pattern as the
       timer self-test above) for the reaper to have actually torn both
       down, then confirm every frame their address spaces consumed
       came back. A leak here would silently regress every future
       milestone that creates and exits processes repeatedly (e.g. a
       real shell running multiple programs) into slowly exhausting
       physical memory. Milestone 18 (ADR 0018) raised the target from
       2 to 4: frames_before_processes was captured before the
       fork/wait demo process too (created above, alongside process_a/
       process_b), and that process's own runtime-forked child is a
       FIFTH task whose resources must also come back before this
       baseline comparison is valid -- 2 hello processes + the fork
       demo's own exit + its forked child's exit. */
    while (scheduler_reaped_count() < 4) {
        __asm__ volatile("hlt");
    }
    uint64_t frames_after_reap = pmm_frames_free();
    if (frames_after_reap != frames_before_processes) {
        panic("process lifecycle self-test failed: frames leaked after all processes exited");
    }
    console_write("[OK] process lifecycle self-test passed, all ring-3 processes exited and were fully reaped (0x");
    console_write_hex(frames_after_reap);
    console_write(" frames free, matches pre-creation baseline)\n");

    /* Steady state: an interactive shell instead of a bare idle loop --
       this is what actually makes the kernel usable sitting at real
       hardware. Still just one more participant in the scheduler's
       round-robin (kernel_main's own bootstrap task), competing fairly
       with the Milestone 6/7 demo tasks the same way any task does;
       waiting on keyboard input via hlt naturally yields the rest of
       its time slice each round. */
    shell_run();
}
