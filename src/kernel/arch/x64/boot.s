.intel_syntax noprefix

.section .text.boot
.code32
.align 4
multiboot_header:
    .long 0x1BADB002             /* Magic number */
    .long 0x00000007             /* Flags: ALIGN + MEMINFO + GRAPHICS */
    .long -(0x1BADB002 + 0x00000007) /* Checksum */
    
    /* AOUT kludge fields (padding required to reach graphics fields) */
    .long 0
    .long 0
    .long 0
    .long 0
    .long 0
    
    /* Graphics fields */
    .long 0                      /* mode_type: 0 = linear graphics */
    .long 1024                   /* width */
    .long 768                    /* height */
    .long 32                     /* depth */

.global _start
_start:
    cli

    /* Initialize stack for early booting */
    lea esp, early_stack_top

    /* Setup page tables */
    /* Point PML4[0] to PDPT */
    lea eax, boot_pdpt
    or eax, 3 /* Present | RW */
    mov [boot_pml4], eax

    /* Point PDPT[0] to PD0 */
    lea eax, boot_pd0
    or eax, 3 /* Present | RW */
    mov [boot_pdpt], eax

    /* Point PDPT[1] to PD1 */
    lea eax, boot_pd1
    or eax, 3 /* Present | RW */
    mov [boot_pdpt + 8], eax

    /* Map PD0 (0 - 1GB) with 2MB huge pages */
    mov ecx, 0
map_pd0:
    mov eax, ecx
    shl eax, 21 /* ecx * 2MB */
    or eax, 0x83 /* Present | RW | Huge Page */
    mov [boot_pd0 + ecx * 8], eax
    inc ecx
    cmp ecx, 512
    jne map_pd0

    /* Map PD1 (1 - 2GB) with 2MB huge pages */
    mov ecx, 0
map_pd1:
    mov eax, ecx
    shl eax, 21 /* ecx * 2MB */
    add eax, 0x40000000 /* Offset by 1GB */
    or eax, 0x83 /* Present | RW | Huge Page */
    mov [boot_pd1 + ecx * 8], eax
    inc ecx
    cmp ecx, 512
    jne map_pd1

    /* Load CR3 */
    lea eax, boot_pml4
    mov cr3, eax

    /* Enable PAE */
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    /* Enable Long Mode (EFER.LME) */
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    /* Enable Paging */
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    /* Load main GDT */
    .att_syntax prefix
    lgdt gdt_desc
    .intel_syntax noprefix

    /* Far jump to 64-bit code segment */
    .att_syntax prefix
    ljmp $0x08, $long_mode_entry
    .intel_syntax noprefix

.code64
long_mode_entry:
    /* Load data segment registers */
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    /* Setup stack pointer */
    lea rsp, __stack_top

    /* Clear BSS */
    lea rdi, __bss_start
    lea rcx, __bss_end
    sub rcx, rdi
    xor al, al
    rep stosb

    /* Call C main */
    call main

    /* If main returns, halt */
    call halt

.global halt
halt:
    /* Try QEMU ACPI shutdown */
    mov ax, 0x2000
    mov dx, 0x604
    out dx, ax
    
    /* Try QEMU older ACPI shutdown */
    mov ax, 0x2000
    mov dx, 0xB004
    out dx, ax
    
    /* Try Debug Exit (port 0x501) */
    mov al, 0x0
    mov dx, 0x501
    out dx, al
    
    /* Fallback */
halt_fallback:
    cli
    hlt
    jmp halt_fallback

.code16
.global trampoline_start
trampoline_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    /* Load 16-bit GDT */
    .att_syntax prefix
    lgdt trampoline_gdtr - trampoline_start + 0x7000
    .intel_syntax noprefix

    /* Enable protected mode (CR0.PE = 1) */
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    /* Far jump into 32-bit protected mode */
    .att_syntax prefix
    ljmpl $0x08, $trampoline_pm
    .intel_syntax noprefix

.align 4
trampoline_gdt:
    .quad 0x0000000000000000 /* Null */
    .quad 0x00cf9a000000ffff /* 32-bit Code (Kernel) */
    .quad 0x00cf92000000ffff /* 32-bit Data (Kernel) */
trampoline_gdt_end:

trampoline_gdtr:
    .word trampoline_gdt_end - trampoline_gdt - 1
    .long trampoline_gdt - trampoline_start + 0x7000

.code32
.align 4
trampoline_pm:
    /* Set segment registers */
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    /* Load CR3 (boot_pml4) */
    lea eax, boot_pml4
    mov cr3, eax

    /* Enable PAE */
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    /* Enable Long Mode */
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    /* Enable Paging */
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    /* Load the main 64-bit GDT */
    .att_syntax prefix
    lgdt gdt_desc
    .intel_syntax noprefix

    /* Far jump to 64-bit long mode */
    .att_syntax prefix
    ljmp $0x08, $trampoline_lm
    .intel_syntax noprefix

.code64
.align 8
trampoline_lm:
    /* Set segment registers */
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    /* Get our stack pointer from the global variable */
    mov rsp, [smp_temp_stack]

    /* Get our CPU ID from the global variable and pass it as the first argument (rdi) */
    mov rdi, [smp_temp_cpu]

    /* Call secondary_main */
    call secondary_main

trampoline_halt:
    cli
    hlt
    jmp trampoline_halt

.global trampoline_end
trampoline_end:

.section .data
.align 16
.global gdt_start
gdt_start:
    .quad 0x0000000000000000 /* Null descriptor */
    .quad 0x00209a0000000000 /* Kernel Code (64-bit, CS=0x08) */
    .quad 0x0000920000000000 /* Kernel Data (DS/SS=0x10) */
    .quad 0x0020fa0000000000 /* User Code (64-bit, CS=0x1b) */
    .quad 0x0000f20000000000 /* User Data (DS/SS=0x23) */
    /* Core 0 TSS (16 bytes, index 5, 6) */
    .quad 0x0000000000000000
    .quad 0x0000000000000000
    /* Core 1 TSS (16 bytes, index 7, 8) */
    .quad 0x0000000000000000
    .quad 0x0000000000000000
    /* Core 2 TSS (16 bytes, index 9, 10) */
    .quad 0x0000000000000000
    .quad 0x0000000000000000
    /* Core 3 TSS (16 bytes, index 11, 12) */
    .quad 0x0000000000000000
    .quad 0x0000000000000000
gdt_end:

gdt_desc:
    .word gdt_end - gdt_start - 1
    .quad gdt_start

.global smp_temp_stack
smp_temp_stack:
    .quad 0

.global smp_temp_cpu
smp_temp_cpu:
    .quad 0

.align 4096
.global boot_pml4
boot_pml4:
    .zero 4096
.global boot_pdpt
boot_pdpt:
    .zero 4096
.global boot_pd0
boot_pd0:
    .zero 4096
.global boot_pd1
boot_pd1:
    .zero 4096

/* Stack for early 32-bit booting before paging is enabled */
.align 16
early_stack:
    .zero 4096
early_stack_top:
