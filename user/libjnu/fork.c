#include <jnu_syscall.h>

int fork(void) { return (int)jnu_syscall0(JNU_SYS_fork); }
