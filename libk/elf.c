#include "elf.h"

bool elf64_validate(const uint8_t *image, uint64_t image_size, const elf64_ehdr_t **out_ehdr)
{
    if (image_size < sizeof(elf64_ehdr_t)) {
        return false;
    }

    const elf64_ehdr_t *ehdr = (const elf64_ehdr_t *)(const void *)image;

    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E'
        || ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F') {
        return false;
    }
    if (ehdr->e_ident[4] != ELF64_CLASS64 || ehdr->e_ident[5] != ELF64_DATA2LSB) {
        return false;
    }
    if (ehdr->e_type != ELF64_ET_EXEC || ehdr->e_machine != ELF64_EM_X86_64) {
        return false;
    }
    if (ehdr->e_phentsize != sizeof(elf64_phdr_t)) {
        return false;
    }
    if (ehdr->e_phnum == 0) {
        return false;
    }

    /* Overflow-safe bounds check: compute the table's byte length first
       (phentsize is a uint16_t and phnum is a uint16_t, so their
       product fits in uint64_t with no overflow), then check
       e_phoff <= image_size AND the remaining room covers the table --
       rather than "e_phoff + table_len <= image_size" directly, which
       could wrap past UINT64_MAX for a maliciously large e_phoff and
       falsely pass. */
    uint64_t table_len = (uint64_t)ehdr->e_phentsize * (uint64_t)ehdr->e_phnum;
    if (ehdr->e_phoff > image_size) {
        return false;
    }
    if (table_len > image_size - ehdr->e_phoff) {
        return false;
    }

    *out_ehdr = ehdr;
    return true;
}

bool elf64_get_phdr(const uint8_t *image, uint64_t image_size, const elf64_ehdr_t *ehdr, uint16_t index, elf64_phdr_t *out_phdr)
{
    uint64_t offset = ehdr->e_phoff + (uint64_t)index * (uint64_t)ehdr->e_phentsize;
    if (offset > image_size || sizeof(elf64_phdr_t) > image_size - offset) {
        return false;
    }

    const elf64_phdr_t *src = (const elf64_phdr_t *)(const void *)(image + offset);
    *out_phdr = *src;
    return true;
}

bool elf64_validate_load_segment(const elf64_phdr_t *phdr, uint64_t image_size)
{
    if (phdr->p_memsz == 0) {
        return false;
    }
    if (phdr->p_filesz > phdr->p_memsz) {
        return false;
    }
    if (phdr->p_offset > image_size) {
        return false;
    }
    if (phdr->p_filesz > image_size - phdr->p_offset) {
        return false;
    }
    if ((phdr->p_vaddr & 0xfffULL) != 0) {
        return false;
    }
    /* p_vaddr + p_memsz must not overflow the address space -- a
       maliciously huge p_memsz paired with a high p_vaddr could wrap. */
    if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr) {
        return false;
    }

    return true;
}
