#include "machook.h"
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach/mach.h>
#include <signal.h>
#include <sys/ucontext.h>

namespace fm8plus::mac {

const void* imageOf(const void* p) {
    Dl_info di{};
    return dladdr(p, &di) ? di.dli_fbase : nullptr;
}

int resolve(const void* image, const char* const* names, void** out, int count) {
    auto* h = (const mach_header_64*)image;
    if (!h || h->magic != MH_MAGIC_64) return 0;
    const segment_command_64 *text = nullptr, *link = nullptr;
    const symtab_command* st = nullptr;
    auto* lc = (const load_command*)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; ++i, lc = (const load_command*)((const char*)lc + lc->cmdsize)) {
        if (lc->cmd == LC_SEGMENT_64) {
            auto* s = (const segment_command_64*)lc;
            if (!strcmp(s->segname, SEG_TEXT)) text = s;
            if (!strcmp(s->segname, SEG_LINKEDIT)) link = s;
        } else if (lc->cmd == LC_SYMTAB) {
            st = (const symtab_command*)lc;
        }
    }
    if (!text || !link || !st) return 0;
    const intptr_t slide = (intptr_t)h - (intptr_t)text->vmaddr;
    const char* le = (const char*)(slide + link->vmaddr - link->fileoff);   // file offset -> memory
    auto* sym = (const nlist_64*)(le + st->symoff);
    const char* strs = le + st->stroff;
    int found = 0;
    for (int k = 0; k < count; ++k) out[k] = nullptr;
    for (uint32_t i = 0; i < st->nsyms && found < count; ++i) {
        if ((sym[i].n_type & N_STAB) || (sym[i].n_type & N_TYPE) != N_SECT) continue;
        const char* n = strs + sym[i].n_un.n_strx;
        for (int k = 0; k < count; ++k)
            if (!out[k] && !strcmp(n, names[k])) { out[k] = (void*)(slide + sym[i].n_value); ++found; break; }
    }
    return found;
}

// ---- hardware-breakpoint redirects -----------------------------------------------------------
// The CPU's debug registers stop a thread when it reaches an address, the kernel turns that into a
// SIGTRAP, and our handler moves the thread's program counter to the replacement before it
// resumes. No code page is touched, so code signing, the hardened runtime and arm64 page checks
// never see anything. Debug registers are per thread, so each audio thread arms itself.
namespace {
constexpr int kMaxRedirects = 4;   // x86_64 has four breakpoint registers, arm64 at least six
struct Redirect { uintptr_t from, to; };
Redirect g_redirects[kMaxRedirects];
int g_count = 0;
struct sigaction g_prevTrap;
thread_local int tl_armed = 0;   // how many of g_redirects this thread's registers hold

void onTrap(int sig, siginfo_t* si, void* ctx) {
    auto* uc = (ucontext_t*)ctx;
#if defined(__x86_64__)
    uint64_t& pc = uc->uc_mcontext->__ss.__rip;
#else
    uint64_t& pc = uc->uc_mcontext->__ss.__pc;
#endif
    for (int i = 0; i < g_count; ++i)
        if (pc == g_redirects[i].from) { pc = g_redirects[i].to; return; }
    // Not ours: whoever had SIGTRAP before us (a debugger's trap, a host's handler) gets it.
    if (g_prevTrap.sa_flags & SA_SIGINFO) { if (g_prevTrap.sa_sigaction) g_prevTrap.sa_sigaction(sig, si, ctx); }
    else if (g_prevTrap.sa_handler == SIG_DFL) { signal(SIGTRAP, SIG_DFL); raise(SIGTRAP); }
    else if (g_prevTrap.sa_handler != SIG_IGN) g_prevTrap.sa_handler(sig);
}
} // namespace

bool redirect(void* from, void* to) {
    if (!from || !to || g_count >= kMaxRedirects) return false;
    if (!g_count) {
        struct sigaction sa{};
        sa.sa_sigaction = &onTrap;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        if (sigaction(SIGTRAP, &sa, &g_prevTrap) != 0) return false;
    }
    g_redirects[g_count++] = {(uintptr_t)from, (uintptr_t)to};
    return true;
}

bool armThread() {
    if (tl_armed == g_count) return true;
#if defined(__x86_64__)
    x86_debug_state64_t ds{};
    uint64_t* dr[4] = {&ds.__dr0, &ds.__dr1, &ds.__dr2, &ds.__dr3};
    for (int i = 0; i < g_count; ++i) {
        *dr[i] = g_redirects[i].from;
        ds.__dr7 |= 1ull << (2 * i);   // local enable; type 00 (execute) and length 00
    }
    const bool ok = thread_set_state(mach_thread_self(), x86_DEBUG_STATE64, (thread_state_t)&ds, x86_DEBUG_STATE64_COUNT) == KERN_SUCCESS;
#else
    arm_debug_state64_t ds{};
    for (int i = 0; i < g_count; ++i) {
        ds.__bvr[i] = g_redirects[i].from;
        ds.__bcr[i] = 0x1e5;   // enabled, all four byte lanes, EL0 (user) only
    }
    const bool ok = thread_set_state(mach_thread_self(), ARM_DEBUG_STATE64, (thread_state_t)&ds, ARM_DEBUG_STATE64_COUNT) == KERN_SUCCESS;
#endif
    if (ok) tl_armed = g_count;
    return ok;
}

} // namespace fm8plus::mac
