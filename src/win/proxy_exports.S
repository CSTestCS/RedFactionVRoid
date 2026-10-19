/*
 * Pass-through export trampolines. Each export jumps through rfv_real[index]. That slot
 * initially points at a resolver stub, so the real DLL is only loaded the first time the
 * function is called (never inside DllMain). The resolver preserves every argument
 * register, finds the real function and jumps to it; later calls jump straight there.
 */
#define PROXY(idx, dll, name)                                    \
    .globl rfv_##name;                                           \
    .def rfv_##name; .scl 2; .type 32; .endef;                   \
rfv_##name:                                                      \
    jmp *(rfv_real + 8 * idx)(%rip);                             \
rfv_resolve_##name:                                              \
    mov $(idx), %eax;                                            \
    jmp rfv_lazy;

    .text
    .p2align 4
#include "proxy_export_list.h"

/* eax = export index. Preserve rcx/rdx/r8/r9 and xmm0-3 (the Win64 argument registers). */
rfv_lazy:
    push %rcx
    push %rdx
    push %r8
    push %r9
    sub $0x68, %rsp
    movdqu %xmm0, 0x20(%rsp)
    movdqu %xmm1, 0x30(%rsp)
    movdqu %xmm2, 0x40(%rsp)
    movdqu %xmm3, 0x50(%rsp)
    mov %eax, %ecx
    call rfv_resolve_export
    movdqu 0x20(%rsp), %xmm0
    movdqu 0x30(%rsp), %xmm1
    movdqu 0x40(%rsp), %xmm2
    movdqu 0x50(%rsp), %xmm3
    add $0x68, %rsp
    pop %r9
    pop %r8
    pop %rdx
    pop %rcx
    jmp *%rax

#undef PROXY
#define PROXY(idx, dll, name) .quad rfv_resolve_##name;
    .data
    .p2align 3
    .globl rfv_real
rfv_real:
#include "proxy_export_list.h"
