/*
 * Export trampolines for the dxgi.dll proxy. Every export jumps straight to the
 * matching function of the real dxgi.dll (resolved in dxgi_proxy.cpp), so callers'
 * arguments and return values pass through untouched whatever the signature is.
 */
#define FORWARD(name)              \
    .globl rfv_##name;             \
    .def rfv_##name; .scl 2; .type 32; .endef; \
rfv_##name:                        \
    jmp *g_real_##name(%rip);

    .text
    .p2align 4
#include "dxgi_export_list.h"
