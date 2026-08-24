#include <stdint.h>

#include "vmm.h"
#include "pmm.h"
#include "../arch/x86_64/msr.h"
#include "../panic.h"

/*
 * 4-level page-table walker/mapper, built on top of the physical frame
 * allocator (pmm.c). See ADR 0004 for why this EXTENDS boot.asm's
 * existing PML4 (reused via CR3, never reloaded) with new 4KiB-paged
 * regions, rather than replacing boot.asm's 2MiB identity/higher-half
 * mapping outright -- lower blast radius for a paging change, and nothing
 * yet needs the kernel image itself to be remapped at finer granularity.
 *
 * Entry bit layout (present/writable/PS) is Intel SDM Vol. 3A Sec. 4.5's
 * 4-level paging format -- the same bits boot.asm already uses
 * successfully (bit0=present, bit1=writable, bit7=PS at PD/PDPT level),
 * so this isn't a fresh guess, it's the same encoding already proven to
 * boot correctly.
 */

#define ENTRIES_PER_TABLE 512

#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_PS        (1ULL << 7) /* huge page at PD/PDPT level (boot.asm's 2MiB mappings) */
#define PTE_ADDR_MASK 0x000ffffffffff000ULL /* bits 12-51 */

/* VMM_IDENTITY_WINDOW_LIMIT is declared in vmm.h -- Milestone 17's ELF
   loader (kernel/mm/elf_loader.c) needs the same constant, which is why
   it moved out of this file. */

#define MSR_EFER 0xC0000080u /* same MSR syscall.c already programs EFER_SCE into */
#define EFER_NXE (1ULL << 11)

void vmm_enable_nx(void)
{
    /* CPUID 80000001h:EDX bit 20 = NX/XD available (Intel SDM Vol. 2A
       Table 3-8 / AMD64 APM Vol. 3) -- the exact same extended leaf
       boot.asm's check_long_mode already queried (bit 29, LM) before
       this kernel ever reached long mode, so leaf 80000000h having
       reported >= 80000001h is already a proven invariant by the time
       any C code runs; only the NX bit itself needs checking here. */
    uint32_t eax = 0x80000001u, edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=d"(edx) : : "ebx", "ecx");
    if (!(edx & (1u << 20))) {
        panic("vmm: CPU does not support the NX/XD feature (CPUID 80000001h:EDX bit 20)");
    }

    uint64_t efer = read_msr(MSR_EFER);
    write_msr(MSR_EFER, efer | EFER_NXE);
}

static inline uint64_t pml4_index(uint64_t va) { return (va >> 39) & 0x1ff; }
static inline uint64_t pdpt_index(uint64_t va) { return (va >> 30) & 0x1ff; }
static inline uint64_t pd_index(uint64_t va)   { return (va >> 21) & 0x1ff; }
static inline uint64_t pt_index(uint64_t va)   { return (va >> 12) & 0x1ff; }

uint64_t vmm_current_pml4(void)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & PTE_ADDR_MASK;
}

static uint64_t *get_pml4(void)
{
    /* boot.asm's pml4 (and, since address spaces exist, any other
       table CR3 might point at) lives within the low identity window,
       so casting the physical address straight to a pointer is valid. */
    return (uint64_t *)(uintptr_t)vmm_current_pml4();
}

static uint64_t *get_or_create_table(uint64_t *table, uint64_t index, bool user)
{
    if (!(table[index] & PTE_PRESENT)) {
        uint64_t frame = pmm_alloc_frame();
        if (frame == 0 || frame >= VMM_IDENTITY_WINDOW_LIMIT) {
            panic("vmm: page-table bootstrap frame outside identity window");
        }

        uint64_t *new_table = (uint64_t *)(uintptr_t)frame;
        for (int i = 0; i < ENTRIES_PER_TABLE; i++) {
            new_table[i] = 0;
        }

        table[index] = frame | PTE_PRESENT | PTE_WRITABLE | (user ? PTE_USER : 0);
    } else if (user && !(table[index] & PTE_USER)) {
        /* PML4[511] alone spans the entire top-2GiB kernel region
           (kernel image, heap, AND the "dedicated" user PDPT slots all
           fall under it -- mcmodel=kernel's whole -2GB range is one
           PML4 entry), so it's unavoidably shared and may already
           exist, created supervisor-only by an earlier non-user
           mapping (e.g. heap_init(), which runs first). Upgrading it
           here is safe: the U bit is necessary but not sufficient --
           every OTHER region's own leaf PTEs are still supervisor-only
           and independently block ring-3 access, so this can't expose
           anything beyond the new user mapping this call is actually
           creating. */
        table[index] |= PTE_USER;
    }

    return (uint64_t *)(uintptr_t)(table[index] & PTE_ADDR_MASK);
}

