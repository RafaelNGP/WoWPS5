#pragma once
/*
 * ps4_ksignal.h, PS5 edition. The payload SDK's signal headers are FreeBSD's,
 * matching the kernel, so the helpers call sigaction/sigaltstack directly with
 * the native structures. The PS4_* names keep the FreeBSD values.
 */
#include <stddef.h>
#include <signal.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PS4_SA_ONSTACK   = SA_ONSTACK,
    PS4_SA_RESTART   = SA_RESTART,
    PS4_SA_RESETHAND = SA_RESETHAND,
    PS4_SA_NODEFER   = SA_NODEFER,
    PS4_SA_SIGINFO   = SA_SIGINFO,
    PS4_SS_ONSTACK   = SS_ONSTACK,
    PS4_SS_DISABLE   = SS_DISABLE
};

typedef void (*Ps4SigactionHandler)(int, siginfo_t*, void*);

static inline int ps4KernelSigaltstack(void* base, size_t size) {
    stack_t st;
    memset(&st, 0, sizeof(st));
    st.ss_sp = base;
    st.ss_size = size;
    return sigaltstack(&st, (stack_t*)0);
}

static inline int ps4KernelSigaction(int sig, Ps4SigactionHandler handler, int extraFlags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | extraFlags;
    return sigaction(sig, &sa, (struct sigaction*)0);
}

#ifdef __cplusplus
}
#endif
