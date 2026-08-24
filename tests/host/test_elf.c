/* Host-compiled unit test for libk/elf.c (see CLAUDE.md "Testing").
   Build/run directly, e.g.:
     gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
         -I../.. test_elf.c ../../libk/elf.c -o test_elf && ./test_elf
*/
#include <stdio.h>
#include <string.h>

#include "../../libk/elf.h"

static int checks_failed = 0;

#define EXPECT(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
            checks_failed++; \
        } \
    } while (0)

/* Hand-builds a minimal, otherwise-valid ELF64 static executable image
   in `buf`: one ehdr, two phdrs (a PT_LOAD text segment and a
   non-PT_LOAD entry that a real loader would skip), followed by 16
   bytes of "file content" the text segment's p_filesz covers. Returns
   the total image size. Every field is set by hand, byte-exact,
   mirroring the real System V ABI layout -- not derived from the
   library under test, so this is an independent check. */
static uint64_t build_valid_image(uint8_t *buf, uint64_t buf_size)
{
    memset(buf, 0, buf_size);

    elf64_ehdr_t ehdr = {0};
    ehdr.e_ident[0] = 0x7f;
    ehdr.e_ident[1] = 'E';
    ehdr.e_ident[2] = 'L';
    ehdr.e_ident[3] = 'F';
    ehdr.e_ident[4] = ELF64_CLASS64;
    ehdr.e_ident[5] = ELF64_DATA2LSB;
    ehdr.e_type = ELF64_ET_EXEC;
    ehdr.e_machine = ELF64_EM_X86_64;
    ehdr.e_entry = 0x8000400000ULL;
    ehdr.e_phoff = sizeof(elf64_ehdr_t);
    ehdr.e_phentsize = sizeof(elf64_phdr_t);
    ehdr.e_phnum = 2;

    uint64_t phdr_table_off = sizeof(elf64_ehdr_t);
    uint64_t payload_off = phdr_table_off + 2 * sizeof(elf64_phdr_t);

    elf64_phdr_t load = {0};
    load.p_type = ELF64_PT_LOAD;
    load.p_flags = ELF64_PF_R | ELF64_PF_X;
    load.p_offset = payload_off;
    load.p_vaddr = 0x8000400000ULL; /* 4KiB-aligned */
    load.p_filesz = 16;
    load.p_memsz = 16;
    load.p_align = 0x1000;

    elf64_phdr_t other = {0};
    other.p_type = 0x6474e551u; /* PT_GNU_STACK -- a real loader skips non-PT_LOAD entries */

    memcpy(buf, &ehdr, sizeof(ehdr));
    memcpy(buf + phdr_table_off, &load, sizeof(load));
    memcpy(buf + phdr_table_off + sizeof(elf64_phdr_t), &other, sizeof(other));
    for (uint64_t i = 0; i < 16; i++) {
        buf[payload_off + i] = (uint8_t)(0xa0 + i);
    }

    return payload_off + 16;
}

static void test_valid_image_parses(void)
{
    uint8_t buf[512];
    uint64_t size = build_valid_image(buf, sizeof(buf));

    const elf64_ehdr_t *ehdr;
    EXPECT(elf64_validate(buf, size, &ehdr), "valid image should validate");
    EXPECT(ehdr->e_phnum == 2, "phnum should be 2");
    EXPECT(ehdr->e_entry == 0x8000400000ULL, "entry point should round-trip");

    elf64_phdr_t phdr0, phdr1;
    EXPECT(elf64_get_phdr(buf, size, ehdr, 0, &phdr0), "phdr 0 should be readable");
    EXPECT(phdr0.p_type == ELF64_PT_LOAD, "phdr 0 should be PT_LOAD");
    EXPECT(elf64_validate_load_segment(&phdr0, size), "phdr 0 should be a valid load segment");

    EXPECT(elf64_get_phdr(buf, size, ehdr, 1, &phdr1), "phdr 1 should be readable");
    EXPECT(phdr1.p_type != ELF64_PT_LOAD, "phdr 1 should not be PT_LOAD (skipped by a real loader)");

    /* Out-of-range index must fail, not read past the table. */
    elf64_phdr_t phdr_oob;
    EXPECT(!elf64_get_phdr(buf, size, ehdr, 2, &phdr_oob), "phdr index 2 is out of range and must fail");
}

static void test_bad_magic_rejected(void)
{
    uint8_t buf[512];
    uint64_t size = build_valid_image(buf, sizeof(buf));
    buf[0] = 0x00; /* corrupt the magic */

    const elf64_ehdr_t *ehdr;
    EXPECT(!elf64_validate(buf, size, &ehdr), "bad magic must be rejected");
}