/* Milestone 19: a general physical-memory direct-map, closing the ADR
   0004 known limitation Milestones 17/18 both ran into a second and
   third time (elf_load()'s segment frames, task_fork()'s copied
   frames both needed to be directly writable via their own physical
   address, the same identity-window constraint vmm.c's own page-table
   bootstrap frames have always had). Covers the FULL 4GiB pmm.h's
   bitmap tracks (PMM_FRAME bitmap size), unconditionally, regardless
   of how much RAM is actually installed -- accessing an unbacked
   physical address through it would only happen if a caller passes a
   bogus frame number, a caller bug this function has no way to detect
   (same trust boundary as every other pmm_alloc_frame() consumer).
   Placed at PDPT[505..508] (4 x 1GiB, verified against ADR 0012's
   established indices via python3, not guessed -- PDPT[509]=kernel
   stacks, [510]=kernel image, [511]=heap, all under the SAME shared
   PML4[511] entry, so 505-508 needed to be confirmed free of those,
   not assumed), using 2MiB PS pages (the same encoding boot.asm's own
   identity map already proved correct, Sec. 4.5) rather than 4KiB
   pages -- 2048 PD entries total instead of over a million PT entries.

   MUST run before the first vmm_create_address_space() call (i.e.
   before any ring-3 process exists): PML4[511]'s entry is copied BY
   REFERENCE into every new address space (ADR 0009), so the PDPT
   entries this function adds are only automatically visible to future
   processes if they already exist at that point -- kernel_main enforces
   this ordering (called right after pmm_init(), before heap_init() or
   any task_create_user()/task_fork()).

   Does NOT replace vmm.c's own page-table BOOTSTRAP frames' identity-
   window requirement (get_or_create_table above, and
   vmm_create_address_space() below) -- those allocate the very tables
   this function (and everything else) depends on, so they can't be
   bootstrapped via a direct map that doesn't exist yet. This function
   closes the gap for DATA frames (something's actual content, like an
   ELF segment or a forked page) only. */
#define DIRECT_MAP_VIRT_BASE  0xFFFFFFFE40000000ULL
#define DIRECT_MAP_PDPT_START 505u
#define DIRECT_MAP_PDPT_COUNT 4u /* 4 x 1GiB = 4GiB, matches pmm.h's PMM_FRAME bitmap tracking limit */
#define PAGE_2MIB_SIZE         0x200000ULL

void vmm_direct_map_init(void)
{
    uint64_t *pml4 = get_pml4();
    uint64_t *pdpt = get_or_create_table(pml4, 511, false); /* shared kernel-half PDPT -- same one heap.c/task.c already extend */

    uint64_t phys = 0;
    for (uint64_t slot = 0; slot < DIRECT_MAP_PDPT_COUNT; slot++) {
        uint64_t pd_frame = pmm_alloc_frame();
        if (pd_frame == 0 || pd_frame >= VMM_IDENTITY_WINDOW_LIMIT) {
            panic("vmm_direct_map_init: pmm exhausted or PD frame outside identity window");
        }

        /* New (previously-absent) mappings, not a present->present
           change -- no invlpg needed (Intel SDM Vol. 3A Sec. 4.10: the
           TLB never caches a not-present translation as valid, so
           there's nothing stale to invalidate here), same reasoning
           vmm_map_page_in()'s own doc comment already relies on. */
        uint64_t *pd = (uint64_t *)(uintptr_t)pd_frame;
        for (uint64_t i = 0; i < ENTRIES_PER_TABLE; i++) {
            pd[i] = phys | PTE_PRESENT | PTE_WRITABLE | PTE_PS;
            phys += PAGE_2MIB_SIZE;
        }

        pdpt[DIRECT_MAP_PDPT_START + slot] = pd_frame | PTE_PRESENT | PTE_WRITABLE;
    }
}

