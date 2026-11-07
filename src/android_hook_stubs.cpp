// Hook targets that exist only in the title-update (TU2) image. The base image
// has no such functions, so the hooks that wrap them are never dispatched; these
// weak stubs only satisfy the linker.
#include <rex/ppc/func.h>

#define DI_MISSING_TARGET(sym)                                         \
  extern "C" __attribute__((weak)) REX_FUNC(sym) {                     \
    (void)ctx;                                                         \
    (void)base;                                                        \
  }

DI_MISSING_TARGET(__imp__sub_8252EDC0)
DI_MISSING_TARGET(__imp__sub_82629450)
DI_MISSING_TARGET(__imp__sub_82239AA8)
