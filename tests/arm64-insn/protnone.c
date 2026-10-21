// Any access to a PROT_NONE guest page must raise SIGSEGV in the guest. iSH used
// to hand the emulator a pointer into the host PROT_NONE page backing it, so a
// read killed the whole app with a host SIGBUS (Bun reading musl mallocng's brk
// guard page with ldp q0,q1). Also checks that the vdso still works now that
// its pages need real protection bits.
// Guest-only:  gcc -O1 -o protnone protnone.c && ./protnone    expected: "protnone ok"
#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static sigjmp_buf env;
static volatile uintptr_t fault_addr;
static volatile int usr1_seen;

static void on_fault(int sig, siginfo_t *info, void *ctx) {
    (void) sig; (void) ctx;
    fault_addr = (uintptr_t) info->si_addr;
    siglongjmp(env, 1);
}

static void on_usr1(int sig) { (void) sig; usr1_seen = 1; }

enum { LDP_Q, LDRB, STRB };

static void touch(char *p, int how) {
    uint32_t v;
    switch (how) {
    case LDP_Q: __asm__ volatile("ldp q0, q1, [%0]" : : "r"(p) : "v0", "v1", "memory"); break;
    case LDRB: __asm__ volatile("ldrb %w0, [%1]" : "=r"(v) : "r"(p) : "memory"); break;
    case STRB: __asm__ volatile("strb wzr, [%0]" : : "r"(p) : "memory"); break;
    }
}

// 1 if the access faulted at p (and the process survived), 0 if it didn't
static int faults(char *p, int how) {
    fault_addr = 0;
    if (sigsetjmp(env, 1) == 0) {
        touch(p, how);
        return 0;
    }
    return fault_addr >= (uintptr_t) p && fault_addr < (uintptr_t) p + 32;
}

static int all_fault(char *p) {
    return faults(p, LDP_Q) && faults(p, LDRB) && faults(p, STRB);
}

int main(void) {
    struct sigaction sa = {0};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    int fail = 0;

    // a. fresh PROT_NONE page
    char *m = mmap(NULL, 65536, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED || !all_fault(m + 4096)) { printf("fresh PROT_NONE page: no fault\n"); fail = 1; }

    // b. RW page, used (so it is in the TLB), then mprotect(PROT_NONE)
    char *rw = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (rw == MAP_FAILED) return 1;
    memset(rw, 1, 65536);
    if (mprotect(rw + 4096, 4096, PROT_NONE) || !all_fault(rw + 4096)) {
        printf("mprotect(PROT_NONE): no fault\n"); fail = 1;
    }
    if (faults(rw, LDP_Q) || faults(rw + 8192, LDRB) || rw[0] != 1 || rw[8192] != 1) {
        printf("mprotect(PROT_NONE): neighbours broke\n"); fail = 1;
    }

    // c. musl mallocng's guard page right at the start of the brk area
    uintptr_t b = (syscall(SYS_brk, 0) + 4095) & ~(uintptr_t) 4095;
    if ((uintptr_t) syscall(SYS_brk, b + 8192) >= b + 8192) {
        if (mmap((void *) b, 4096, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED ||
                !all_fault((char *) b)) {
            printf("brk guard page: no fault\n"); fail = 1;
        }
        char *after = (char *) b + 4096;
        if (faults(after, STRB) || faults(after, LDP_Q)) { printf("brk page after the guard faulted\n"); fail = 1; }
    } else {
        printf("brk failed\n"); fail = 1;
    }

    // d. untouched pages of a 256 MB PROT_NONE reservation (iSH maps these lazily)
    size_t size = 256u << 20;
    char *r = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r == MAP_FAILED || !all_fault(r) || !all_fault(r + size / 2) || !all_fault(r + size - 4096)) {
        printf("256 MB PROT_NONE reservation: no fault\n"); fail = 1;
    }
    // and committing part of it still works
    if (r != MAP_FAILED) {
        if (mprotect(r + size / 2, 4096, PROT_READ | PROT_WRITE)) { printf("mprotect in reservation failed\n"); fail = 1; }
        else if (faults(r + size / 2, STRB) || faults(r + size / 2, LDP_Q) || !all_fault(r + size / 2 + 4096)) {
            printf("committed reservation page wrong\n"); fail = 1;
        }
    }

    // e. the kernel's copy from a PROT_NONE page is EFAULT, not a crash
    int pfd[2];
    if (pipe(pfd) == 0) {
        errno = 0;
        if (m == MAP_FAILED || write(pfd[1], m + 4096, 16) != -1 || errno != EFAULT) {
            printf("write() from PROT_NONE: not EFAULT\n"); fail = 1;
        }
        close(pfd[0]); close(pfd[1]);
    }

    // f. vdso / vvar still usable, signal return (through the vdso) still works
    const char *vdso = (const char *) getauxval(AT_SYSINFO_EHDR);
    if (vdso != NULL && memcmp(vdso, "\177ELF", 4) != 0) { printf("vdso header unreadable\n"); fail = 1; }
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) { printf("clock_gettime failed\n"); fail = 1; }
    signal(SIGUSR1, on_usr1);
    raise(SIGUSR1);
    if (!usr1_seen) { printf("SIGUSR1 handler did not run\n"); fail = 1; }

    if (fail) { printf("protnone FAIL\n"); return 1; }
    printf("protnone ok\n");
    return 0;
}
