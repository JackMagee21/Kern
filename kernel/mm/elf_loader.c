#include <stdint.h>

#include "elf_loader.h"
#include "pmm.h"
#include "vmm.h"
#include "../../libk/elf.h"
#include "../panic.h"

bool elf_load(uint64_t pml4_phys, const uint8_t *image, uint64_t image_size, uint64_t *out_entry)
{
    const elf64_ehdr_t *ehdr;
    if (!elf64_validate(image, image_size, &ehdr)) {
        return false;
    }

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        elf64_phdr_t phdr;
        if (!elf64_get_phdr(image, image_size, ehdr, i, &phdr)) {
            return false; /* shouldn't happen: elf64_validate already bounds the table */
        }
        if (phdr.p_type != ELF64_PT_LOAD) {
            continue; /* e.g. PT_GNU_STACK -- nothing to map */
        }
        if (!elf64_validate_load_segment(&phdr, image_size)) {
            return false;
        }

        uint64_t flags = VMM_FLAG_USER | VMM_FLAG_OWNED;
        if (phdr.p_flags & ELF64_PF_W) {
            flags |= VMM_FLAG_WRITABLE;
        }
        if (!(phdr.p_flags & ELF64_PF_X)) {
            flags |= VMM_FLAG_NX;
        }

        for (uint64_t off = 0; off < phdr.p_memsz; off += PMM_FRAME_SIZE) {
            uint64_t frame = pmm_alloc_frame();
            if (frame == 0 || frame >= VMM_IDENTITY_WINDOW_LIMIT) {
                panic("elf_load: pmm exhausted or destination frame outside identity window");
            }

            /* Zero the whole frame first, THEN copy only the file-backed
               portion over it -- this is what makes .bss (memsz > filesz,
               the tail left unspecified by the file) come out zeroed
               without a separate code path: any byte beyond p_filesz
               within this segment just never gets overwritten below. */
            uint8_t *dst = (uint8_t *)(uintptr_t)frame;
            for (uint64_t b = 0; b < PMM_FRAME_SIZE; b++) {
                dst[b] = 0;
            }

            if (off < phdr.p_filesz) {
                uint64_t copy_len = phdr.p_filesz - off;
                if (copy_len > PMM_FRAME_SIZE) {
                    copy_len = PMM_FRAME_SIZE;
                }
                const uint8_t *src = image + phdr.p_offset + off;
                for (uint64_t b = 0; b < copy_len; b++) {
                    dst[b] = src[b];
                }
            }

            if (!vmm_map_page_in(pml4_phys, phdr.p_vaddr + off, frame, flags)) {
                panic("elf_load: vmm_map_page_in failed (overlapping PT_LOAD segments?)");
            }
        }
    }

    *out_entry = ehdr->e_entry;
    return true;
}
