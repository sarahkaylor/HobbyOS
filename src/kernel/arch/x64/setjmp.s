.intel_syntax noprefix

.section .text
.global setjmp
.global longjmp

/* int setjmp(jmp_buf env) */
/* rdi = env */
setjmp:
    mov [rdi + 0], rbx
    mov [rdi + 8], rbp
    mov [rdi + 16], r12
    mov [rdi + 24], r13
    mov [rdi + 32], r14
    mov [rdi + 40], r15
    
    /* Save RSP after return (caller's stack pointer) */
    lea rax, [rsp + 8]
    mov [rdi + 48], rax
    
    /* Save return address */
    mov rax, [rsp]
    mov [rdi + 56], rax
    
    xor rax, rax
    ret

/* void longjmp(jmp_buf env, int val) */
/* rdi = env, rsi = val */
longjmp:
    mov rbx, [rdi + 0]
    mov rbp, [rdi + 8]
    mov r12, [rdi + 16]
    mov r13, [rdi + 24]
    mov r14, [rdi + 32]
    mov r15, [rdi + 40]
    
    /* Restore RSP and RIP */
    mov r8, [rdi + 48]
    mov r9, [rdi + 56]
    mov rsp, r8
    sub rsp, 8
    mov [rsp], r9
    
    /* Set return value to val (rsi), if val is 0, return 1 */
    mov rax, rsi
    test rax, rax
    jnz .L_ret_val
    mov rax, 1
.L_ret_val:
    ret