uint64_t vmm_phys_to_virt(uint64_t phys_addr)
{
    return DIRECT_MAP_VIRT_BASE + phys_addr;
}

bool vmm_map_page_in(uint64_t pml4_phys, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags)
{
    bool user = (flags & VMM_FLAG_USER) != 0;

    /* pml4_phys must itself be identity-reachable, same requirement as
       every table get_or_create_table allocates -- true for both
       get_pml4() (the caller's own live table) and any PML4
       vmm_create_address_space() just handed back (fresh
       pmm_alloc_frame(), same guarantee). */
    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_phys;
    uint64_t *pdpt = get_or_create_table(pml4, pml4_index(virt_addr), user);
    uint64_t *pd   = get_or_create_table(pdpt, pdpt_index(virt_addr), user);
    uint64_t *pt   = get_or_create_table(pd, pd_index(virt_addr), user);

    uint64_t *pte = &pt[pt_index(virt_addr)];
    if (*pte & PTE_PRESENT) {
        return false; /* refuse to silently overwrite an existing mapping */
    }

    *pte = (phys_addr & PTE_ADDR_MASK) | PTE_PRESENT | flags;

    /* Harmless (no-op) if pml4_phys isn't the currently active table --
       there is nothing cached for an address space that was never
       loaded. Necessary when it IS (the vmm_map_page() case below). */
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    return true;
}

bool vmm_map_page(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags)
{
    return vmm_map_page_in(vmm_current_pml4(), virt_addr, phys_addr, flags);
}

uint64_t vmm_create_address_space(void)
{
    uint64_t new_pml4_frame = pmm_alloc_frame();
    if (new_pml4_frame == 0 || new_pml4_frame >= VMM_IDENTITY_WINDOW_LIMIT) {
        panic("vmm: address-space PML4 frame outside identity window");
    }

    uint64_t *new_pml4 = (uint64_t *)(uintptr_t)new_pml4_frame;
    for (int i = 0; i < ENTRIES_PER_TABLE; i++) {
        new_pml4[i] = 0;
    }

    /* Share the kernel half (PML4[511]: image + heap) by copying the
       ENTRY (a pointer to the existing PDPT), not its contents -- see
       this function's doc comment in vmm.h for why that keeps every
       address space permanently in sync with kernel/heap growth, not
       just at this snapshot in time.

       ALSO share PML4[0] -- boot.asm's low identity map. Found the hard
       way (a real triple/page-fault chain on the first per-process
       boot, see ADR 0009): kernel code that runs under a PROCESS's CR3
       (a syscall or exception handler -- neither SYSCALL nor an
       interrupt switches CR3 on entry) still needs identity-mapped
       kernel structures reachable: vmm.c's own page-table-walking
       functions (get_pml4/is_user_page) cast the live CR3 straight to a
       pointer, the VGA console writes straight to 0xB8000, and any
       future kernel code doing the same would hit the identical
       problem. Safe to share: every identity-mapped entry is
       supervisor-only (boot.asm never sets the U bit there), so this
       grants kernel CODE broader reach without granting RING-3 code in
       that process anything new -- the leaf permissions ring 3 would
       actually hit are unchanged. This is also exactly why process
       code/stack (kernel/sched/task.c) live under PML4 index 1, not 0:
       PML4[0] is now committed entirely to this shared identity map, so
       nothing process-private can live there too. */
    uint64_t *current_pml4 = get_pml4();
    new_pml4[0] = current_pml4[0];
    new_pml4[511] = current_pml4[511];

    return new_pml4_frame;
}

