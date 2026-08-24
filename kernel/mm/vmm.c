#include <stdint.h>

#include "vmm.h"
#include "pmm.h"
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

/* boot.asm's pd_shared covers physical 0-8MiB (ADR 0001) with 2MiB
   identity pages; new page-table frames this file allocates must land
   in that range to be directly writable via their own physical address
   as a pointer (no general physical-memory direct-map exists yet -- see
   ADR 0004 for why one wasn't built for this milestone). pmm_alloc_frame
   hands out the lowest-numbered free frame first and this runs early
   (heap_init, right after pmm_init), so in practice every table frame
   this allocates lands well inside this window; get_or_create_table
   panics instead of silently corrupting memory if that's ever violated. */
#define VMM_IDENTITY_WINDOW_LIMIT 0x800000ULL

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