static void test_wrong_class_and_machine_rejected(void)
{
    uint8_t buf[512];
    uint64_t size = build_valid_image(buf, sizeof(buf));

    buf[4] = 1; /* ELFCLASS32, not 64 */
    const elf64_ehdr_t *ehdr;
    EXPECT(!elf64_validate(buf, size, &ehdr), "32-bit class must be rejected");

    size = build_valid_image(buf, sizeof(buf));
    elf64_ehdr_t *h = (elf64_ehdr_t *)(void *)buf;
    h->e_machine = 3; /* EM_386, not x86_64 */
    EXPECT(!elf64_validate(buf, size, &ehdr), "wrong machine must be rejected");
}

static void test_truncated_phdr_table_rejected(void)
{
    uint8_t buf[512];
    uint64_t size = build_valid_image(buf, sizeof(buf));

    /* Claim more program headers than actually fit before image_size. */
    elf64_ehdr_t *h = (elf64_ehdr_t *)(void *)buf;
    h->e_phnum = 5000;

    const elf64_ehdr_t *ehdr;
    EXPECT(!elf64_validate(buf, size, &ehdr), "phdr table exceeding image_size must be rejected");
}

static void test_phoff_overflow_rejected(void)
{
    uint8_t buf[512];
    uint64_t size = build_valid_image(buf, sizeof(buf));

    elf64_ehdr_t *h = (elf64_ehdr_t *)(void *)buf;
    h->e_phoff = 0xfffffffffffffff0ULL; /* would overflow phoff + table_len */

    const elf64_ehdr_t *ehdr;
    EXPECT(!elf64_validate(buf, size, &ehdr), "overflowing e_phoff must be rejected, not wrap and pass");
}

static void test_unaligned_vaddr_rejected(void)
{
    elf64_phdr_t phdr = {0};
    phdr.p_type = ELF64_PT_LOAD;
    phdr.p_offset = 0;
    phdr.p_vaddr = 0x8000400123ULL; /* not 4KiB-aligned */
    phdr.p_filesz = 16;
    phdr.p_memsz = 16;

    EXPECT(!elf64_validate_load_segment(&phdr, 1024), "unaligned p_vaddr must be rejected");
}

static void test_filesz_exceeds_memsz_rejected(void)
{
    elf64_phdr_t phdr = {0};
    phdr.p_type = ELF64_PT_LOAD;
    phdr.p_offset = 0;
    phdr.p_vaddr = 0x8000400000ULL;
    phdr.p_filesz = 100;
    phdr.p_memsz = 50;

    EXPECT(!elf64_validate_load_segment(&phdr, 1024), "p_filesz > p_memsz must be rejected");
}

static void test_offset_plus_filesz_exceeds_image_rejected(void)
{
    elf64_phdr_t phdr = {0};
    phdr.p_type = ELF64_PT_LOAD;
    phdr.p_offset = 1000;
    phdr.p_vaddr = 0x8000400000ULL;
    phdr.p_filesz = 100;
    phdr.p_memsz = 100;

    EXPECT(!elf64_validate_load_segment(&phdr, 1024), "p_offset+p_filesz beyond image_size must be rejected");
}

static void test_offset_plus_filesz_overflow_rejected(void)
{
    elf64_phdr_t phdr = {0};
    phdr.p_type = ELF64_PT_LOAD;
    phdr.p_offset = 0xfffffffffffffff0ULL;
    phdr.p_vaddr = 0x8000400000ULL;
    phdr.p_filesz = 100;
    phdr.p_memsz = 100;

    EXPECT(!elf64_validate_load_segment(&phdr, 1024), "overflowing p_offset+p_filesz must be rejected, not wrap and pass");
}

static void test_zero_memsz_rejected(void)
{
    elf64_phdr_t phdr = {0};
    phdr.p_type = ELF64_PT_LOAD;
    phdr.p_vaddr = 0x8000400000ULL;
    phdr.p_filesz = 0;
    phdr.p_memsz = 0;

    EXPECT(!elf64_validate_load_segment(&phdr, 1024), "zero p_memsz must be rejected");
}

int main(void)
{
    test_valid_image_parses();
    test_bad_magic_rejected();
    test_wrong_class_and_machine_rejected();
    test_truncated_phdr_table_rejected();
    test_phoff_overflow_rejected();
    test_unaligned_vaddr_rejected();
    test_filesz_exceeds_memsz_rejected();
    test_offset_plus_filesz_exceeds_image_rejected();
    test_offset_plus_filesz_overflow_rejected();
    test_zero_memsz_rejected();

    if (checks_failed != 0) {
        fprintf(stderr, "%d check(s) failed\n", checks_failed);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