void vmm_destroy_address_space(uint64_t pml4_phys)
{
    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_phys;

    for (uint64_t i = 0; i < ENTRIES_PER_TABLE; i++) {
        if (i == 0 || i == 511) {
            continue; /* shared with the kernel and every other address space -- never freed here */
        }
        uint64_t pml4e = pml4[i];
        if (!(pml4e & PTE_PRESENT)) {
            continue;
        }
        uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4e & PTE_ADDR_MASK);

        for (uint64_t j = 0; j < ENTRIES_PER_TABLE; j++) {
            uint64_t pdpte = pdpt[j];
            if (!(pdpte & PTE_PRESENT)) {
                continue;
            }
            if (pdpte & PTE_PS) {
                panic("vmm_destroy_address_space: unexpected 1GiB page in a process-private mapping");
            }
            uint64_t *pd = (uint64_t *)(uintptr_t)(pdpte & PTE_ADDR_MASK);

            for (uint64_t k = 0; k < ENTRIES_PER_TABLE; k++) {
                uint64_t pde = pd[k];
                if (!(pde & PTE_PRESENT)) {
                    continue;
                }
                if (pde & PTE_PS) {
                    panic("vmm_destroy_address_space: unexpected 2MiB page in a process-private mapping");
                }
                uint64_t *pt = (uint64_t *)(uintptr_t)(pde & PTE_ADDR_MASK);

                for (uint64_t l = 0; l < ENTRIES_PER_TABLE; l++) {
                    uint64_t pte = pt[l];
                    if (!(pte & PTE_PRESENT)) {
                        continue;
                    }
                    if (pte & VMM_FLAG_OWNED) {
                        pmm_free_frame(pte & PTE_ADDR_MASK);
                    }
                }
                pmm_free_frame((uint64_t)(uintptr_t)pt);
            }
            pmm_free_frame((uint64_t)(uintptr_t)pd);
        }
        pmm_free_frame((uint64_t)(uintptr_t)pdpt);
    }

    pmm_free_frame(pml4_phys);
}

bool vmm_translate(uint64_t virt_addr, uint64_t *out_phys)
{
    uint64_t *pml4 = get_pml4();

    uint64_t pml4e = pml4[pml4_index(virt_addr)];
    if (!(pml4e & PTE_PRESENT)) {
        return false;
    }
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4e & PTE_ADDR_MASK);

    uint64_t pdpte = pdpt[pdpt_index(virt_addr)];
    if (!(pdpte & PTE_PRESENT)) {
        return false;
    }
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpte & PTE_ADDR_MASK);

    uint64_t pde = pd[pd_index(virt_addr)];
    if (!(pde & PTE_PRESENT)) {
        return false;
    }
    if (pde & PTE_PS) {
        *out_phys = (pde & PTE_ADDR_MASK) | (virt_addr & 0x1fffffULL); /* 2MiB page: PDE is the leaf */
        return true;
    }
    uint64_t *pt = (uint64_t *)(uintptr_t)(pde & PTE_ADDR_MASK);

    uint64_t pte = pt[pt_index(virt_addr)];
    if (!(pte & PTE_PRESENT)) {
        return false;
    }
    *out_phys = (pte & PTE_ADDR_MASK) | (virt_addr & 0xfffULL);
    return true;
}

void vmm_unmap_page(uint64_t virt_addr)
{
    uint64_t *pml4 = get_pml4();

    uint64_t pml4e = pml4[pml4_index(virt_addr)];
    if (!(pml4e & PTE_PRESENT)) {
        return;
    }
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4e & PTE_ADDR_MASK);

    uint64_t pdpte = pdpt[pdpt_index(virt_addr)];
    if (!(pdpte & PTE_PRESENT)) {
        return;
    }
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpte & PTE_ADDR_MASK);

    uint64_t pde = pd[pd_index(virt_addr)];
    if (!(pde & PTE_PRESENT)) {
        return;
    }
    uint64_t *pt = (uint64_t *)(uintptr_t)(pde & PTE_ADDR_MASK);

    uint64_t *pte = &pt[pt_index(virt_addr)];
    if (!(*pte & PTE_PRESENT)) {
        return;
    }

    *pte = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
}

/* U/S is effectively ANDed across every level of the walk (Intel SDM
   Vol. 3A Sec. 4.6): a supervisor-only entry at ANY level blocks ring-3
   access to everything beneath it, regardless of the leaf's own bit. */
static bool is_user_page(uint64_t virt_addr)
{
    uint64_t *pml4 = get_pml4();

    uint64_t pml4e = pml4[pml4_index(virt_addr)];
    if (!(pml4e & PTE_PRESENT) || !(pml4e & PTE_USER)) {
        return false;
    }
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4e & PTE_ADDR_MASK);

    uint64_t pdpte = pdpt[pdpt_index(virt_addr)];
    if (!(pdpte & PTE_PRESENT) || !(pdpte & PTE_USER)) {
        return false;
    }
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpte & PTE_ADDR_MASK);

    uint64_t pde = pd[pd_index(virt_addr)];
    if (!(pde & PTE_PRESENT) || !(pde & PTE_USER)) {
        return false;
    }
    if (pde & PTE_PS) {
        return true; /* 2MiB page (boot.asm's mappings): PDE is the leaf */
    }
    uint64_t *pt = (uint64_t *)(uintptr_t)(pde & PTE_ADDR_MASK);

    uint64_t pte = pt[pt_index(virt_addr)];
    return (pte & PTE_PRESENT) && (pte & PTE_USER);
}

