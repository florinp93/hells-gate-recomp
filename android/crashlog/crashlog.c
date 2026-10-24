// Minimal native crash reporter: on a fatal signal, writes the signal, fault address and an
// unwound backtrace (module+offset) to fd 2 (redirected to the Download log), then re-raises.
#include <jni.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <dlfcn.h>
#include <unwind.h>
#include <ucontext.h>

#define MAXFRAMES 64
typedef struct { uintptr_t pcs[MAXFRAMES]; int n; } Bt;

static _Unwind_Reason_Code step(struct _Unwind_Context* c, void* arg) {
  Bt* b = (Bt*)arg;
  uintptr_t pc = _Unwind_GetIP(c);
  if (pc && b->n < MAXFRAMES) b->pcs[b->n++] = pc;
  return b->n >= MAXFRAMES ? _URC_END_OF_STACK : _URC_NO_REASON;
}

static void out(const char* s) { size_t l = strlen(s); (void)!write(2, s, l); }

static void handler(int sig, siginfo_t* si, void* uc_) {
  char buf[512];
  ucontext_t* uc = (ucontext_t*)uc_;
  snprintf(buf, sizeof buf,
           "\n=== NATIVE CRASH: signal %d code %d fault_addr %p ===\npc=%p sp=%p lr=%p\n", sig,
           si->si_code, si->si_addr, (void*)uc->uc_mcontext.pc, (void*)uc->uc_mcontext.sp,
           (void*)uc->uc_mcontext.regs[30]);
  out(buf);
  Bt bt = {{0}, 0};
  // Start from the faulting pc, then unwind the handler's own stack as best effort.
  bt.pcs[bt.n++] = (uintptr_t)uc->uc_mcontext.pc;
  bt.pcs[bt.n++] = (uintptr_t)uc->uc_mcontext.regs[30];
  _Unwind_Backtrace(step, &bt);
  for (int i = 0; i < bt.n; i++) {
    Dl_info d;
    if (dladdr((void*)bt.pcs[i], &d) && d.dli_fname) {
      snprintf(buf, sizeof buf, "  #%02d pc %p  %s+0x%lx  (%s)\n", i, (void*)bt.pcs[i], d.dli_fname,
               (unsigned long)(bt.pcs[i] - (uintptr_t)d.dli_fbase), d.dli_sname ? d.dli_sname : "?");
    } else {
      snprintf(buf, sizeof buf, "  #%02d pc %p\n", i, (void*)bt.pcs[i]);
    }
    out(buf);
  }
  out("=== END NATIVE CRASH ===\n");
  signal(sig, SIG_DFL);
  raise(sig);
}

JNIEXPORT void JNICALL Java_dev_hellsgate_android_DiagLog_installNative(JNIEnv* e, jclass c) {
  static char altstack[64 * 1024];
  stack_t ss = {.ss_sp = altstack, .ss_size = sizeof altstack, .ss_flags = 0};
  sigaltstack(&ss, 0);
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  int sigs[] = {SIGABRT, SIGBUS, SIGILL, SIGFPE, SIGSEGV, SIGTRAP};
  for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, 0);
}
