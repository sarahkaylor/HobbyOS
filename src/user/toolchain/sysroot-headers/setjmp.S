// HobbyOS sysroot compat: aarch64 setjmp/longjmp for the LLVM toolchain
// cross-build (lane W2-LLVM, wave 2).
//
// HobbyOS's userland libc has NO setjmp/longjmp bodies (only the kernel's
// src/kernel/arch/arm/setjmp.s, which this file derives from), and clang's
// CrashRecoveryContext.cpp pulls them at LINK time.  The OS setjmp.h also
// lacks `extern "C"` (C++ TUs mangle the refs), which `_missing.h` fixes by
// force-declaring them extern "C" before any header is seen.  L2 work item:
// graduate setjmp/longjmp into src/libc (+ extern "C" in src/include/
// setjmp.h).  AD-3 build-config only; no LLVM source patch.
.section .text
.global setjmp
.global longjmp

// int setjmp(jmp_buf env)
//   x0 = pointer to jmp_buf  (uint64_t[16], kernel setjmp.s layout)
setjmp:
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x1, sp
    str x1, [x0, #96]
    mov x0, #0
    ret

// void longjmp(jmp_buf env, int val)
//   x0 = pointer to jmp_buf
//   x1 = return value
longjmp:
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x30, [x0, #80]
    ldr x2, [x0, #96]
    mov sp, x2

    // Set return value to val (x1); if val is 0, return 1
    cmp w1, #0
    mov w2, #1
    csel w0, w1, w2, ne
    ret
