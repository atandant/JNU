#include <jnu_syscall.h>

/* v0.0.3 §2.2: the underlying syscall is sched_yield (Linux #24). */
int yield(void) { return (int)jnu_syscall0(JNU_SYS_sched_yield); }
