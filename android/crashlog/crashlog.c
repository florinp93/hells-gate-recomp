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
#include <malloc.h>

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
  // The GPU thread allocates and frees large buffers every frame; without decay
  // the allocator returns them to the OS each time (madvise + re-zeroing).
#ifdef M_DECAY_TIME
  int decay_ok = mallopt(M_DECAY_TIME, 1);
  char msg[96];
  snprintf(msg, sizeof msg, "mallopt(M_DECAY_TIME, 1) -> %d\n", decay_ok);
  out(msg);
#endif
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

// ---------------------------------------------------------------------------------------------
// Sampling profiler: interrupts one thread ~1000x/s with SIGPROF and records its pc and lr.
// Results are aggregated and written to fd 2 (the Download log) as module+offset lines, which are
// symbolized offline with the unstripped libraries.
#include <pthread.h>
#include <sys/syscall.h>
#include <time.h>
#include <stdlib.h>

#define PROF_MAX 20000
static volatile uintptr_t prof_pc[PROF_MAX];
static volatile uintptr_t prof_lr[PROF_MAX];
static volatile int prof_n;
static volatile int prof_tid;

static void prof_handler(int sig, siginfo_t* si, void* uc_) {
  (void)sig; (void)si;
  ucontext_t* uc = (ucontext_t*)uc_;
  int i = __atomic_fetch_add(&prof_n, 1, __ATOMIC_RELAXED);
  if (i < PROF_MAX) {
    prof_pc[i] = (uintptr_t)uc->uc_mcontext.pc;
    prof_lr[i] = (uintptr_t)uc->uc_mcontext.regs[30];
  }
}

typedef struct { uintptr_t key; int count; } Bucket;

static int cmp_bucket(const void* a, const void* b) {
  return ((const Bucket*)b)->count - ((const Bucket*)a)->count;
}

static void dump_hist(const char* title, volatile uintptr_t* arr, int n) {
  Bucket* b = (Bucket*)calloc((size_t)n + 1, sizeof(Bucket));
  int nb = 0;
  for (int i = 0; i < n; i++) {
    int j;
    for (j = 0; j < nb; j++) if (b[j].key == arr[i]) { b[j].count++; break; }
    if (j == nb) { b[nb].key = arr[i]; b[nb].count = 1; nb++; }
  }
  qsort(b, (size_t)nb, sizeof(Bucket), cmp_bucket);
  char buf[512];
  snprintf(buf, sizeof buf, "PROFILE %s: %d samples, %d distinct\n", title, n, nb);
  out(buf);
  for (int i = 0; i < nb && i < 400; i++) {
    Dl_info d;
    if (dladdr((void*)b[i].key, &d) && d.dli_fname) {
      const char* base = strrchr(d.dli_fname, '/');
      snprintf(buf, sizeof buf, "PROFILE %s %5d %s+0x%lx %s\n", title, b[i].count,
               base ? base + 1 : d.dli_fname, (unsigned long)(b[i].key - (uintptr_t)d.dli_fbase),
               d.dli_sname ? d.dli_sname : "");
    } else {
      snprintf(buf, sizeof buf, "PROFILE %s %5d %p\n", title, b[i].count, (void*)b[i].key);
    }
    out(buf);
  }
  free(b);
}

typedef struct { int tid; int ms; } ProfArgs;

static void* prof_thread(void* p) {
  ProfArgs a = *(ProfArgs*)p;
  free(p);
  prof_n = 0;
  prof_tid = a.tid;
  struct sigaction sa, old;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = prof_handler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGPROF, &sa, &old);
  pid_t pid = getpid();
  struct timespec ts = {0, 1000000};
  for (int t = 0; t < a.ms && prof_n < PROF_MAX; t++) {
    if (syscall(SYS_tgkill, pid, a.tid, SIGPROF) != 0) break;
    nanosleep(&ts, 0);
  }
  struct timespec settle = {0, 20000000};
  nanosleep(&settle, 0);
  sigaction(SIGPROF, &old, 0);
  int n = prof_n < PROF_MAX ? prof_n : PROF_MAX;
  char buf[128];
  snprintf(buf, sizeof buf, "PROFILE begin tid %d\n", a.tid);
  out(buf);
  dump_hist("pc", prof_pc, n);
  dump_hist("lr", prof_lr, n);
  out("PROFILE end\n");
  return 0;
}

JNIEXPORT void JNICALL Java_dev_hellsgate_android_DiagLog_profileThread(JNIEnv* e, jclass c,
                                                                        jint tid, jint ms) {
  (void)e; (void)c;
  ProfArgs* a = (ProfArgs*)malloc(sizeof(ProfArgs));
  a->tid = tid;
  a->ms = ms;
  pthread_t th;
  if (pthread_create(&th, 0, prof_thread, a) == 0) pthread_detach(th);
}