bool vmm_page_is_executable_in(uint64_t pml4_phys, uint64_t virt_addr)
{
    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_phys;

    uint64_t pml4e = pml4[pml4_index(virt_addr)];
    if (!(pml4e & PTE_PRESENT)) {
        return false;
    }
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4e & PTE_ADDR_MASK);

    uint64_t pdpte = pdpt[pdpt_index(virt_addr)];
    if (!(pdpte & PTE_PRESENT)) {
        return false;
    }
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpte & PTE_ADDR_MASK);

    uint64_t pde = pd[pd_index(virt_addr)];
    if (!(pde & PTE_PRESENT)) {
        return false;
    }
    if (pde & PTE_PS) {
        return !(pde & VMM_FLAG_NX); /* 2MiB page (boot.asm's mappings): PDE is the leaf */
    }
    uint64_t *pt = (uint64_t *)(uintptr_t)(pde & PTE_ADDR_MASK);

    uint64_t pte = pt[pt_index(virt_addr)];
    if (!(pte & PTE_PRESENT)) {
        return false;
    }
    return !(pte & VMM_FLAG_NX);
}

void vmm_for_each_user_page(uint64_t pml4_phys, vmm_page_visitor_t visitor, void *ctx)
{
    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_phys;

    for (uint64_t i = 0; i < ENTRIES_PER_TABLE; i++) {
        if (i == 0 || i == 511) {
            continue; /* shared identity map / kernel half -- not process-private */
        }
        uint64_t pml4e = pml4[i];
        if (!(pml4e & PTE_PRESENT)) {
            continue;
        }
        uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4e & PTE_ADDR_MASK);

        for (uint64_t j = 0; j < ENTRIES_PER_TABLE; j++) {
            uint64_t pdpte = pdpt[j];
            if (!(pdpte & PTE_PRESENT)) {
                continue;
            }
            if (pdpte & PTE_PS) {
                panic("vmm_for_each_user_page: unexpected 1GiB page in a process-private mapping");
            }
            uint64_t *pd = (uint64_t *)(uintptr_t)(pdpte & PTE_ADDR_MASK);

            for (uint64_t k = 0; k < ENTRIES_PER_TABLE; k++) {
                uint64_t pde = pd[k];
                if (!(pde & PTE_PRESENT)) {
                    continue;
                }
                if (pde & PTE_PS) {
                    panic("vmm_for_each_user_page: unexpected 2MiB page in a process-private mapping");
                }
                uint64_t *pt = (uint64_t *)(uintptr_t)(pde & PTE_ADDR_MASK);

                for (uint64_t l = 0; l < ENTRIES_PER_TABLE; l++) {
                    uint64_t pte = pt[l];
                    if (!(pte & PTE_PRESENT)) {
                        continue;
                    }
                    uint64_t va = (i << 39) | (j << 30) | (k << 21) | (l << 12);
                    uint64_t phys = pte & PTE_ADDR_MASK;
                    uint64_t flags = pte & (VMM_FLAG_WRITABLE | VMM_FLAG_USER | VMM_FLAG_NX | VMM_FLAG_OWNED);
                    visitor(va, phys, flags, ctx);
                }
            }
        }
    }
}

bool vmm_is_user_range(uint64_t addr, uint64_t length)
{
    if (length == 0) {
        return true; /* nothing to access */
    }
    if (addr + length < addr) {
        return false; /* overflow: a malicious/buggy caller wrapped the address space */
    }

    uint64_t start_page = addr & ~(uint64_t)0xfff;
    uint64_t end_page = (addr + length - 1) & ~(uint64_t)0xfff;

    for (uint64_t page = start_page; ; page += 0x1000) {
        if (!is_user_page(page)) {
            return false;
        }
        if (page == end_page) {
            return true;
        }
    }
}
