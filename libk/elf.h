#ifndef LIBK_ELF_H
#define LIBK_ELF_H

#include <stdbool.h>
#include <stdint.h>

/* ELF64 header/program-header layouts and the constants below are the
   System V ABI / ELF-64 spec's fixed encoding (e_ident magic/class/
   data, PT_LOAD, PF_X/PF_W/PF_R) -- decades-stable, the same format
   every ELF64 toolchain (binutils, LLVM) already produces and consumes,
   not guessed. Pure parsing/validation only, no allocation, no kernel
   dependency -- host-testable (tests/host/test_elf.c) per CLAUDE.md's
   "factor host-testable logic out of kernel-only code" guidance;
   kernel/mm/elf_loader.c is the thin kernel-only layer that actually
   maps memory using what this validates.

   Every function here is a parser boundary per CLAUDE.md's security
   rule ("validate all sizes/offsets in any parser... before use") --
   image_size is always the trust boundary; nothing here ever reads
   past it, including guarding against integer overflow in offset+size
   arithmetic rather than just checking the sum naively. */

#define ELF64_CLASS64   2
#define ELF64_DATA2LSB  1
#define ELF64_ET_EXEC   2
#define ELF64_EM_X86_64 62

#define ELF64_PT_LOAD 1

#define ELF64_PF_X 1
#define ELF64_PF_W 2
#define ELF64_PF_R 4

typedef struct __attribute__((packed)) {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;

typedef struct __attribute__((packed)) {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

/* Validates: e_ident magic (0x7f 'E' 'L' 'F') + ELFCLASS64 + little-
   endian, e_type == ET_EXEC (this loader only ever loads static
   executables -- no PT_INTERP/dynamic-linking support exists), e_machine
   == EM_X86_64, and that the program-header table
   [e_phoff, e_phoff + e_phnum*e_phentsize) fits entirely within
   [0, image_size) with no overflow-based bypass of that check.
   e_phentsize must equal sizeof(elf64_phdr_t) exactly -- this parser
   only knows how to read that one fixed layout.

   On success, sets *out_ehdr to point directly at the image's own bytes
   (not a copy -- the image must stay alive and unmodified for as long
   as *out_ehdr or any elf64_get_phdr() result derived from it is used).
   Returns false and leaves *out_ehdr unchanged on any validation
   failure. image_size must be >= sizeof(elf64_ehdr_t) or this returns
   false immediately. */
bool elf64_validate(const uint8_t *image, uint64_t image_size, const elf64_ehdr_t **out_ehdr);

/* Bounds-checked read of program header `index` (caller must ensure
   index < ehdr->e_phnum, already established in-range by
   elf64_validate's table-bounds check). Copies the header out to
   *out_phdr (a plain struct copy -- safe regardless of the image
   buffer's alignment, since the source is byte-exact packed). Returns
   false only if ehdr/index somehow describe a read outside
   [0, image_size) (defensive -- elf64_validate already guarantees this
   for every index < e_phnum, so this should never actually fail when
   called correctly, but a parser never trusts its own caller either). */
bool elf64_get_phdr(const uint8_t *image, uint64_t image_size, const elf64_ehdr_t *ehdr, uint16_t index, elf64_phdr_t *out_phdr);

/* For a program header already known to have p_type == ELF64_PT_LOAD:
   validates p_offset+p_filesz fits within [0, image_size) with no
   overflow, p_filesz <= p_memsz (a segment can't copy more file bytes
   than its own memory footprint), p_memsz != 0 (an empty PT_LOAD is
   nonsensical and would map zero pages), and p_vaddr is 4KiB-aligned.
   The alignment check is this loader's own simplifying assumption (see
   kernel/mm/elf_loader.c's doc comment for why), NOT part of the ELF
   spec itself -- a general-purpose loader would need to handle
   sub-page-aligned segments too. */
bool elf64_validate_load_segment(const elf64_phdr_t *phdr, uint64_t image_size);

#endif /* LIBK_ELF_H */
