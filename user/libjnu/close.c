#include <jnu_syscall.h>

int close(int fd) { return (int)jnu_syscall1(JNU_SYS_close, fd); }
