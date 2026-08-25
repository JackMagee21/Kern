; Milestone 1: Boot -> "hello kernel" via serial.
;
; GRUB2 loads this image per the Multiboot2 spec and jumps to _start in
; 32-bit protected mode with paging disabled, A20 already enabled, and
; EAX/EBX holding the boot magic / multiboot info pointer (verified against
; the canonical multiboot2.h header: MULTIBOOT2_BOOTLOADER_MAGIC ==
; 0x36d76289, header magic == 0xe85250d6, end-tag type/flags/size == 0/0/8,
; https://raw.githubusercontent.com/rhboot/grub2/master/include/multiboot2.h).
;
; This file takes the CPU from 32-bit protected mode to 64-bit long mode by
; hand, per the Intel SDM Vol. 3A Sec. 9.8.5 "Initializing IA-32e Mode"
; sequence: enable PAE -> load CR3 -> set IA32_EFER.LME -> enable paging ->
; far jump to reload CS with a 64-bit (L=1) code descriptor.
;
; Page tables use 2MiB pages (PS bit) to avoid a 4th paging level for this
; milestone; a real VMM/frame allocator replaces this in later milestones
; (see /docs/roadmap.md and /docs/adr/0001-boot-protocol-and-long-mode-entry.md).
; The low 8MiB of physical memory is mapped twice: identity (VA=PA), and at
; VA = PA + KERNEL_VMA_OFFSET, matching boot/linker.ld's higher-half layout.

default abs

%define KERNEL_VMA_OFFSET 0xFFFFFFFF80000000
%define MB2_MAGIC          0xe85250d6
%define MB2_ARCH_I386      0
%define COM1_PORT          0x3f8

; ---------------------------------------------------------------------
; Multiboot2 header (must be 8-byte aligned and within the first 32768
; bytes of the image; it is emitted as the very first section by
; boot/linker.ld, so it lands within the first few hundred bytes).
; ---------------------------------------------------------------------
section .multiboot2
align 8
mb2_header_start:
    dd MB2_MAGIC
    dd MB2_ARCH_I386
    dd mb2_header_end - mb2_header_start
    dd -(MB2_MAGIC + MB2_ARCH_I386 + (mb2_header_end - mb2_header_start)) & 0xFFFFFFFF

    ; Milestone 23 (ADR 0023): framebuffer request tag, type=5, struct
    ; verified against the canonical GRUB header (rhboot/grub2's
    ; multiboot2.h, struct multiboot_header_tag_framebuffer -- same
    ; primary source ADR 0001 already used for this file's other
    ; Multiboot2 structures): { type:u16 flags:u16 size:u32 width:u32
    ; height:u32 depth:u32 } = 20 bytes. flags=1
    ; (MULTIBOOT_HEADER_TAG_OPTIONAL) -- if the bootloader genuinely
    ; can't satisfy this, boot must still proceed rather than fail
    ; outright; kernel/drivers/framebuffer.c panics with a clear message
    ; if no framebuffer tag comes back in the boot-info structure, which
    ; beats an opaque GRUB-side boot failure. Requesting 1024x768x32
    ; specifically (not 0/0/0 "don't care") for a deterministic,
    ; reproducible mode across boots -- the boot-info tag's OWN reported
    ; width/height/bpp are what the kernel actually trusts and uses,
    ; never this request verbatim, since a bootloader is free to
    ; substitute its own best match.
    align 8
fb_tag_start:
    dw 5                          ; MULTIBOOT_HEADER_TAG_FRAMEBUFFER
    dw 1                          ; MULTIBOOT_HEADER_TAG_OPTIONAL
    dd fb_tag_end - fb_tag_start  ; size = the WHOLE tag, type/flags/size fields included
    dd 1024                       ; width
    dd 768                        ; height
    dd 32                         ; depth (bits per pixel)
fb_tag_end:

    ; End tag: type=0, flags=0, size=8
    align 8
    dw 0
    dw 0
    dd 8
mb2_header_end:

; ---------------------------------------------------------------------
; Low, identity-VMA==LMA boot trampoline (paging is off when we start).
; ---------------------------------------------------------------------
section .boot.data
align 16
multiboot_magic:    dd 0
multiboot_info_ptr:  dd 0

msg_no_cpuid:    db "[PANIC] CPUID not supported", 0
msg_no_longmode: db "[PANIC] CPU does not support long mode (x86_64)", 0

align 16
gdt64:
    dq 0x0000000000000000      ; null descriptor
.code equ $ - gdt64
    dw 0x0000                  ; limit low (unused: limit checks are off in long mode)
    dw 0x0000                  ; base low
    db 0x00                    ; base mid
    db 10011010b                ; access: P=1 DPL=00 S=1 Type=1010 (exec, readable)
    db 00100000b                ; flags: G=0 D=0 L=1 AVL=0 | limit_high=0000
    db 0x00                    ; base high
.data equ $ - gdt64
    dw 0x0000
    dw 0x0000
    db 0x00
    db 10010010b                ; access: P=1 DPL=00 S=1 Type=0010 (data, writable)
    db 00000000b                ; flags: G=0 D=0 (L n/a for data) AVL=0 | limit_high=0000
    db 0x00
gdt64_end:

gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dq gdt64

CODE_SEL equ gdt64.code
DATA_SEL equ gdt64.data

section .boot.bss nobits align=16
boot_stack_bottom:
    resb 32768
boot_stack_top:

align 4096
pml4:      resb 4096
pdpt_low:  resb 4096
pdpt_high: resb 4096
pd_shared: resb 4096

section .boot.text
bits 32

global _start
extern kernel_main

_start:
    cli
    cld                          ; don't rely on loader-guaranteed DF=0 for the rep stosb below
    mov esp, boot_stack_top

    mov [multiboot_magic], eax
    mov [multiboot_info_ptr], ebx

    call check_cpuid
    call check_long_mode
    call setup_page_tables
    call enable_long_mode_paging

    lgdt [gdt64_ptr]
    jmp CODE_SEL:long_mode_entry

; Toggling EFLAGS.ID (bit 21) must stick if CPUID is supported.
check_cpuid:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .no_cpuid
    ret
.no_cpuid:
    mov esi, msg_no_cpuid
    jmp panic32

; CPUID leaf 0x80000001, EDX bit 29 (LM) indicates long-mode support.
; Leaf 0x80000000 must first report >= 0x80000001 available.
check_long_mode:
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_longmode
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_longmode
    ret
.no_longmode:
    mov esi, msg_no_longmode
    jmp panic32

; Identity-map and higher-half-map the low 8MiB using 2MiB pages.
; PML4[0]   -> pdpt_low  -> pd_shared   (identity: VA 0x0..0x800000)
; PML4[511] -> pdpt_high -> pd_shared   (higher half: VA KERNEL_VMA_OFFSET..+0x800000)
; Indices for KERNEL_VMA_OFFSET verified: (0xFFFFFFFF80000000>>39)&0x1FF=511,
; (>>30)&0x1FF=510, (>>21)&0x1FF=0.
setup_page_tables:
    ; zero the four tables (4096 bytes each = 4 * 1024 dwords)
    mov edi, pml4
    xor eax, eax
    mov ecx, 4096
    rep stosb

    mov edi, pdpt_low
    xor eax, eax
    mov ecx, 4096
    rep stosb

    mov edi, pdpt_high
    xor eax, eax
    mov ecx, 4096
    rep stosb

    mov edi, pd_shared
    xor eax, eax
    mov ecx, 4096
    rep stosb

    ; pml4[0] = pdpt_low | present | writable
    mov eax, pdpt_low
    or eax, 0x3
    mov [pml4 + 0*8], eax

    ; pml4[511] = pdpt_high | present | writable
    mov eax, pdpt_high
    or eax, 0x3
    mov [pml4 + 511*8], eax

    ; pdpt_low[0] = pd_shared | present | writable
    mov eax, pd_shared
    or eax, 0x3
    mov [pdpt_low + 0*8], eax

    ; pdpt_high[510] = pd_shared | present | writable
    mov eax, pd_shared
    or eax, 0x3
    mov [pdpt_high + 510*8], eax

    ; pd_shared[0..3] = 2MiB pages covering physical 0x0..0x800000,
    ; present | writable | PS(huge page)
    mov dword [pd_shared + 0*8], 0x00000000 | 0x83
    mov dword [pd_shared + 0*8 + 4], 0x0
    mov dword [pd_shared + 1*8], 0x00200000 | 0x83
    mov dword [pd_shared + 1*8 + 4], 0x0
    mov dword [pd_shared + 2*8], 0x00400000 | 0x83
    mov dword [pd_shared + 2*8 + 4], 0x0
    mov dword [pd_shared + 3*8], 0x00600000 | 0x83
    mov dword [pd_shared + 3*8 + 4], 0x0
    ret

; Intel SDM Vol. 3A Sec. 9.8.5: PAE -> CR3 -> EFER.LME -> CR0.PG.
enable_long_mode_paging:
    mov eax, cr4
    or eax, 1 << 5              ; CR4.PAE
    mov cr4, eax

    mov eax, pml4
    mov cr3, eax

    mov ecx, 0xC0000080         ; IA32_EFER
    rdmsr
    or eax, 1 << 8               ; LME
    wrmsr

    mov eax, cr0
    or eax, 1 << 31              ; PG
    mov cr0, eax
    ret

; Pre-long-mode panic path: raw 16550 UART write (no C runtime yet).
; Register map / bit layout: standard NS16450/16550 (COM1 base 0x3f8;
; +0 THR, +1 IER, +2 FCR, +3 LCR, +5 LSR bit5=THR-empty) -- the same
; register set kernel/drivers/serial.c documents and uses post-boot.
panic32:
    mov dx, COM1_PORT + 1
    xor al, al
    out dx, al                  ; disable UART interrupts

    mov dx, COM1_PORT + 3
    mov al, 0x80
    out dx, al                  ; enable DLAB
    mov dx, COM1_PORT + 0
    mov al, 0x03
    out dx, al                  ; divisor low: 38400 baud
    mov dx, COM1_PORT + 1
    xor al, al
    out dx, al                  ; divisor high
    mov dx, COM1_PORT + 3
    mov al, 0x03
    out dx, al                  ; 8N1, DLAB off

.putc_loop:
    mov al, [esi]
    test al, al
    jz .halt
    mov dx, COM1_PORT + 5
.wait_thre:
    in al, dx
    test al, 0x20
    jz .wait_thre
    mov dx, COM1_PORT
    mov al, [esi]
    out dx, al
    inc esi
    jmp .putc_loop
.halt:
    cli
.halt_loop:
    hlt
    jmp .halt_loop

bits 64
long_mode_entry:
    mov ax, DATA_SEL
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov rax, higher_half_entry
    jmp rax

; ---------------------------------------------------------------------
; From here on we run at the higher-half virtual base (.text, linked per
; boot/linker.ld); RSP still points into boot_stack_top, which stays
; reachable because pd_shared maps that physical memory at both VAs.
; ---------------------------------------------------------------------
section .text
bits 64

extern __bss_start
extern __bss_end

higher_half_entry:
    ; Zero .bss before any C code runs: GRUB only loads the file's real
    ; content, so nothing else guarantees this physical RAM starts zero
    ; (see boot/linker.ld's __bss_start/__bss_end comment).
    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi
    xor eax, eax
    rep stosb

    mov edi, [multiboot_magic]
    mov esi, [multiboot_info_ptr]
    and rsp, ~0xf                ; SysV ABI: RSP must be 16-byte aligned at `call`
    call kernel_main

.hang:
    cli
    hlt
    jmp .hang
