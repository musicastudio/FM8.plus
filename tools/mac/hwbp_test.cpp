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
long inserted(long a, long b, long c, long d) { return a * 1000 + b * 100 + c * 10 + d; }
}

// The shape of FM8's arp dispatch for redirectCall: values parked in callee-saved registers, a
// trap on an instruction mid-function, a resume point after it. Unarmed it returns 7; armed, the
// trap calls inserted(a, b, c, d) from the registers and its result comes back through `resume`.
extern "C" long cs_target(long a, long b, long c, long d);
extern "C" char cs_trap[], cs_resume[];
#if defined(__x86_64__)
asm(R"(
    .text
    .intel_syntax noprefix
    .globl _cs_target, _cs_trap, _cs_resume
_cs_target:
    push rbp
    mov rbp, rsp
    push r15
    push r14
    push r12
    push rbx
    mov r15, rdi
    mov r12, rsi
    mov r14, rdx
    mov rbx, rcx
_cs_trap:
    mov eax, 7
_cs_resume:
    pop rbx
    pop r12
    pop r14
    pop r15
    pop rbp
    ret
    .att_syntax
)");
// The registers FM8's x86_64 dispatch keeps its four values in, by thread-state number.
const unsigned char kArgs[4] = {15, 12, 14, 1};
#else
asm(R"(
    .text
    .globl _cs_target, _cs_trap, _cs_resume
    .p2align 2
_cs_target:
    stp x22, x21, [sp, #-48]!
    stp x20, x19, [sp, #16]
    stp x29, x30, [sp, #32]
    add x29, sp, #32
    mov x20, x0
    mov x0, x1
    mov x19, x2
    mov x21, x3
_cs_trap:
    mov x0, #7
_cs_resume:
    ldp x29, x30, [sp, #32]
    ldp x20, x19, [sp, #16]
    ldp x22, x21, [sp], #48
    ret
)");
const unsigned char kArgs[4] = {20, 0, 19, 21};   // FM8's arm64 dispatch
#endif

int main() {
    using namespace fm8plus::mac;
#if defined(__aarch64__)
    printf("arch arm64\n");
#else
    printf("arch x86_64\n");
#endif
    const bool registered = redirect((void*)&target, (void*)&replacement)
                         && redirectCall(cs_trap, (void*)&inserted, cs_resume, kArgs);
    const bool armed = armThread();
    const int onArmed = target(2);
    const long called = cs_target(1, 2, 3, 4);
    int onOther = 0;
    long calledOther = 0;
    std::thread([&] { onOther = target(2); calledOther = cs_target(1, 2, 3, 4); }).join();   // never armed
    const int again = target(3);
    printf("redirect registered %d, thread armed %d\n", registered, armed);
    printf("armed thread: target(2) = %d (want 200), target(3) = %d (want 300)\n", onArmed, again);
    printf("armed thread: mid-function call = %ld (want 1234)\n", called);
    printf("other thread: target(2) = %d (want 3), mid-function = %ld (want 7)\n", onOther, calledOther);
    const bool ok = registered && armed && onArmed == 200 && again == 300 && called == 1234
                 && onOther == 3 && calledOther == 7;
    printf("RESULT: %s\n", ok ? "hardware-breakpoint redirect works" : "FAILED");
    return ok ? 0 : 1;
}
