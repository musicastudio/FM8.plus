// Self-test for the hardware-breakpoint redirect in src/mac/machook.cpp, with no FM8 needed, so it
// runs anywhere, including GitHub's Apple Silicon runners. CI runs it twice: as built, and signed
// with the hardened runtime the way DAWs ship.
//
//     hwbp_test        exit 0 when an armed thread is redirected and an unarmed one is not
#include "../../src/mac/machook.h"
#include <cstdio>
#include <thread>

namespace {
volatile int g_bias = 1;   // keeps the calls below from being folded or reused by the compiler
__attribute__((noinline)) int target(int x) { return x + g_bias; }
__attribute__((noinline)) int replacement(int x) { return x * 100 + g_bias - 1; }
}

int main() {
    using namespace fm8plus::mac;
#if defined(__aarch64__)
    printf("arch arm64\n");
#else
    printf("arch x86_64\n");
#endif
    const bool registered = redirect((void*)&target, (void*)&replacement);
    const bool armed = armThread();
    const int onArmed = target(2);
    int onOther = 0;
    std::thread([&] { onOther = target(2); }).join();   // never armed: must run the real target
    const int again = target(3);
    printf("redirect registered %d, thread armed %d\n", registered, armed);
    printf("armed thread: target(2) = %d (want 200), target(3) = %d (want 300)\n", onArmed, again);
    printf("other thread: target(2) = %d (want 3)\n", onOther);
    const bool ok = registered && armed && onArmed == 200 && again == 300 && onOther == 3;
    printf("RESULT: %s\n", ok ? "hardware-breakpoint redirect works" : "FAILED");
    return ok ? 0 : 1;
}
